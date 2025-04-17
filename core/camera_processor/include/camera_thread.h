#ifndef CAMERA_THREAD_H
#define CAMERA_THREAD_H

#include <stdbool.h> 
#include "camera_processor.h" // Precisa dos tipos de callback

/**
 * @brief Inicia a thread de processamento da câmera.
 * 
 * @param url A URL do stream a ser processado.
 * @param status_cb Callback para notificações de status.
 * @param frame_cb Callback para entrega de frames.
 * @param status_cb_user_data Dados do usuário para o callback de status.
 * @param frame_cb_user_data Dados do usuário para o callback de frame.
 * @param target_fps Taxa de quadros alvo para o frame_cb (FPS). Se <= 0, usa 1 FPS.
 * @return bool true se a thread foi criada com sucesso, false caso contrário.
 */
bool camera_thread_start(const char* url, 
                         status_callback_t status_cb,
                         frame_callback_t frame_cb,
                         void* status_cb_user_data,
                         void* frame_cb_user_data,
                         int target_fps);

/**
 * @brief Sinaliza a thread da câmera para parar e aguarda sua finalização.
 * 
 * @return bool true se a thread foi parada com sucesso (ou já estava parada),
 *         false se houve erro ao aguardar ou a thread não estava rodando.
 */
bool camera_thread_stop_and_join();

/**
 * @brief Verifica se a thread da câmera está atualmente em execução.
 * 
 * @return bool true se a thread está rodando, false caso contrário.
 */
bool camera_thread_is_running();

#endif // CAMERA_THREAD_H 