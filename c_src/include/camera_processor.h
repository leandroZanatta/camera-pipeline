#ifndef CAMERA_PROCESSOR_H
#define CAMERA_PROCESSOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Inclui definições comuns (estrutura de callback, tipos de callback)
#include "callback_utils.h"

// --- Constantes --- 
#ifndef MAX_CAMERAS
#define MAX_CAMERAS 64 // Define um limite máximo razoável
#endif

#ifndef MAX_URL_LENGTH
#define MAX_URL_LENGTH 1024 
#endif

// --- Tipos de Callback (Python -> C) --- 
// Mantidos aqui pois fazem parte da API do processador

/**
 * @brief Callback para notificar o status da câmera.
 * 
 * @param camera_id ID da câmera (sempre 0 na versão atual).
 * @param status_code Código numérico do status (ver enum camera_state_t interno).
 * @param message Mensagem descritiva do status.
 * @param user_data Ponteiro opaco passado em processor_add_camera.
 */
typedef void (*status_callback_t)(int camera_id, int status_code, const char* message, void* user_data);

/**
 * @brief Callback para entregar um frame processado.
 * 
 * @param frame_data Ponteiro para a estrutura com os dados do frame.
 *                   A implementação Python DEVE chamar processor_free_callback_data() 
 *                   neste ponteiro quando terminar de usá-lo.
 * @param user_data Ponteiro opaco passado em processor_initialize.
 */
typedef void (*frame_callback_t)(callback_frame_data_t* frame_data, void* user_data);

// --- Funções da API Pública --- 

/**
 * @brief Inicializa o processador globalmente (rede FFmpeg, etc.).
 *        Deve ser chamada uma vez antes de usar outras funções.
 * 
 * @return int 0 em sucesso, < 0 em erro.
 */
int processor_initialize(void);

/**
 * @brief Adiciona e inicia UMA câmera.
 * 
 * @param url URL do stream da câmera.
 * @param status_cb Callback para notificações de status.
 * @param frame_cb Callback para entrega de frames.
 * @param status_cb_user_data Ponteiro opaco para ser passado ao status_cb.
 * @param frame_cb_user_data Ponteiro opaco para ser passado ao frame_cb.
 * @param target_fps Taxa de quadros alvo para o frame_cb (FPS). Se <= 0, usa 1 FPS.
 * @return int O ID da câmera (>= 0) em sucesso, < 0 em erro.
 *         Possíveis erros: -1 (não inicializado), -2 (sem slots), -3 (URL inválida), -5 (erro thread).
 */
int processor_add_camera(const char* url, 
                         status_callback_t status_cb, 
                         frame_callback_t frame_cb, 
                         void* status_cb_user_data, 
                         void* frame_cb_user_data,
                         int target_fps);

/**
 * @brief Solicita a parada de UMA câmera específica.
 *        Esta função apenas sinaliza a parada e retorna imediatamente.
 *        O término real da thread ocorrerá em background.
 * 
 * @param camera_id O ID da câmera retornado por processor_add_camera.
 * @return int 0 se a solicitação foi enviada, < 0 se erro (ID inválido, não inicializado, etc).
 */
int processor_stop_camera(int camera_id);

/**
 * @brief Desliga completamente o processador, parando a câmera se necessário.
 * 
 * @return int 0 em sucesso.
 */
int processor_shutdown(void);

#endif // CAMERA_PROCESSOR_H 