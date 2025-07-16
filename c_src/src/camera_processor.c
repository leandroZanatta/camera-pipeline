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

// MODIFICAÇÃO: Estrutura alterada para armazenar ponteiro para contexto
// ao invés da estrutura inteira, evitando problemas de reorganização da hash
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

// Nova função para interromper uma thread bloqueada
static void interrupt_camera_thread(pthread_t thread_id) {
    // Primeiro, tenta sinalizar via pipe
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

// Nova função para limpar dados antigos do pipe compartilhado
static void clear_interrupt_pipe() {
    if (g_interrupt_pipe[0] != -1) {
        // Ler e descartar todos os dados antigos do pipe
        char buf[256];
        int bytes_read;
        do {
            bytes_read = read(g_interrupt_pipe[0], buf, sizeof(buf));
        } while (bytes_read > 0);
        
        if (bytes_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            log_message(LOG_LEVEL_WARNING, "[Pipe Clear] Erro ao limpar pipe: %s", strerror(errno));
        } else {
            log_message(LOG_LEVEL_DEBUG, "[Pipe Clear] Pipe compartilhado limpo antes de adicionar nova câmera");
        }
    }
}

// Nova função para verificar se uma thread ainda está executando
static bool is_thread_running(pthread_t thread_id) {
    // Tentar join não-bloqueante para verificar se a thread ainda está ativa
    int result = pthread_tryjoin_np(thread_id, NULL);
    if (result == EBUSY) {
        return true; // Thread ainda executando
    } else if (result == 0) {
        return false; // Thread já terminou
    } else {
        // Erro - assumir que não está executando
        log_message(LOG_LEVEL_WARNING, "[Thread Check] Erro ao verificar thread: %s", strerror(result));
        return false;
    }
}

// Nova função para aguardar finalização de thread com timeout
static bool wait_for_thread_completion(pthread_t thread_id, int camera_id, int timeout_seconds) {
    log_message(LOG_LEVEL_INFO, "[Thread Wait] Aguardando finalização da thread anterior para câmera ID %d (timeout: %ds)...", camera_id, timeout_seconds);
    
    const int CHECK_INTERVAL_MS = 100; // 100ms
    const int MAX_ATTEMPTS = (timeout_seconds * 1000) / CHECK_INTERVAL_MS;
    
    struct timespec wait_time = {0, CHECK_INTERVAL_MS * 1000000}; // 100ms em nanosegundos
    
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        if (!is_thread_running(thread_id)) {
            log_message(LOG_LEVEL_INFO, "[Thread Wait] Thread anterior para câmera ID %d finalizada após %d tentativas.", camera_id, attempt + 1);
            return true; // Thread finalizada
        }
        
        nanosleep(&wait_time, NULL);
    }
    
    log_message(LOG_LEVEL_WARNING, "[Thread Wait] TIMEOUT: Thread anterior para câmera ID %d não finalizou em %d segundos.", camera_id, timeout_seconds);
    return false; // Timeout
}

