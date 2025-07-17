#include "../include/frame_queue.h"
#include "../include/logger.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

// --- Constantes ---
#define PUSH_TIMEOUT_MS 500  // Timeout para push (500ms)
#define POP_TIMEOUT_MS 2000  // Timeout para pop (2 segundos)

// --- Implementação das Funções ---

bool frame_queue_init(FrameQueue* queue, int capacity) {
    if (!queue) {
        log_message(LOG_LEVEL_ERROR, "[FrameQueue] Ponteiro de fila NULL");
        return false;
    }
    
    // Usar capacidade padrão se não especificada
    if (capacity <= 0) {
        capacity = FRAME_BUFFER_CAPACITY;
    }
    
    log_message(LOG_LEVEL_INFO, "[FrameQueue] Inicializando fila com capacidade %d", capacity);
    
    // Alocar array de ponteiros para frames
    queue->frames = (AVFrame**)malloc(capacity * sizeof(AVFrame*));
    if (!queue->frames) {
        log_message(LOG_LEVEL_ERROR, "[FrameQueue] Falha ao alocar array de frames");
        return false;
    }
    
    // Inicializar ponteiros como NULL
    memset(queue->frames, 0, capacity * sizeof(AVFrame*));
    
    // Inicializar campos da estrutura
    queue->capacity = capacity;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->frames_pushed = 0;
    queue->frames_popped = 0;
    queue->frames_dropped = 0;
    
    // Inicializar mutex e variáveis de condição
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        log_message(LOG_LEVEL_ERROR, "[FrameQueue] Falha ao inicializar mutex");
        free(queue->frames);
        queue->frames = NULL;
        return false;
    }
    
    if (pthread_cond_init(&queue->cond_not_empty, NULL) != 0) {
        log_message(LOG_LEVEL_ERROR, "[FrameQueue] Falha ao inicializar cond_not_empty");
        pthread_mutex_destroy(&queue->mutex);
        free(queue->frames);
        queue->frames = NULL;
        return false;
    }
    
    if (pthread_cond_init(&queue->cond_not_full, NULL) != 0) {
        log_message(LOG_LEVEL_ERROR, "[FrameQueue] Falha ao inicializar cond_not_full");
        pthread_cond_destroy(&queue->cond_not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->frames);
        queue->frames = NULL;
        return false;
    }
    
    log_message(LOG_LEVEL_INFO, "[FrameQueue] Fila inicializada com sucesso");
    return true;
}

void frame_queue_destroy(FrameQueue* queue) {
    if (!queue) {
        return;
    }
    
    log_message(LOG_LEVEL_INFO, "[FrameQueue] Destruindo fila...");
    
    // Bloquear mutex para operação segura
    pthread_mutex_lock(&queue->mutex);
    
    // Liberar todos os frames restantes na fila
    for (int i = 0; i < queue->capacity; i++) {
        if (queue->frames[i]) {
            av_frame_free(&queue->frames[i]);
        }
    }
    
    // Liberar array de ponteiros
    if (queue->frames) {
        free(queue->frames);
        queue->frames = NULL;
    }
    
    // Destruir mutex e variáveis de condição
    pthread_mutex_unlock(&queue->mutex);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_not_empty);
    pthread_cond_destroy(&queue->cond_not_full);
    
    // Zerar estrutura
    queue->capacity = 0;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;
    
    log_message(LOG_LEVEL_INFO, "[FrameQueue] Fila destruída. Stats: Pushed=%lu, Popped=%lu, Dropped=%lu", 
                queue->frames_pushed, queue->frames_popped, queue->frames_dropped);
}

