// ==========================================================================
// camera_processor.c - API Pública e Gerenciamento de Múltiplas Câmeras
// ==========================================================================

#include "camera_processor.h" 
#include "logger.h"         
#include "callback_utils.h" 
#include "camera_thread.h" 
#include "camera_context.h" 
#include "uthash.h" // Incluir a biblioteca uthash para tabela hash

#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <pthread.h>
#include <errno.h> 
#include <signal.h> // Para uso de sinais em threads
#include <unistd.h> // Para pipe()
#include <time.h>   // Para timeouts

#include <fcntl.h>

// Includes FFmpeg (completos para tipos da struct e init/deinit)
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h> 
#include <libavutil/pixfmt.h> 

// Definições de constantes
#define THREAD_JOIN_TIMEOUT_SEC 3  // Timeout para join de threads (segundos)
#define MAX_INTERRUPTION_WAIT_MS 500 // Tempo máximo para aguardar interrupção

// Função da thread (declarar aqui para ser usada por pthread_create)
extern void* run_camera_loop(void* arg);

// --- Variáveis Globais Estáticas (Nível do Processador) --- 

// Estrutura para armazenar ponteiro para contexto na hash
typedef struct {
    int camera_id;                      // ID da câmera (chave da hash)
    camera_thread_context_t *context;   // PONTEIRO para contexto da câmera
    UT_hash_handle hh;                  // Tornando esta estrutura "hashable"
} camera_context_hash_t;

// Tabela hash para armazenar os contextos de câmera
static camera_context_hash_t *g_camera_contexts = NULL;

// Mutex para proteger o acesso à tabela hash de contextos
static pthread_mutex_t contexts_mutex = PTHREAD_MUTEX_INITIALIZER;

// Flag global para indicar se o sistema está inicializado
static volatile bool g_processor_initialized = false;

// Pipe para interrupção de threads bloqueadas
static int g_interrupt_pipe[2] = {-1, -1};

// --- Funções de Gerenciamento Interno de Contexto --- 

// Função para encontrar um contexto pelo ID (deve ser chamada com mutex bloqueado)
static camera_context_hash_t* find_context_by_id(int camera_id) {
    camera_context_hash_t *context = NULL;
    HASH_FIND_INT(g_camera_contexts, &camera_id, context);
    return context;
}

// Função para interromper uma thread bloqueada
static void interrupt_camera_thread(pthread_t thread_id) {
    // Tenta sinalizar via pipe (se o pipe estiver aberto)
    if (g_interrupt_pipe[1] != -1) {
        char c = 'I'; // 'I' para interrupção
        if (write(g_interrupt_pipe[1], &c, 1) == -1) {
            log_message(LOG_LEVEL_ERROR, "[Thread Interrupt] Falha ao sinalizar via pipe: %s", strerror(errno));
        }
    }
    
    // Envia SIGUSR1 como backup para casos onde o pipe não funciona
    // Este sinal deve ser tratado na thread para terminar graciosamente
    if (pthread_kill(thread_id, SIGUSR1) != 0) {
        log_message(LOG_LEVEL_ERROR, "[Thread Interrupt] Falha ao enviar sinal para thread: %s", strerror(errno));
    }
}

// Função para limpar dados antigos do pipe compartilhado
static void clear_interrupt_pipe() {
    if (g_interrupt_pipe[0] != -1) {
        // Ler e descartar todos os dados antigos do pipe
        char buf[256];
        int bytes_read;
        // Definir pipe como não-bloqueante para leitura
        int flags = fcntl(g_interrupt_pipe[0], F_GETFL, 0);
        fcntl(g_interrupt_pipe[0], F_SETFL, flags | O_NONBLOCK);

        do {
            bytes_read = read(g_interrupt_pipe[0], buf, sizeof(buf));
        } while (bytes_read > 0);
        
        // Voltar o pipe para o modo bloqueante original (ou remover esta linha se quiser que continue non-blocking)
        fcntl(g_interrupt_pipe[0], F_SETFL, flags); // Restaura as flags originais

        if (bytes_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            log_message(LOG_LEVEL_WARNING, "[Pipe Clear] Erro ao limpar pipe: %s", strerror(errno));
        } else {
            log_message(LOG_LEVEL_DEBUG, "[Pipe Clear] Pipe compartilhado limpo antes de adicionar nova câmera");
        }
    }
}