// Função chamada pela thread da câmera para enviar um frame BGR processado
void send_frame_to_python(void* camera_context) {
    camera_thread_context_t* ctx = (camera_thread_context_t*)camera_context;
    if (!ctx) { // Verificação simplificada, active será checado depois
        log_message(LOG_LEVEL_WARNING, "[Send Frame] Contexto NULL recebido.");
        return;
    }
    
    // Bloquear para ler dados do contexto com segurança
    pthread_mutex_lock(&contexts_mutex);
    
    // MODIFICAÇÃO: Verificar se a câmera ainda existe na tabela hash
    camera_context_hash_t *hash_entry = find_context_by_id(ctx->camera_id);
    if (!hash_entry || !hash_entry->context || !hash_entry->context->active) {
        log_message(LOG_LEVEL_WARNING, "[Send Frame] Câmera ID %d não encontrada ou inativa.", ctx->camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return;
    }
    
    // MODIFICAÇÃO: Usar o ponteiro do contexto da hash para garantir consistência
    bool is_active = hash_entry->context->active;
    frame_callback_t cb = hash_entry->context->frame_cb;
    void* user_data = hash_entry->context->frame_cb_user_data;
    AVFrame* frame_to_send = hash_entry->context->frame_bgr;
    int cam_id = hash_entry->context->camera_id;
    int64_t pts = frame_to_send ? frame_to_send->pts : -1;
    
    pthread_mutex_unlock(&contexts_mutex);

    if (!is_active) {
        log_message(LOG_LEVEL_TRACE, "[Send Frame ID %d] Contexto inativo, frame descartado.", cam_id);
        return; // Não enviar se inativo
    }

    if (cb) {
        if (!frame_to_send || frame_to_send->width <= 0 || frame_to_send->height <= 0 || !frame_to_send->data[0]) {
            log_message(LOG_LEVEL_WARNING, "[Send Frame ID %d] Tentativa de enviar frame BGR inválido.", cam_id);
            return;
        }

        callback_frame_data_t* cb_data = callback_pool_get_data(frame_to_send, cam_id);
        
        if (cb_data) {
            // MODIFICAÇÃO: Log de debug detalhado para diagnóstico
            log_message(LOG_LEVEL_INFO, "[Send Frame ID BEFORE] Camera: %d, PTS: %ld, Address: %p", 
                     cam_id, pts, (void*)cb_data);
            log_message(LOG_LEVEL_INFO, "[Send Frame ID STRUCT] Campo ID na estrutura: %d, Width: %d, Height: %d", 
                     cb_data->camera_id, cb_data->width, cb_data->height);
            
            // MODIFICAÇÃO: Log para depuração que verifica consistência de IDs
            log_message(LOG_LEVEL_DEBUG, "[Send Frame ID %d] Enviando frame para Python (PTS: %ld, Camera ID estrutura: %d)", 
                     cam_id, pts, cb_data->camera_id);
            cb(cb_data, user_data);
        } else {
            log_message(LOG_LEVEL_ERROR, "[Send Frame ID %d] Falha ao obter dados de callback do pool.", cam_id);
        }
    } else {
        log_message(LOG_LEVEL_TRACE, "[Send Frame ID %d] Callback de frame não definido, frame descartado.", cam_id);
    }
}

// Marca um contexto como inativo 
// Deve ser chamado com contexts_mutex BLOQUEADO
static void mark_context_inactive(int camera_id) {
    camera_context_hash_t *context_entry = find_context_by_id(camera_id);
    if (context_entry && context_entry->context && context_entry->context->active) {
        context_entry->context->active = false;
        log_message(LOG_LEVEL_DEBUG, "[Context Mgr] ID %d marcado como inativo.", camera_id);
    } else {
        log_message(LOG_LEVEL_WARNING, "[Context Mgr] Tentativa de marcar ID inválido (%d) ou já inativo como inativo.", camera_id);
    }
}

// --- API Pública --- 

int processor_initialize(void) 
{
    pthread_mutex_lock(&contexts_mutex); // Usar o mutex correto
    if (g_processor_initialized) {
        log_message(LOG_LEVEL_WARNING, "[Processor API] Processador já inicializado.");
        pthread_mutex_unlock(&contexts_mutex);
        return 0;
    }
    
    // Inicializar hash de contextos
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Inicializando hash de contextos para IDs dinâmicos");
    g_camera_contexts = NULL; // A tabela hash começa vazia

    // MODIFICAÇÃO: Criar pipe para sinalização de interrupção
    if (pipe(g_interrupt_pipe) == -1) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao criar pipe para interrupção: %s", strerror(errno));
    } else {
        log_message(LOG_LEVEL_DEBUG, "[Processor API] Pipe de interrupção criado com sucesso");
    }

    // Inicializar rede FFmpeg 
    int net_ret = avformat_network_init();
    if (net_ret != 0) {
         log_ffmpeg_error(LOG_LEVEL_WARNING, "[Processor API] Falha ao inicializar rede FFmpeg", net_ret);
    }

    // Inicializar o pool de callbacks
    if (!callback_pool_initialize(0)) { // Usar tamanho padrão
         log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao inicializar o pool de callbacks!");
         // Limpar rede FFmpeg se falhar?
         avformat_network_deinit();
         
         // Fechar pipe de interrupção
         if (g_interrupt_pipe[0] != -1) close(g_interrupt_pipe[0]);
         if (g_interrupt_pipe[1] != -1) close(g_interrupt_pipe[1]);
         g_interrupt_pipe[0] = g_interrupt_pipe[1] = -1;
         
         pthread_mutex_unlock(&contexts_mutex);
         return -1; // Indicar erro na inicialização
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
        return -1; // Erro: Não inicializado
    }

    // 1. Validar URL
    if (!url || strlen(url) == 0) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] URL inválida fornecida para add_camera.");
        pthread_mutex_unlock(&contexts_mutex);
        return -3; // Erro: URL inválida
    }

    // 2. Verificar se o ID já está em uso
    camera_context_hash_t *context_entry = find_context_by_id(camera_id);
    if (context_entry) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Tentativa de adicionar câmera com ID %d que já está em uso.", camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return -4; // Erro: ID já em uso
    }
    
    // 3. Verificar se há uma thread anterior ainda executando (race condition)
    // Procurar por threads órfãs que podem estar usando o mesmo ID
    camera_context_hash_t *current, *tmp;
    pthread_t orphaned_thread = 0;
    bool found_orphaned_thread = false;
    
    HASH_ITER(hh, g_camera_contexts, current, tmp) {
        if (current->camera_id == camera_id && current->context && current->context->active) {
            // Encontrou uma thread ativa com o mesmo ID
            orphaned_thread = current->context->thread_id;
            found_orphaned_thread = true;
            log_message(LOG_LEVEL_WARNING, "[Processor API] Encontrada thread órfã para câmera ID %d. Aguardando finalização...", camera_id);
            break;
        }
    }
    
    // Se encontrou thread órfã, aguardar sua finalização
    if (found_orphaned_thread) {
        pthread_mutex_unlock(&contexts_mutex); // Desbloquear para permitir que a thread termine
        
        // Aguardar até 5 segundos para a thread anterior terminar
        if (!wait_for_thread_completion(orphaned_thread, camera_id, 5)) {
            log_message(LOG_LEVEL_ERROR, "[Processor API] Thread anterior para câmera ID %d não finalizou no tempo esperado. Abortando adição.", camera_id);
            return -7; // Erro: Thread anterior não finalizou
        }
        
        // Re-bloquear mutex para continuar
        pthread_mutex_lock(&contexts_mutex);
        
        // Verificar novamente se o ID ainda está em uso
        context_entry = find_context_by_id(camera_id);
        if (context_entry) {
            log_message(LOG_LEVEL_ERROR, "[Processor API] ID %d ainda em uso após aguardar thread anterior.", camera_id);
            pthread_mutex_unlock(&contexts_mutex);
            return -4; // Erro: ID ainda em uso
        }
    }

    // MODIFICAÇÃO: 4. Alocar contexto e entrada de hash separadamente
    // Primeiro alocamos o contexto
    camera_thread_context_t *ctx = (camera_thread_context_t*)malloc(sizeof(camera_thread_context_t));
    if (!ctx) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao alocar memória para contexto de câmera.");
        pthread_mutex_unlock(&contexts_mutex);
        return -5; // Erro: Falha de alocação
    }
    
    // Agora alocamos a entrada da hash
    context_entry = (camera_context_hash_t*)malloc(sizeof(camera_context_hash_t));
    if (!context_entry) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao alocar memória para entrada de hash.");
        free(ctx);
        pthread_mutex_unlock(&contexts_mutex);
        return -5; // Erro: Falha de alocação
    }

    // 5. Inicializar o contexto
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
    ctx->frame_skip_count = 1; // Default inicial
    
    // Configurar referência ao pipe de interrupção
    ctx->interrupt_read_fd = g_interrupt_pipe[0];
    
    // Inicializar a entrada da hash
    context_entry->camera_id = camera_id;
    context_entry->context = ctx;

    // 6. Adicionar à tabela hash
    HASH_ADD_INT(g_camera_contexts, camera_id, context_entry);
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Contexto para câmera ID %d adicionado à tabela hash.", camera_id);

    // 7. Limpar dados antigos do pipe compartilhado antes de criar a thread
    clear_interrupt_pipe();
    
    // 8. Criar a thread para este contexto
    log_message(LOG_LEVEL_INFO, "[Processor API] Criando thread para câmera ID %d (URL: %s)", camera_id, url);
    int rc = pthread_create(&ctx->thread_id, NULL, run_camera_loop, ctx);
    
    if (rc) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Erro ao criar thread para câmera ID %d: %d (%s)", camera_id, rc, strerror(rc));
        // Remover da tabela hash se a thread não pôde ser criada
        HASH_DEL(g_camera_contexts, context_entry);
        free(ctx);
        free(context_entry);
        pthread_mutex_unlock(&contexts_mutex);
        return -6; // Erro: Falha na criação da thread
    }

    log_message(LOG_LEVEL_DEBUG, "[Processor API] Thread para câmera ID %d criada com sucesso.", camera_id);
    pthread_mutex_unlock(&contexts_mutex);
    return 0; // Retorna 0 em sucesso
}

