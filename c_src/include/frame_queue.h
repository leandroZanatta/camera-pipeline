#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <libavutil/frame.h> // Para AVFrame

// --- Configuração da Capacidade do Buffer ---
#ifndef FRAME_BUFFER_CAPACITY
#define FRAME_BUFFER_CAPACITY 100 // Capacidade aumentada da fila de frames
#endif

// --- Estrutura da Fila Thread-Safe ---
typedef struct {
    AVFrame** frames;                    // Array de ponteiros para AVFrame
    int capacity;                        // Capacidade máxima da fila
    int size;                           // Número atual de frames na fila
    int head;                           // Índice do próximo frame a ser removido
    int tail;                           // Índice do próximo slot livre
    
    // --- Sincronização Thread-Safe ---
    pthread_mutex_t mutex;              // Mutex para proteger acesso concorrente
    pthread_cond_t cond_not_empty;      // Condição para sinalizar quando há frames
    pthread_cond_t cond_not_full;       // Condição para sinalizar quando há espaço
    
    // --- Estatísticas ---
    uint64_t frames_pushed;             // Total de frames adicionados
    uint64_t frames_popped;             // Total de frames removidos
    uint64_t frames_dropped;            // Frames descartados por fila cheia
} FrameQueue;

// --- Funções da API ---

/**
 * @brief Inicializa uma fila de frames thread-safe
 * 
 * @param queue Ponteiro para a fila a ser inicializada
 * @param capacity Capacidade máxima da fila (0 = usar padrão)
 * @return true em sucesso, false em erro
 */
bool frame_queue_init(FrameQueue* queue, int capacity);

/**
 * @brief Destroi uma fila de frames, liberando todos os recursos
 * 
 * @param queue Ponteiro para a fila a ser destruída
 */
void frame_queue_destroy(FrameQueue* queue);

/**
 * @brief Adiciona um frame à fila (thread-safe)
 * 
 * @param queue Ponteiro para a fila
 * @param frame Frame a ser adicionado (será copiado)
 * @param stop_requested Ponteiro para flag de parada (para timeout)
 * @return true se frame foi adicionado, false se fila cheia ou erro
 */
bool frame_queue_push(FrameQueue* queue, AVFrame* frame, bool* stop_requested);

/**
 * @brief Remove um frame da fila (thread-safe)
 * 
 * @param queue Ponteiro para a fila
 * @param stop_requested Ponteiro para flag de parada (para timeout)
 * @return AVFrame* ponteiro para o frame removido, ou NULL se fila vazia
 */
AVFrame* frame_queue_pop(FrameQueue* queue, bool* stop_requested);

/**
 * @brief Obtém estatísticas da fila (thread-safe)
 * 
 * @param queue Ponteiro para a fila
 * @param size Ponteiro para receber o tamanho atual
 * @param capacity Ponteiro para receber a capacidade
 * @param dropped Ponteiro para receber frames descartados
 */
void frame_queue_get_stats(FrameQueue* queue, int* size, int* capacity, uint64_t* dropped);

/**
 * @brief Verifica se a fila está vazia (thread-safe)
 * 
 * @param queue Ponteiro para a fila
 * @return true se vazia, false caso contrário
 */
bool frame_queue_is_empty(FrameQueue* queue);

/**
 * @brief Verifica se a fila está cheia (thread-safe)
 * 
 * @param queue Ponteiro para a fila
 * @return true se cheia, false caso contrário
 */
bool frame_queue_is_full(FrameQueue* queue);

#endif // FRAME_QUEUE_H 