// Função para verificar se uma thread ainda está executando (apenas para debug ou lógica específica)
static bool is_thread_running(pthread_t thread_id) {
    int result = pthread_tryjoin_np(thread_id, NULL);
    if (result == EBUSY) {
        return true; 
    } else if (result == 0) {
        return false;
    } else {
        log_message(LOG_LEVEL_WARNING, "[Thread Check] Erro ao verificar thread: %s", strerror(result));
        return false;
    }
}

// Função para aguardar finalização de thread com timeout
static bool wait_for_thread_completion(pthread_t thread_id, int camera_id, int timeout_seconds) {
    log_message(LOG_LEVEL_INFO, "[Thread Wait] Aguardando finalização da thread anterior para câmera ID %d (timeout: %ds)...", camera_id, timeout_seconds);
    
    const int CHECK_INTERVAL_MS = 100; // 100ms
    const int MAX_ATTEMPTS = (timeout_seconds * 1000) / CHECK_INTERVAL_MS;
    
    struct timespec wait_time = {0, CHECK_INTERVAL_MS * 1000000}; // 100ms em nanosegundos
    
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        if (!is_thread_running(thread_id)) {
            log_message(LOG_LEVEL_INFO, "[Thread Wait] Thread anterior para câmera ID %d finalizada após %d tentativas.", camera_id, attempt + 1);
            return true; 
        }
        
        nanosleep(&wait_time, NULL);
    }
    
    log_message(LOG_LEVEL_WARNING, "[Thread Wait] TIMEOUT: Thread anterior para câmera ID %d não finalizou em %d segundos.", camera_id, timeout_seconds);
    return false; // Timeout
}