int processor_stop_camera(int camera_id) {
    pthread_mutex_lock(&contexts_mutex);
    if (!g_processor_initialized) {
         log_message(LOG_LEVEL_WARNING, "[Processor API] Processador não inicializado ao parar câmera ID %d.", camera_id);
         pthread_mutex_unlock(&contexts_mutex);
         return -1;
     }

    // Encontrar o contexto pelo ID
    camera_context_hash_t *context_entry = find_context_by_id(camera_id);
    if (!context_entry || !context_entry->context || !context_entry->context->active) {
        log_message(LOG_LEVEL_WARNING, "[Processor API] Tentativa de parar câmera inativa ou ID inválido (%d).", camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return -2; // ID inválido ou inativo
    }

    // Armazenar thread_id antes de marcar como stop_requested
    pthread_t thread_to_stop = context_entry->context->thread_id;
    
    log_message(LOG_LEVEL_INFO, "[Processor API] Solicitando parada da câmera ID %d...", camera_id);
    context_entry->context->stop_requested = true;
    
    // Interromper a thread se estiver bloqueada
    interrupt_camera_thread(thread_to_stop);
    
    pthread_mutex_unlock(&contexts_mutex);
    
    // MODIFICAÇÃO: Aguardar com TIMEOUT de segurança para evitar travamentos
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Aguardando finalização da thread para câmera ID %d (com timeout de segurança)...", camera_id);
    
    // Tentar join não-bloqueante primeiro
    int join_result = pthread_tryjoin_np(thread_to_stop, NULL);
    
    if (join_result == EBUSY) {
        // Thread ainda executando, aguardar com timeout limitado
        log_message(LOG_LEVEL_DEBUG, "[Processor API] Thread da câmera ID %d ainda executando, aguardando com timeout...", camera_id);
        
        // Aguardar até 3 segundos com verificações periódicas
        const int MAX_WAIT_SECONDS = 3;
        const int CHECK_INTERVAL_MS = 100; // 100ms
        const int MAX_ATTEMPTS = (MAX_WAIT_SECONDS * 1000) / CHECK_INTERVAL_MS;
        
        struct timespec wait_time = {0, CHECK_INTERVAL_MS * 1000000}; // 100ms em nanosegundos
        
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
        
        // Se ainda não finalizou após timeout, marcar como inativo mas NÃO bloquear
        if (join_result == EBUSY) {
            log_message(LOG_LEVEL_WARNING, "[Processor API] TIMEOUT: Thread da câmera ID %d não finalizou em %d segundos. Marcando como inativa mas mantendo thread.", 
                       camera_id, MAX_WAIT_SECONDS);
            
            // Marcar como inativo para liberar o ID, mas deixar a thread terminar em background
            pthread_mutex_lock(&contexts_mutex);
            mark_context_inactive(camera_id);
            pthread_mutex_unlock(&contexts_mutex);
            
            return 0; // Retorna sucesso mesmo com timeout para liberar o ID
        }
    }
    
    if (join_result != 0) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Erro ao aguardar thread da câmera ID %d: %s", 
                   camera_id, strerror(join_result));
        
        // Mesmo com erro, marcar como inativo para liberar o ID
        pthread_mutex_lock(&contexts_mutex);
        mark_context_inactive(camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        
        return -3; // Erro na finalização da thread
    }
    
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Thread da câmera ID %d finalizada com sucesso.", camera_id);
    
    // Remover COMPLETAMENTE da tabela hash após thread finalizada
    pthread_mutex_lock(&contexts_mutex);
    
    // Re-encontrar o contexto (garantir que ainda existe)
    context_entry = find_context_by_id(camera_id);
    if (context_entry && context_entry->context) {
        log_message(LOG_LEVEL_DEBUG, "[Processor API] Removendo câmera ID %d completamente da tabela hash.", camera_id);
        
        // Remover da tabela hash
        HASH_DEL(g_camera_contexts, context_entry);
        
        // Liberar memória do contexto
        free(context_entry->context);
        free(context_entry);
        
        log_message(LOG_LEVEL_INFO, "[Processor API] Câmera ID %d completamente removida e recursos liberados.", camera_id);
    } else {
        log_message(LOG_LEVEL_WARNING, "[Processor API] Contexto da câmera ID %d não encontrado durante limpeza.", camera_id);
    }
    
    pthread_mutex_unlock(&contexts_mutex);
    
    // Só retorna sucesso quando tudo foi finalizado e limpo (ou timeout controlado)
    return 0;
}

