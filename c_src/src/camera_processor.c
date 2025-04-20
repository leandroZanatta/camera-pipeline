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

// Includes FFmpeg (completos para tipos da struct e init/deinit)
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h> 
#include <libavutil/pixfmt.h> 

// Função da thread (declarar aqui para ser usada por pthread_create)
extern void* run_camera_loop(void* arg);

// --- Variáveis Globais Estáticas (Nível do Processador) --- 

// Estrutura para manter os contextos de câmera em uma tabela hash
typedef struct {
    int camera_id;                    // ID da câmera (chave da hash)
    camera_thread_context_t context;  // Contexto da câmera
    UT_hash_handle hh;                // Tornando esta estrutura "hashable"
} camera_context_hash_t;

// Tabela hash para armazenar os contextos de câmera
static camera_context_hash_t *g_camera_contexts = NULL;

// Mutex para proteger o acesso à tabela hash de contextos
static pthread_mutex_t contexts_mutex = PTHREAD_MUTEX_INITIALIZER;

// Flag global para indicar se o sistema está inicializado
static bool g_processor_initialized = false;

// --- Funções de Gerenciamento Interno de Contexto --- 

// Função para encontrar um contexto pelo ID (deve ser chamada com mutex bloqueado)
static camera_context_hash_t* find_context_by_id(int camera_id) {
    camera_context_hash_t *context = NULL;
    HASH_FIND_INT(g_camera_contexts, &camera_id, context);
    return context;
}

// Função chamada pela thread da câmera para enviar um frame BGR processado
void send_frame_to_python(void* camera_context) {
    camera_thread_context_t* ctx = (camera_thread_context_t*)camera_context;
    if (!ctx) { // Verificação simplificada, active será checado depois
        log_message(LOG_LEVEL_WARNING, "[Send Frame] Contexto NULL recebido.");
        return;
    }
    
    // Bloquear para ler dados do contexto com segurança?
    // Provavelmente não necessário para frame_cb e user_data que são definidos uma vez.
    // Mas acessar 'active' pode precisar.
    pthread_mutex_lock(&contexts_mutex);
    bool is_active = ctx->active; // Ler estado ativo sob lock
    frame_callback_t cb = ctx->frame_cb;
    void* user_data = ctx->frame_cb_user_data;
    AVFrame* frame_to_send = ctx->frame_bgr;
    int cam_id = ctx->camera_id;
    int64_t pts = ctx ? ctx->frame_bgr->pts : -1; // Obter PTS antes de desbloquear
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
            log_message(LOG_LEVEL_TRACE, "[Send Frame ID %d] Enviando frame para Python (PTS: %ld)", cam_id, pts);
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
    if (context_entry && context_entry->context.active) {
        context_entry->context.active = false;
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

    // 3. Criar uma nova entrada na tabela hash
    context_entry = (camera_context_hash_t*)malloc(sizeof(camera_context_hash_t));
    if (!context_entry) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Falha ao alocar memória para novo contexto de câmera.");
        pthread_mutex_unlock(&contexts_mutex);
        return -5; // Erro: Falha de alocação
    }

    // 4. Inicializar o contexto
    context_entry->camera_id = camera_id;
    memset(&context_entry->context, 0, sizeof(camera_thread_context_t));
    context_entry->context.camera_id = camera_id;
    context_entry->context.active = true;
    context_entry->context.stop_requested = false;
    context_entry->context.state = CAM_STATE_CONNECTING;
    strncpy(context_entry->context.url, url, MAX_URL_LENGTH - 1);
    context_entry->context.url[MAX_URL_LENGTH - 1] = '\0';
    context_entry->context.status_cb = status_cb;
    context_entry->context.frame_cb = frame_cb;
    context_entry->context.status_cb_user_data = status_cb_user_data;
    context_entry->context.frame_cb_user_data = frame_cb_user_data;
    context_entry->context.target_fps = (target_fps <= 0) ? 1 : target_fps;
    context_entry->context.video_stream_index = -1;
    context_entry->context.reconnect_attempts = 0;
    context_entry->context.frame_skip_count = 1; // Default inicial

    // 5. Adicionar à tabela hash
    HASH_ADD_INT(g_camera_contexts, camera_id, context_entry);
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Contexto para câmera ID %d adicionado à tabela hash.", camera_id);

    // 6. Criar a thread para este contexto
    log_message(LOG_LEVEL_INFO, "[Processor API] Criando thread para câmera ID %d (URL: %s)", camera_id, url);
    int rc = pthread_create(&context_entry->context.thread_id, NULL, run_camera_loop, &context_entry->context);
    
    if (rc) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Erro ao criar thread para câmera ID %d: %d (%s)", camera_id, rc, strerror(rc));
        // Remover da tabela hash se a thread não pôde ser criada
        HASH_DEL(g_camera_contexts, context_entry);
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
    if (!context_entry || !context_entry->context.active) {
        log_message(LOG_LEVEL_WARNING, "[Processor API] Tentativa de parar câmera inativa ou ID inválido (%d).", camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return -2; // ID inválido ou inativo
    }

    log_message(LOG_LEVEL_INFO, "[Processor API] Solicitando parada da câmera ID %d...", camera_id);
    context_entry->context.stop_requested = true;
    // Não fazemos join aqui, apenas sinalizamos.
    // A thread deve verificar stop_requested e sair.
    // O join será feito em processor_shutdown.

    pthread_mutex_unlock(&contexts_mutex);
    return 0; // Solicitação enviada
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
        if (current->context.active) {
            log_message(LOG_LEVEL_DEBUG, "[Processor API] Sinalizando câmera ID %d.", current->camera_id);
            current->context.stop_requested = true;
            if (threads_to_join) {
                threads_to_join[active_count].thread_id = current->context.thread_id;
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
            int rc = pthread_join(threads_to_join[i].thread_id, NULL);
            if (rc) {
                log_message(LOG_LEVEL_ERROR, "[Processor API] Erro (%d: %s) ao aguardar join da thread para câmera ID %d.", 
                            rc, strerror(rc), threads_to_join[i].camera_id);
                // Continuar tentando fazer join das outras
            } else {
                log_message(LOG_LEVEL_DEBUG, "[Processor API] Join da thread para câmera ID %d concluído.", threads_to_join[i].camera_id);
                // Marcar como inativo APÓS o join bem-sucedido
                pthread_mutex_lock(&contexts_mutex);
                mark_context_inactive(threads_to_join[i].camera_id);
                pthread_mutex_unlock(&contexts_mutex);
            }
        }
        free(threads_to_join);
        log_message(LOG_LEVEL_INFO, "[Processor API] Todas as threads ativas finalizaram.");
    }

    // 4. Limpar a tabela hash e liberar os recursos
    pthread_mutex_lock(&contexts_mutex);
    
    // Liberar cada entrada da hash
    HASH_ITER(hh, g_camera_contexts, current, tmp) {
        HASH_DEL(g_camera_contexts, current);
        free(current);
    }
    g_camera_contexts = NULL;
    
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