// Função chamada pela thread da câmera para enviar um frame BGR processado
void send_frame_to_python(void* camera_context) {
    camera_thread_context_t* ctx_from_thread = (camera_thread_context_t*)camera_context;
    if (!ctx_from_thread) {
        log_message(LOG_LEVEL_WARNING, "[Send Frame] Contexto NULL recebido.");
        return;
    }
    
    // Bloquear para ler dados do contexto com segurança
    pthread_mutex_lock(&contexts_mutex);
    
    // Verificar se a câmera ainda existe na tabela hash e se é o mesmo contexto
    camera_context_hash_t *hash_entry = find_context_by_id(ctx_from_thread->camera_id);
    if (!hash_entry || hash_entry->context != ctx_from_thread || !hash_entry->context->active) {
        log_message(LOG_LEVEL_WARNING, "[Send Frame] Câmera ID %d não encontrada, contexto diferente ou inativa. Frame descartado.", ctx_from_thread->camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return;
    }
    
    // Usar o ponteiro do contexto da hash para garantir consistência
    bool is_active = hash_entry->context->active;
    frame_callback_t cb = hash_entry->context->frame_cb;
    void* user_data = hash_entry->context->frame_cb_user_data;
    AVFrame* frame_to_send = hash_entry->context->frame_bgr;
    int cam_id = hash_entry->context->camera_id;
    int64_t pts = frame_to_send ? frame_to_send->pts : -1;
    
    pthread_mutex_unlock(&contexts_mutex);

    if (!is_active) {
        log_message(LOG_LEVEL_TRACE, "[Send Frame ID %d] Contexto inativo, frame descartado.", cam_id);
        return; 
    }

    if (cb) {
        if (!frame_to_send || frame_to_send->width <= 0 || frame_to_send->height <= 0 || !frame_to_send->data[0]) {
            log_message(LOG_LEVEL_WARNING, "[Send Frame ID %d] Tentativa de enviar frame BGR inválido.", cam_id);
            return;
        }

        callback_frame_data_t* cb_data = callback_pool_get_data(frame_to_send, cam_id);
        
        if (cb_data) {
            log_message(LOG_LEVEL_INFO, "[Send Frame ID %d] Enviando frame para Python (PTS: %ld, Width: %d, Height: %d)", 
                     cam_id, pts, cb_data->width, cb_data->height);
            cb(cb_data, user_data);
        } else {
            log_message(LOG_LEVEL_ERROR, "[Send Frame ID %d] Falha ao obter dados de callback do pool.", cam_id);
        }
    } else {
        log_message(LOG_LEVEL_TRACE, "[Send Frame ID %d] Callback de frame não definido, frame descartado.", cam_id);
    }
}

// --- API Pública --- 

int processor_initialize(void) 
{
    pthread_mutex_lock(&contexts_mutex);
    if (g_processor_initialized) {
        log_message(LOG_LEVEL_WARNING, "[Processor API] Processador já inicializado.");
        pthread_mutex_unlock(&contexts_mutex);
        return 0;
    }
    
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Inicializando hash de contextos para IDs dinâmicos");
    g_camera_contexts = NULL; 

    // Criar pipe para sinalização de interrupção
    if (pipe(g_interrupt_pipe) == -1) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao criar pipe para interrupção: %s", strerror(errno));
        pthread_mutex_unlock(&contexts_mutex);
        return -1;
    } else {
        log_message(LOG_LEVEL_DEBUG, "[Processor API] Pipe de interrupção criado com sucesso");
    }

    // Inicializar rede FFmpeg 
    int net_ret = avformat_network_init();
    if (net_ret != 0) {
         log_ffmpeg_error(LOG_LEVEL_WARNING, "[Processor API] Falha ao inicializar rede FFmpeg", net_ret);
    }

    // Inicializar o pool de callbacks
    if (!callback_pool_initialize(0)) { 
         log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao inicializar o pool de callbacks!");
         avformat_network_deinit();
         if (g_interrupt_pipe[0] != -1) close(g_interrupt_pipe[0]);
         if (g_interrupt_pipe[1] != -1) close(g_interrupt_pipe[1]);
         g_interrupt_pipe[0] = g_interrupt_pipe[1] = -1;
         pthread_mutex_unlock(&contexts_mutex);
         return -1; 
    }
        
    g_processor_initialized = true;
    log_message(LOG_LEVEL_INFO, "[Processor API] Processador inicializado com sucesso.");
    pthread_mutex_unlock(&contexts_mutex);
    return 0;
}