int processor_shutdown(void) {
    pthread_mutex_lock(&contexts_mutex);
    log_message(LOG_LEVEL_INFO, "[Processor API] Iniciando desligamento do processador...");
    
    if (!g_processor_initialized) {
         log_message(LOG_LEVEL_WARNING, "[Processor API] Processador já desligado ou não inicializado.");
         pthread_mutex_unlock(&contexts_mutex);
         return 0; // Ou -1?
    }

    // 1. Coletar todas as threads ativas para parada
    int active_count = 0;
    camera_context_hash_t *current, *tmp;
    typedef struct {
        pthread_t thread_id;
        int camera_id;
    } thread_to_join_t;
    
    // Estimar o número máximo de threads (contando todas as entradas na hash)
    int max_threads = HASH_COUNT(g_camera_contexts);
    thread_to_join_t *threads_to_join = NULL;
    
    if (max_threads > 0) {
        threads_to_join = (thread_to_join_t*)malloc(max_threads * sizeof(thread_to_join_t));
        if (!threads_to_join) {
            log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao alocar memória para lista de threads para join.");
            pthread_mutex_unlock(&contexts_mutex);
            return -1;
        }
    }

    // 2. Sinalizar todas as threads ativas para parar e coletar seus IDs
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Sinalizando parada para threads ativas...");
    HASH_ITER(hh, g_camera_contexts, current, tmp) {
        if (current->context && current->context->active) {
            log_message(LOG_LEVEL_DEBUG, "[Processor API] Sinalizando câmera ID %d.", current->camera_id);
            current->context->stop_requested = true;
            
            // MODIFICAÇÃO: Interromper cada thread diretamente
            interrupt_camera_thread(current->context->thread_id);
            
            if (threads_to_join) {
                threads_to_join[active_count].thread_id = current->context->thread_id;
                threads_to_join[active_count].camera_id = current->camera_id;
                active_count++;
            }
        }
    }
    log_message(LOG_LEVEL_INFO, "[Processor API] %d threads ativas sinalizadas para parada.", active_count);
    
    // Desbloquear mutex ANTES de fazer join para permitir que as threads terminem
    pthread_mutex_unlock(&contexts_mutex);

    // 3. Aguardar o término de todas as threads sinalizadas
    if (active_count > 0 && threads_to_join) {
        log_message(LOG_LEVEL_INFO, "[Processor API] Aguardando término das threads...");
        for (int i = 0; i < active_count; ++i) {
            log_message(LOG_LEVEL_DEBUG, "[Processor API] Aguardando join da thread para câmera ID %d...", threads_to_join[i].camera_id);
            
            // Tentar join com tempo limite usando método alternativo
            // Primeiro dá um tempo curto para a thread terminar naturalmente
            struct timespec wait_time = {0, 100000000}; // 100ms
            nanosleep(&wait_time, NULL);
            
            // Tenta join não bloqueante
            int rc = pthread_tryjoin_np(threads_to_join[i].thread_id, NULL);
            
            if (rc == EBUSY) {
                // Thread ainda em execução após tempo de espera, forçar encerramento
                log_message(LOG_LEVEL_WARNING, "[Processor API] Thread para câmera ID %d não terminou no tempo esperado. Forçando encerramento.", 
                            threads_to_join[i].camera_id);
                // Forçar encerramento da thread
                pthread_cancel(threads_to_join[i].thread_id);
                // Fazer join após cancel
                pthread_join(threads_to_join[i].thread_id, NULL);
            } else if (rc != 0) {
                log_message(LOG_LEVEL_ERROR, "[Processor API] Erro (%d: %s) ao aguardar join da thread para câmera ID %d.", 
                            rc, strerror(rc), threads_to_join[i].camera_id);
                // Continuar tentando fazer join das outras
            } else {
                log_message(LOG_LEVEL_DEBUG, "[Processor API] Join da thread para câmera ID %d concluído.", threads_to_join[i].camera_id);
            }
            
            // Marcar como inativo APÓS o join (bem-sucedido ou forçado)
            pthread_mutex_lock(&contexts_mutex);
            mark_context_inactive(threads_to_join[i].camera_id);
            pthread_mutex_unlock(&contexts_mutex);
        }
        free(threads_to_join);
        log_message(LOG_LEVEL_INFO, "[Processor API] Todas as threads ativas finalizaram.");
    }

    // 4. Limpar a tabela hash e liberar os recursos
    pthread_mutex_lock(&contexts_mutex);
    
    // Liberar cada entrada da hash e seu contexto associado
    HASH_ITER(hh, g_camera_contexts, current, tmp) {
        HASH_DEL(g_camera_contexts, current);
        if (current->context) {
            free(current->context); // Liberar o contexto alocado
        }
        free(current); // Liberar a entrada da hash
    }
    g_camera_contexts = NULL;
    
    // Fechar pipe de interrupção
    if (g_interrupt_pipe[0] != -1) close(g_interrupt_pipe[0]);
    if (g_interrupt_pipe[1] != -1) close(g_interrupt_pipe[1]);
    g_interrupt_pipe[0] = g_interrupt_pipe[1] = -1;
    
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

// Função pública para liberar dados do callback (REMOVIDA DAQUI)
// void processor_free_callback_data(callback_frame_data_t* frame_data) {
//     callback_utils_free_data(frame_data); // Delega para a função do utilitário
// } 