bool frame_queue_push(FrameQueue* queue, AVFrame* frame, bool* stop_requested) {
    if (!queue || !frame) {
        log_message(LOG_LEVEL_WARNING, "[FrameQueue] Push com parâmetros inválidos");
        return false;
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    // Aguardar espaço na fila com timeout
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_nsec += PUSH_TIMEOUT_MS * 1000000; // Converter para nanosegundos
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec += timeout.tv_nsec / 1000000000;
        timeout.tv_nsec %= 1000000000;
    }
    
    while (queue->size >= queue->capacity && !(*stop_requested)) {
        int ret = pthread_cond_timedwait(&queue->cond_not_full, &queue->mutex, &timeout);
        if (ret == ETIMEDOUT) {
            // Log menos verboso - apenas a cada 10 frames descartados
            if (queue->frames_dropped % 10 == 0) {
                log_message(LOG_LEVEL_WARNING, "[FrameQueue] Timeout aguardando espaço na fila (cheia) - %lu frames descartados", queue->frames_dropped);
            }
            queue->frames_dropped++;
            pthread_mutex_unlock(&queue->mutex);
            return false;
        } else if (ret != 0) {
            log_message(LOG_LEVEL_ERROR, "[FrameQueue] Erro aguardando espaço: %s", strerror(ret));
            pthread_mutex_unlock(&queue->mutex);
            return false;
        }
    }
    
    // Verificar se foi solicitado para parar
    if (*stop_requested) {
        log_message(LOG_LEVEL_DEBUG, "[FrameQueue] Push cancelado por solicitação de parada");
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    
    // Alocar novo frame e copiar dados
    AVFrame* new_frame = av_frame_alloc();
    if (!new_frame) {
        log_message(LOG_LEVEL_ERROR, "[FrameQueue] Falha ao alocar novo frame");
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    
    // Copiar dados do frame original
    if (av_frame_ref(new_frame, frame) < 0) {
        log_message(LOG_LEVEL_ERROR, "[FrameQueue] Falha ao referenciar frame");
        av_frame_free(&new_frame);
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    
    // Adicionar à fila
    queue->frames[queue->tail] = new_frame;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;
    queue->frames_pushed++;
    
    log_message(LOG_LEVEL_TRACE, "[FrameQueue] Frame adicionado. Size=%d/%d", queue->size, queue->capacity);
    
    // Sinalizar que há frames disponíveis
    pthread_cond_signal(&queue->cond_not_empty);
    pthread_mutex_unlock(&queue->mutex);
    
    return true;
}

AVFrame* frame_queue_pop(FrameQueue* queue, bool* stop_requested) {
    if (!queue) {
        return NULL;
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    // Aguardar frames disponíveis com timeout
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_nsec += POP_TIMEOUT_MS * 1000000; // Converter para nanosegundos
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec += timeout.tv_nsec / 1000000000;
        timeout.tv_nsec %= 1000000000;
    }
    
    while (queue->size == 0 && !(*stop_requested)) {
        int ret = pthread_cond_timedwait(&queue->cond_not_empty, &queue->mutex, &timeout);
        if (ret == ETIMEDOUT) {
            // Log menos verboso - apenas ocasionalmente
            static int empty_timeout_count = 0;
            empty_timeout_count++;
            if (empty_timeout_count % 50 == 0) {
                log_message(LOG_LEVEL_TRACE, "[FrameQueue] Timeout aguardando frames (fila vazia) - %d timeouts", empty_timeout_count);
            }
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        } else if (ret != 0) {
            log_message(LOG_LEVEL_ERROR, "[FrameQueue] Erro aguardando frames: %s", strerror(ret));
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
    }
    
    // Verificar se foi solicitado para parar
    if (*stop_requested) {
        log_message(LOG_LEVEL_DEBUG, "[FrameQueue] Pop cancelado por solicitação de parada");
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }
    
    // Verificar se ainda há frames (pode ter sido consumido por outra thread)
    if (queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }
    
    // Remover frame da fila
    AVFrame* frame = queue->frames[queue->head];
    queue->frames[queue->head] = NULL;
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    queue->frames_popped++;
    
    log_message(LOG_LEVEL_TRACE, "[FrameQueue] Frame removido. Size=%d/%d", queue->size, queue->capacity);
    
    // Sinalizar que há espaço disponível
    pthread_cond_signal(&queue->cond_not_full);
    pthread_mutex_unlock(&queue->mutex);
    
    return frame;
}

void frame_queue_get_stats(FrameQueue* queue, int* size, int* capacity, uint64_t* dropped) {
    if (!queue) {
        if (size) *size = 0;
        if (capacity) *capacity = 0;
        if (dropped) *dropped = 0;
        return;
    }
    
    pthread_mutex_lock(&queue->mutex);
    if (size) *size = queue->size;
    if (capacity) *capacity = queue->capacity;
    if (dropped) *dropped = queue->frames_dropped;
    pthread_mutex_unlock(&queue->mutex);
}

bool frame_queue_is_empty(FrameQueue* queue) {
    if (!queue) return true;
    
    pthread_mutex_lock(&queue->mutex);
    bool empty = (queue->size == 0);
    pthread_mutex_unlock(&queue->mutex);
    return empty;
}

bool frame_queue_is_full(FrameQueue* queue) {
    if (!queue) return true;
    
    pthread_mutex_lock(&queue->mutex);
    bool full = (queue->size >= queue->capacity);
    pthread_mutex_unlock(&queue->mutex);
    return full;
} 