int processor_add_camera(int camera_id,
                         const char* url,
                         status_callback_t status_cb,
                         frame_callback_t frame_cb,
                         void* status_cb_user_data,
                         void* frame_cb_user_data,
                         int target_fps)
{
    pthread_mutex_lock(&contexts_mutex);
    if (!g_processor_initialized) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Processador não inicializado ao adicionar câmera.");
        pthread_mutex_unlock(&contexts_mutex);
        return -1; 
    }

    // 1. Validar URL
    if (!url || strlen(url) == 0) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] URL inválida fornecida para add_camera.");
        pthread_mutex_unlock(&contexts_mutex);
        return -3; 
    }

    // 2. Verificar se o ID já está em uso na hash
    camera_context_hash_t *context_entry = find_context_by_id(camera_id);
    if (context_entry) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Tentativa de adicionar câmera com ID %d que já está em uso na tabela hash.", camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return -4; 
    }
    
    // 3. Alocar contexto e entrada de hash separadamente
    camera_thread_context_t *ctx = (camera_thread_context_t*)malloc(sizeof(camera_thread_context_t));
    if (!ctx) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao alocar memória para contexto de câmera.");
        pthread_mutex_unlock(&contexts_mutex);
        return -5; 
    }
    
    context_entry = (camera_context_hash_t*)malloc(sizeof(camera_context_hash_t));
    if (!context_entry) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao alocar memória para entrada de hash.");
        free(ctx);
        pthread_mutex_unlock(&contexts_mutex);
        return -5; 
    }

    // 4. Inicializar o contexto
    memset(ctx, 0, sizeof(camera_thread_context_t));
    ctx->camera_id = camera_id;
    ctx->active = true;
    ctx->stop_requested = false;
    ctx->state = CAM_STATE_CONNECTING;
    strncpy(ctx->url, url, MAX_URL_LENGTH - 1);
    ctx->url[MAX_URL_LENGTH - 1] = '\0';
    ctx->status_cb = status_cb;
    ctx->frame_cb = frame_cb;
    ctx->status_cb_user_data = status_cb_user_data;
    ctx->frame_cb_user_data = frame_cb_user_data;
    ctx->target_fps = (target_fps <= 0) ? 1 : target_fps;
    ctx->video_stream_index = -1;
    ctx->reconnect_attempts = 0;
    ctx->frame_skip_count = 1; 
    
    // Configurar referência ao pipe de interrupção
    ctx->interrupt_read_fd = g_interrupt_pipe[0];
    
    // Inicializar a entrada da hash
    context_entry->camera_id = camera_id;
    context_entry->context = ctx;

    // 5. Adicionar à tabela hash
    HASH_ADD_INT(g_camera_contexts, camera_id, context_entry);
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Contexto para câmera ID %d adicionado à tabela hash.", camera_id);

    // 6. Limpar dados antigos do pipe compartilhado antes de criar a thread
    clear_interrupt_pipe();
    
    // 7. Criar a thread para este contexto
    log_message(LOG_LEVEL_INFO, "[Processor API] Criando thread para câmera ID %d (URL: %s)", camera_id, url);
    int rc = pthread_create(&ctx->thread_id, NULL, run_camera_loop, ctx);
    
    if (rc) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Erro ao criar thread para câmera ID %d: %d (%s)", camera_id, rc, strerror(rc));
        // Remover da tabela hash se a thread não pôde ser criada
        HASH_DEL(g_camera_contexts, context_entry);
        free(ctx);
        free(context_entry);
        pthread_mutex_unlock(&contexts_mutex);
        return -6; 
    }

    log_message(LOG_LEVEL_DEBUG, "[Processor API] Thread para câmera ID %d criada com sucesso.", camera_id);
    pthread_mutex_unlock(&contexts_mutex);
    return 0; 
}

