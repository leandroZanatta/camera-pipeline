#ifndef CALLBACK_POOL_H
#define CALLBACK_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <libavcodec/avcodec.h>

// Estrutura que contém os dados do frame para callback
typedef struct {
    int camera_id;          // ID da câmera fonte
    int width;              // Largura do frame
    int height;             // Altura do frame
    int format;             // Formato de pixel (AVPixelFormat)
    int64_t pts;            // Presentation timestamp
    
    uint8_t* data[8];       // Ponteiros para os dados de cada plano
    int linesize[8];        // Linesize para cada plano
    size_t data_buffer_size[8]; // Tamanho em bytes de cada buffer
    
    int ref_count;          // Contador de referências para liberação de memória
} callback_frame_data_t;

// Tipos de callback suportados
typedef enum {
    CALLBACK_TYPE_FRAME,     // Frame completo processado
    CALLBACK_TYPE_STATUS,    // Status da câmera (conectada, desconectada, etc)
    // Outros tipos de callback podem ser adicionados aqui
} callback_type_t;

// Funções exportadas
bool callback_pool_init();
void callback_pool_shutdown();

// Registro e desregistro de callbacks
bool callback_pool_register(callback_type_t type, void* callback_func);
bool callback_pool_unregister(callback_type_t type, void* callback_func);

// Notificação de eventos (chamadas de callback)
void callback_pool_notify_frame(callback_frame_data_t* data);
void callback_pool_notify_status(int camera_id, int status_code, const char* message);

// Liberação de dados
void callback_pool_free_data(callback_frame_data_t* data);

// Função para depuração - imprime detalhes da estrutura callback_frame_data_t
void callback_pool_debug_frame_data(callback_frame_data_t* data, const char* context);

#endif /* CALLBACK_POOL_H */ 