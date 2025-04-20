// ==========================================================================
// camera_processor.c - API Pública e Gerenciamento de Múltiplas Câmeras
// ==========================================================================

#include "camera_processor.h" 
#include "logger.h"         
#include "callback_utils.h" 
#include "camera_thread.h" 
#include "camera_context.h" // <<< INCLUIR NOVO CABEÇALHO

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

// Array para armazenar os contextos das câmeras
static camera_thread_context_t camera_contexts[MAX_CAMERAS];

// Mutex para proteger o acesso ao array camera_contexts
static pthread_mutex_t contexts_mutex = PTHREAD_MUTEX_INITIALIZER;

// Flag global para indicar se o sistema está inicializado
static bool g_processor_initialized = false;

// --- Funções de Gerenciamento Interno de Contexto --- 

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

// Marca um slot como inativo 
// Deve ser chamado com contexts_mutex BLOQUEADO
static void mark_context_inactive(int camera_id) {
     if (camera_id >= 0 && camera_id < MAX_CAMERAS && camera_contexts[camera_id].active) {
         camera_contexts[camera_id].active = false;
         // g_active_thread_count--; // Decrementar quando a thread realmente terminar (no join)
         log_message(LOG_LEVEL_DEBUG, "[Context Mgr] Slot %d marcado como inativo.", camera_id);
         // Signal não necessário aqui se usarmos join loop
                          } else {
         log_message(LOG_LEVEL_WARNING, "[Context Mgr] Tentativa de marcar slot inválido (%d) ou já inativo como inativo.", camera_id);
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
    
    // Inicializar array de contextos
    log_message(LOG_LEVEL_DEBUG, "[Processor API] Inicializando array de contextos (MAX_CAMERAS=%d)", MAX_CAMERAS);
    memset(camera_contexts, 0, sizeof(camera_contexts)); // Zera todos os contextos
    for(int i=0; i<MAX_CAMERAS; ++i) {
        camera_contexts[i].active = false;
        camera_contexts[i].camera_id = -1; // Marcar ID inválido inicialmente
    }
    // g_active_thread_count = 0; // Zerar contador

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

    // 2. Validar camera_id
    if (camera_id < 0 || camera_id >= MAX_CAMERAS) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] ID de câmera inválido fornecido: %d (MAX: %d)", camera_id, MAX_CAMERAS - 1);
        pthread_mutex_unlock(&contexts_mutex);
        return -4; // Erro: ID inválido
    }

    // 3. Verificar se o slot já está ativo
    if (camera_contexts[camera_id].active) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Tentativa de adicionar câmera com ID %d que já está ativo.", camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return -4; // Erro: ID já em uso (ou reusar -4)
    }

    // 4. Inicializar o contexto no slot especificado
    log_message(LOG_LEVEL_DEBUG, "[Context Mgr] Usando slot fornecido: %d", camera_id);
    memset(&camera_contexts[camera_id], 0, sizeof(camera_thread_context_t));
    camera_contexts[camera_id].camera_id = camera_id; // Armazena o ID fornecido
    camera_contexts[camera_id].active = true;
    camera_contexts[camera_id].stop_requested = false;
    camera_contexts[camera_id].state = CAM_STATE_CONNECTING;
    strncpy(camera_contexts[camera_id].url, url, MAX_URL_LENGTH - 1);
    camera_contexts[camera_id].url[MAX_URL_LENGTH - 1] = '\0';
    camera_contexts[camera_id].status_cb = status_cb;
    camera_contexts[camera_id].frame_cb = frame_cb;
    camera_contexts[camera_id].status_cb_user_data = status_cb_user_data;
    camera_contexts[camera_id].frame_cb_user_data = frame_cb_user_data;
    camera_contexts[camera_id].target_fps = (target_fps <= 0) ? 1 : target_fps; // Ajustar target_fps
    camera_contexts[camera_id].video_stream_index = -1;
    camera_contexts[camera_id].reconnect_attempts = 0;
    camera_contexts[camera_id].frame_skip_count = 1; // Default inicial
    // Inicializar outros campos se necessário (pts, etc.)

    // 5. Criar a thread para este contexto específico
    log_message(LOG_LEVEL_INFO, "[Processor API] Criando thread para câmera ID %d (URL: %s)", camera_id, url);
    int rc = pthread_create(&camera_contexts[camera_id].thread_id, NULL, run_camera_loop, &camera_contexts[camera_id]);
    
    if (rc) {
        log_message(LOG_LEVEL_ERROR, "[Processor API] Erro ao criar thread para câmera ID %d: %d (%s)", camera_id, rc, strerror(rc));
        // Marcar como inativo se a thread não pôde ser criada
        mark_context_inactive(camera_id); 
        pthread_mutex_unlock(&contexts_mutex);
        return -5; // Erro: Falha na criação da thread
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

    if (camera_id < 0 || camera_id >= MAX_CAMERAS || !camera_contexts[camera_id].active) {
        log_message(LOG_LEVEL_WARNING, "[Processor API] Tentativa de parar câmera inativa ou ID inválido (%d).", camera_id);
        pthread_mutex_unlock(&contexts_mutex);
        return -2; // ID inválido ou inativo
    }

    log_message(LOG_LEVEL_INFO, "[Processor API] Solicitando parada da câmera ID %d...", camera_id);
    camera_contexts[camera_id].stop_requested = true;
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

    // 1. Sinalizar todas as threads ativas para parar
    int active_count = 0;
    pthread_t threads_to_join[MAX_CAMERAS];
    int ids_to_join[MAX_CAMERAS];

    log_message(LOG_LEVEL_DEBUG, "[Processor API] Sinalizando parada para threads ativas...");
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (camera_contexts[i].active) {
            log_message(LOG_LEVEL_DEBUG, "[Processor API] Sinalizando câmera ID %d.", i);
            camera_contexts[i].stop_requested = true;
            threads_to_join[active_count] = camera_contexts[i].thread_id;
            ids_to_join[active_count] = i;
            active_count++;
        }
    }
    log_message(LOG_LEVEL_INFO, "[Processor API] %d threads ativas sinalizadas para parada.", active_count);
    
    // Desbloquear mutex ANTES de fazer join para permitir que as threads terminem e atualizem seu estado
    pthread_mutex_unlock(&contexts_mutex);

    // 2. Aguardar o término de todas as threads sinalizadas
    log_message(LOG_LEVEL_INFO, "[Processor API] Aguardando término das threads...");
    for (int i = 0; i < active_count; ++i) {
        log_message(LOG_LEVEL_DEBUG, "[Processor API] Aguardando join da thread para câmera ID %d...", ids_to_join[i]);
        int rc = pthread_join(threads_to_join[i], NULL);
        if (rc) {
            log_message(LOG_LEVEL_ERROR, "[Processor API] Erro (%d: %s) ao aguardar join da thread para câmera ID %d.", rc, strerror(rc), ids_to_join[i]);
            // Continuar tentando fazer join das outras
        } else {
            log_message(LOG_LEVEL_DEBUG, "[Processor API] Join da thread para câmera ID %d concluído.", ids_to_join[i]);
            // Marcar como inativo APÓS o join bem-sucedido
            pthread_mutex_lock(&contexts_mutex);
            mark_context_inactive(ids_to_join[i]);
            pthread_mutex_unlock(&contexts_mutex);
        }
    }
    log_message(LOG_LEVEL_INFO, "[Processor API] Todas as threads ativas finalizaram.");

    // 3. Bloquear novamente para limpeza final
    pthread_mutex_lock(&contexts_mutex);
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