int processor_stop_camera(int camera_id) {
    pthread_mutex_lock(&contexts_mutex);
    if (!g_processor_initialized) {
         log_message(LOG_LEVEL_WARNING, "[Processor API] Processador não inicializado ao parar câmera ID %d.", camera_id);
         pthread_mutex_unlock(&contexts_mutex);
         return -1;
     }

    camera_context_hash_t *context_entry = find_context_by_id(camera_id);
    if (!context_entry || !context_entry->context) { 
        log_message(LOG_LEVEL_WARNING, "[Processor API] Tentativa de parar câmera ID %d não encontrada ou já em processo de parada.", camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return -2; 
    }

    // Armazenar thread_id e o ponteiro para o contexto ANTES de qualquer modificação
    pthread_t thread_to_stop = context_entry->context->thread_id;
    camera_thread_context_t *ctx_to_free = context_entry->context; 

    log_message(LOG_LEVEL_INFO, "[Processor API] Solicitando parada da câmera ID %d...", camera_id);
    // Sinalizar que a thread deve parar e está inativa
    ctx_to_free->stop_requested = true;
    ctx_to_free->active = false; 
    
    // Interromper a thread se estiver bloqueada
    interrupt_camera_thread(thread_to_stop);
    
    // REMOVER DA HASH IMEDIATAMENTE. Isso libera o ID para ser reutilizado.
    HASH_DEL(g_camera_contexts, context_entry);
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Câmera ID %d removida da tabela hash (liberado para reuso).", camera_id);
    
    pthread_mutex_unlock(&contexts_mutex); // Desbloquear mutex para permitir que a thread termine

    // Aguardar com TIMEOUT de segurança para evitar travamentos
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Aguardando finalização da thread para câmera ID %d (com timeout de segurança)...", camera_id);
    
    int join_result = pthread_tryjoin_np(thread_to_stop, NULL);
    
    if (join_result == EBUSY) {
        log_message(LOG_LEVEL_DEBUG, "[Processor API] Thread da câmera ID %d ainda executando, aguardando com timeout...", camera_id);
        
        const int MAX_WAIT_SECONDS = 3; 
        const int CHECK_INTERVAL_MS = 100; 
        const int MAX_ATTEMPTS = (MAX_WAIT_SECONDS * 1000) / CHECK_INTERVAL_MS;
        
        struct timespec wait_time = {0, CHECK_INTERVAL_MS * 1000000}; 
        
        for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
            nanosleep(&wait_time, NULL);
            
            join_result = pthread_tryjoin_np(thread_to_stop, NULL);
            if (join_result == 0) {
                log_message(LOG_LEVEL_DEBUG, "[Processor API] Thread da câmera ID %d finalizada após %d tentativas.", camera_id, attempt + 1);
                break;
            } else if (join_result != EBUSY) {
                log_message(LOG_LEVEL_ERROR, "[Processor API] Erro inesperado aguardando thread da câmera ID %d: %s", 
                           camera_id, strerror(join_result));
                break;
            }
        }
        
        if (join_result == EBUSY) {
            log_message(LOG_LEVEL_WARNING, "[Processor API] TIMEOUT: Thread da câmera ID %d não finalizou em %d segundos. Prosseguindo com liberação de recursos.", 
                       camera_id, MAX_WAIT_SECONDS);
        }
    }
    
    if (join_result != 0 && join_result != EBUSY) { 
        log_message(LOG_LEVEL_ERROR, "[Processor API] Erro ao aguardar thread da câmera ID %d: %s", 
                   camera_id, strerror(join_result));
    }
    
    // Liberar memória do contexto e da entrada da hash APÓS tentar o join
    if (ctx_to_free) {
        free(ctx_to_free);
        log_message(LOG_LEVEL_DEBUG, "[Processor API] Contexto da câmera ID %d liberado.", camera_id);
    }
    if (context_entry) { 
        free(context_entry);
        log_message(LOG_LEVEL_DEBUG, "[Processor API] Entrada da hash para câmera ID %d liberada.", camera_id);
    }

    log_message(LOG_LEVEL_INFO, "[Processor API] Câmera ID %d completamente parada e recursos liberados.", camera_id);
    
    return 0;
}

