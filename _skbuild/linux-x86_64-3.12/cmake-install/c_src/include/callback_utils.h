#ifndef CALLBACK_UTILS_H
#define CALLBACK_UTILS_H

#include <libavutil/frame.h> // Para AVFrame
#include <stdint.h>          // Para uint8_t
#include <stddef.h>          // Para size_t
#include <stdbool.h>         // Para bool

// --- Estrutura de Dados para Callback --- 

/**
 * @brief Estrutura para passar dados de frame decodificado (e convertido) para o Python.
 *        Esta definição agora reside aqui e não mais em camera_processor.h.
 *        Deve corresponder à definição em python_src/core/c_interface.py (CallbackFrameData).
 */
typedef struct {
    // Dimensões e formato do frame
    int width;
    int height;
    int format; // Esperado ser AV_PIX_FMT_BGR24 (ou outro formato acordado)
    int64_t pts; // Presentation timestamp

    // Campos adicionados para multi-câmera e gerenciamento
    int camera_id;
    int ref_count; 

    // Dados do frame (planar, mas BGR usa apenas o índice 0)
    uint8_t* data[4];
    int linesize[4];

    // Tamanho alocado para cada buffer de dados (usado internamente para free)
    size_t data_buffer_size[4]; 

} callback_frame_data_t;

// --- Funções Públicas --- 

/**
 * @brief Inicializa o pool global de estruturas callback_frame_data_t.
 *        Deve ser chamada uma vez durante a inicialização do processador.
 * 
 * @param pool_size O número de estruturas a serem pré-alocadas no pool.
 * @return true em sucesso, false em erro (falha de alocação).
 */
bool callback_pool_initialize(int pool_size);

/**
 * @brief Destroi o pool global, liberando toda a memória alocada.
 *        Deve ser chamada durante o desligamento do processador.
 */
void callback_pool_destroy();

/**
 * @brief Obtém uma estrutura callback_frame_data_t livre do pool e a preenche.
 *        Aloca o buffer interno para a cópia dos dados do frame.
 * 
 * @param src_frame O frame AVFrame original (geralmente BGR).
 * @param camera_id O ID da câmera associada a este frame.
 * @return callback_frame_data_t* Ponteiro para a estrutura do pool ou NULL se o pool estiver vazio ou erro.
 */
callback_frame_data_t* callback_pool_get_data(AVFrame* src_frame, int camera_id);

/**
 * @brief Retorna uma estrutura callback_frame_data_t para o pool.
 *        Libera o buffer de dados interno antes de retornar a estrutura ao pool.
 * 
 * @param data O ponteiro para a estrutura a ser retornada. Seguro chamar com NULL.
 */
void callback_pool_return_data(callback_frame_data_t* data);


#endif // CALLBACK_UTILS_H 