int processor_shutdown(void) {
    pthread_mutex_lock(&contexts_mutex);
    log_message(LOG_LEVEL_INFO, "[Processor API] Iniciando desligamento do processador...");
    
    if (!g_processor_initialized) {
         log_message(LOG_LEVEL_WARNING, "[Processor API] Processador já desligado ou não inicializado.");
         pthread_mutex_unlock(&contexts_mutex);
         return 0; 
    }

    // 1. Coletar todas as threads ativas para parada
    int active_count = 0;
    camera_context_hash_t *current, *tmp;
    typedef struct {
        pthread_t thread_id;
        int camera_id;
        camera_thread_context_t *context_ptr; // Para liberar depois
        camera_context_hash_t *hash_entry_ptr; // Para liberar depois
    } thread_info_to_process_t; // Renomeado para maior clareza
    
    // Estimar o número máximo de threads
    int max_threads = HASH_COUNT(g_camera_contexts);
    thread_info_to_process_t *threads_to_process = NULL; // Renomeado
    
    if (max_threads > 0) {
        threads_to_process = (thread_info_to_process_t*)malloc(max_threads * sizeof(thread_info_to_process_t));
        if (!threads_to_process) {
            log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao alocar memória para lista de threads para join.");
            pthread_mutex_unlock(&contexts_mutex);
            return -1;
        }
    }

    // 2. Sinalizar todas as threads ativas para parar, remover da hash e coletar seus IDs
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Sinalizando parada para threads ativas e removendo da hash...");
    HASH_ITER(hh, g_camera_contexts, current, tmp) {
        if (current->context) { // Sinaliza e remove mesmo se 'active' for falso, para limpar tudo
            log_message(LOG_LEVEL_DEBUG, "[Processor API] Sinalizando câmera ID %d para desligamento.", current->camera_id);
            current->context->stop_requested = true;
            current->context->active = false;
            
            interrupt_camera_thread(current->context->thread_id);
            
            if (threads_to_process) {
                threads_to_process[active_count].thread_id = current->context->thread_id;
                threads_to_process[active_count].camera_id = current->camera_id;
                threads_to_process[active_count].context_ptr = current->context;
                threads_to_process[active_count].hash_entry_ptr = current;
                active_count++;
            }
        }
    }
    log_message(LOG_LEVEL_INFO, "[Processor API] %d threads encontradas para desligamento.", active_count);

    // Limpar a tabela hash IMEDIATAMENTE (libera todos os IDs)
    // Depois iteramos sobre a lista 'threads_to_process' para fazer os joins e frees
    HASH_CLEAR(hh, g_camera_contexts);
    g_camera_contexts = NULL; // Garantir que a hash está vazia
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Tabela hash de contextos limpa.");
    
    // Desbloquear mutex ANTES de fazer join para permitir que as threads terminem
    pthread_mutex_unlock(&contexts_mutex);

    // 3. Aguardar o término de todas as threads sinalizadas e liberar recursos
    if (active_count > 0 && threads_to_process) {
        log_message(LOG_LEVEL_INFO, "[Processor API] Aguardando término e liberando recursos das threads...");
        for (int i = 0; i < active_count; ++i) {
            log_message(LOG_LEVEL_DEBUG, "[Processor API] Processando thread para câmera ID %d...", threads_to_process[i].camera_id);
            
            struct timespec wait_time = {0, 100000000}; // 100ms
            nanosleep(&wait_time, NULL);
            
            int rc = pthread_tryjoin_np(threads_to_process[i].thread_id, NULL);
            
            if (rc == EBUSY) {
                log_message(LOG_LEVEL_WARNING, "[Processor API] Thread para câmera ID %d não terminou no tempo esperado. Forçando encerramento.", 
                            threads_to_process[i].camera_id);
                pthread_cancel(threads_to_process[i].thread_id); // Forçar encerramento
                pthread_join(threads_to_process[i].thread_id, NULL); // Fazer join após cancel
                log_message(LOG_LEVEL_DEBUG, "[Processor API] Thread para câmera ID %d cancelada e join concluído.", threads_to_process[i].camera_id);
            } else if (rc != 0) {
                log_message(LOG_LEVEL_ERROR, "[Processor API] Erro (%d: %s) ao aguardar join da thread para câmera ID %d.", 
                            rc, strerror(rc), threads_to_process[i].camera_id);
            } else {
                log_message(LOG_LEVEL_DEBUG, "[Processor API] Join da thread para câmera ID %d concluído.", threads_to_process[i].camera_id);
            }
            
            // Liberar memória do contexto e da entrada da hash
            if (threads_to_process[i].context_ptr) {
                free(threads_to_process[i].context_ptr);
            }
            if (threads_to_process[i].hash_entry_ptr) {
                free(threads_to_process[i].hash_entry_ptr);
            }
            log_message(LOG_LEVEL_DEBUG, "[Processor API] Recursos da câmera ID %d liberados.", threads_to_process[i].camera_id);
        }
        free(threads_to_process);
        log_message(LOG_LEVEL_INFO, "[Processor API] Todas as threads processadas e recursos liberados.");
    }

    // 4. Limpezas finais
    pthread_mutex_lock(&contexts_mutex); // Re-bloquear para limpeza final
    
    // Fechar pipe de interrupção
    if (g_interrupt_pipe[0] != -1) {
        close(g_interrupt_pipe[0]);
        g_interrupt_pipe[0] = -1;
    }
    if (g_interrupt_pipe[1] != -1) {
        close(g_interrupt_pipe[1]);
        g_interrupt_pipe[1] = -1;
    }
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Pipe de interrupção fechado.");
    
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Desinicializando rede FFmpeg...");
    avformat_network_deinit(); 

    // Destruir o pool de callbacks
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Destruindo pool de callbacks...");
    callback_pool_destroy();

    g_processor_initialized = false;
    log_message(LOG_LEVEL_INFO, "[Processor API] Processador desligado.");
    pthread_mutex_unlock(&contexts_mutex);
    
    return 0;
}