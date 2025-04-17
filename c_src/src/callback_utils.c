#include "../include/callback_utils.h"
#include "../include/logger.h" // Para logar erros internos
#include "../include/camera_processor.h" // Para MAX_CAMERAS

#include <stdlib.h> // Para malloc, free
#include <string.h> // Para memset, memcpy
#include <pthread.h> // Para mutex
#include <stdbool.h>
#include <libavutil/imgutils.h> // Para av_image_get_buffer_size
#include <libavcodec/avcodec.h> // Para AV_PIX_FMT_BGR24 (idealmente viria de um header comum)
#include <libavutil/frame.h>   // Para AVFrame

// Define o formato esperado aqui, se necessário (ou incluir de outro lugar)
// Poderia vir de um config.h compartilhado no futuro.
#ifndef AV_PIX_FMT_BGR24
#define AV_PIX_FMT_BGR24 3 // Garante que temos o valor correto
#endif

// --- Constantes e Variáveis Globais do Pool ---
#define DEFAULT_POOL_SIZE (MAX_CAMERAS * 4) // Pool inicial pode ser ~4x o max de câmeras

static callback_frame_data_t* g_callback_pool = NULL; // Ponteiro para o array de structs
static int* g_pool_indices = NULL; // Array de índices para os itens livres
static int g_pool_size = 0;
static int g_pool_free_count = 0;
static pthread_mutex_t g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_pool_initialized = false;

// --- Implementação das Funções Públicas --- 

bool callback_pool_initialize(int pool_size) {
    pthread_mutex_lock(&g_pool_mutex);
    if (g_pool_initialized) {
        log_message(LOG_LEVEL_WARNING, "[Callback Pool] Pool já inicializado.");
        pthread_mutex_unlock(&g_pool_mutex);
        return true;
    }

    g_pool_size = (pool_size > 0) ? pool_size : DEFAULT_POOL_SIZE;
    log_message(LOG_LEVEL_INFO, "[Callback Pool] Inicializando com tamanho: %d", g_pool_size);

    // Alocar o array principal de estruturas
    g_callback_pool = (callback_frame_data_t*)malloc(g_pool_size * sizeof(callback_frame_data_t));
    if (!g_callback_pool) {
        log_message(LOG_LEVEL_ERROR, "[Callback Pool] Falha ao alocar pool principal.");
        g_pool_size = 0;
        pthread_mutex_unlock(&g_pool_mutex);
        return false;
    }
    memset(g_callback_pool, 0, g_pool_size * sizeof(callback_frame_data_t));

    // Alocar o array de índices de itens livres
    g_pool_indices = (int*)malloc(g_pool_size * sizeof(int));
    if (!g_pool_indices) {
        log_message(LOG_LEVEL_ERROR, "[Callback Pool] Falha ao alocar array de índices.");
        free(g_callback_pool);
        g_callback_pool = NULL;
        g_pool_size = 0;
        pthread_mutex_unlock(&g_pool_mutex);
        return false;
    }

    // Preencher o array de índices (0, 1, 2, ...)
    for (int i = 0; i < g_pool_size; ++i) {
        g_pool_indices[i] = i;
        g_callback_pool[i].ref_count = 0; // Marcar como livre inicialmente
    }
    g_pool_free_count = g_pool_size;
    g_pool_initialized = true;

    log_message(LOG_LEVEL_INFO, "[Callback Pool] Inicializado com sucesso.");
    pthread_mutex_unlock(&g_pool_mutex);
    return true;
}

void callback_pool_destroy() {
    pthread_mutex_lock(&g_pool_mutex);
    if (!g_pool_initialized) {
        log_message(LOG_LEVEL_WARNING, "[Callback Pool] Tentativa de destruir pool não inicializado.");
        pthread_mutex_unlock(&g_pool_mutex);
        return;
    }

    log_message(LOG_LEVEL_INFO, "[Callback Pool] Destruindo pool...");

    // Liberar buffers internos que possam ter vazado (embora não devesse acontecer)
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_callback_pool[i].data[0]) {
            log_message(LOG_LEVEL_WARNING, "[Callback Pool] Buffer interno (%d) não retornado, liberando...", i);
            free(g_callback_pool[i].data[0]);
        }
    }

    free(g_callback_pool);
    g_callback_pool = NULL;
    free(g_pool_indices);
    g_pool_indices = NULL;
    g_pool_size = 0;
    g_pool_free_count = 0;
    g_pool_initialized = false;

    log_message(LOG_LEVEL_INFO, "[Callback Pool] Destruído.");
    pthread_mutex_unlock(&g_pool_mutex); // Desbloquear antes de destruir o mutex?
    // pthread_mutex_destroy(&g_pool_mutex); // Opcional, se for estático não precisa
}

callback_frame_data_t* callback_pool_get_data(AVFrame* src_frame, int camera_id) {
    if (!g_pool_initialized) {
        log_message(LOG_LEVEL_ERROR, "[Callback Pool] Pool não inicializado ao tentar obter dados.");
        return NULL;
    }
    if (!src_frame) {
        log_message(LOG_LEVEL_ERROR, "[Callback Pool] Tentativa de obter dados a partir de AVFrame nulo.");
        return NULL;
    }
    if (src_frame->format != AV_PIX_FMT_BGR24 || src_frame->width <= 0 || src_frame->height <= 0) {
         log_message(LOG_LEVEL_WARNING, "[Callback Pool] AVFrame inválido (formato/dims) fornecido.");
         return NULL;
    }

    callback_frame_data_t* cb_data = NULL;
    int data_index = -1;

    // --- Obter item do pool --- 
    pthread_mutex_lock(&g_pool_mutex);
    if (g_pool_free_count > 0) {
        g_pool_free_count--;
        data_index = g_pool_indices[g_pool_free_count]; // Pega o último índice livre
        cb_data = &g_callback_pool[data_index];
        cb_data->ref_count = 1; // Marcar como em uso
        // log_message(LOG_LEVEL_TRACE, "[Callback Pool] Item %d obtido do pool.", data_index);
    } else {
        log_message(LOG_LEVEL_WARNING, "[Callback Pool] Pool vazio! Não foi possível obter estrutura.");
        // Poderíamos alocar um novo temporário aqui ou apenas retornar NULL
        // Retornar NULL é mais seguro para evitar alocações fora do pool
    }
    pthread_mutex_unlock(&g_pool_mutex);

    if (!cb_data) {
        return NULL; // Pool estava vazio
    }

    // --- Preencher dados (Fora do Lock) --- 
    // Limpar ponteiros de dados antigos e tamanhos (segurança)
    memset(cb_data->data, 0, sizeof(cb_data->data));
    memset(cb_data->linesize, 0, sizeof(cb_data->linesize));
    memset(cb_data->data_buffer_size, 0, sizeof(cb_data->data_buffer_size));

    // Preencher metadados
    cb_data->width = src_frame->width;
    cb_data->height = src_frame->height;
    cb_data->format = src_frame->format;
    cb_data->pts = src_frame->pts;
    cb_data->camera_id = camera_id;
    // cb_data->ref_count já é 1

    // --- Alocação e Cópia do Buffer BGR (Plano 0) ---
    int plane_index = 0;
    if (!src_frame->data[plane_index] || src_frame->linesize[plane_index] <= 0) {
        log_message(LOG_LEVEL_ERROR, "[Callback Pool Item %d] Plano de dados BGR inválido no AVFrame.", data_index);
        callback_pool_return_data(cb_data); // Devolver ao pool em caso de erro
        return NULL;
    }

    cb_data->data_buffer_size[plane_index] = av_image_get_buffer_size(
        (enum AVPixelFormat)cb_data->format, 
        cb_data->width, 
        cb_data->height, 
        1 // Alinhamento
    );

    if (cb_data->data_buffer_size[plane_index] <= 0) {
        log_message(LOG_LEVEL_ERROR, "[Callback Pool Item %d] Tamanho de buffer BGR inválido (%zu).", data_index, cb_data->data_buffer_size[plane_index]);
        callback_pool_return_data(cb_data); 
        return NULL;
    }

    // Alocar memória para a CÓPIA
    cb_data->data[plane_index] = (uint8_t*)malloc(cb_data->data_buffer_size[plane_index]);
    if (!cb_data->data[plane_index]) {
        log_message(LOG_LEVEL_ERROR, "[Callback Pool Item %d] Falha ao alocar %zu bytes para cópia BGR.", data_index, cb_data->data_buffer_size[plane_index]);
        callback_pool_return_data(cb_data); 
        return NULL;
    }
    
    cb_data->linesize[plane_index] = cb_data->width * 3;
    
    // Copiar dados
    int bytes_per_line_src = src_frame->linesize[plane_index];
    int bytes_per_line_dst = cb_data->linesize[plane_index];
    int bytes_to_copy_per_line = cb_data->width * 3;
    
    if (bytes_per_line_src == bytes_per_line_dst && 
        (size_t)bytes_per_line_src * cb_data->height == cb_data->data_buffer_size[plane_index]) 
    {
         memcpy(cb_data->data[plane_index], src_frame->data[plane_index], cb_data->data_buffer_size[plane_index]);
    } else {
        for (int y = 0; y < cb_data->height; ++y) {
            memcpy(cb_data->data[plane_index] + y * bytes_per_line_dst,
                   src_frame->data[plane_index] + y * bytes_per_line_src,
                   bytes_to_copy_per_line);
        }
    }

    // log_message(LOG_LEVEL_TRACE, "[Callback Pool] Item %d preenchido e pronto.", data_index);
    return cb_data;
}

void callback_pool_return_data(callback_frame_data_t* data) {
    if (!g_pool_initialized) {
        // Se pool não inicializado, apenas libera memória se existir
        if (data && data->data[0]) free(data->data[0]);
        // Não tentar liberar a estrutura 'data' pois não veio do pool
        return; 
    }
    if (!data) {
        return; // Seguro chamar com NULL
    }

    // Verificar se este ponteiro realmente pertence ao nosso pool (verificação opcional)
    // int data_index = data - g_callback_pool; // Aritmética de ponteiros
    // if (data_index < 0 || data_index >= g_pool_size) {
    //     log_message(LOG_LEVEL_ERROR, "[Callback Pool] Tentativa de retornar ponteiro inválido ao pool!");
    //     // Liberar buffer interno por segurança, mas não retornar ao pool
    //     if(data->data[0]) free(data->data[0]);
    //     return;
    // }
    // if (data->ref_count == 0) {
    //     log_message(LOG_LEVEL_WARNING, "[Callback Pool] Tentativa de retornar item (%d) já livre!", data_index);
    //     return; // Já está no pool
    // }

    // --- Liberar recursos internos (buffer de imagem) --- 
    int plane_index = 0;
    if (data->data[plane_index]) {
        // log_message(LOG_LEVEL_TRACE, "[Callback Pool] Liberando buffer interno do item %ld.", data - g_callback_pool);
        free(data->data[plane_index]);
        data->data[plane_index] = NULL;
        data->linesize[plane_index] = 0;
        data->data_buffer_size[plane_index] = 0;
    }
    // Limpar outros campos por segurança?
    data->pts = 0;
    data->width = 0;
    data->height = 0;

    // --- Retornar ao pool --- 
    pthread_mutex_lock(&g_pool_mutex);
    if (g_pool_free_count < g_pool_size) {
        int data_index = data - g_callback_pool; // Calcular índice
        g_pool_indices[g_pool_free_count] = data_index; // Adiciona índice à lista de livres
        g_pool_free_count++;
        data->ref_count = 0; // Marcar como livre
        // log_message(LOG_LEVEL_TRACE, "[Callback Pool] Item %d retornado ao pool.", data_index);
    } else {
        // Isso não deveria acontecer se a lógica estiver correta
        log_message(LOG_LEVEL_ERROR, "[Callback Pool] Tentativa de retornar item para pool cheio!");
    }
    pthread_mutex_unlock(&g_pool_mutex);
}

callback_frame_data_t* callback_utils_create_data(AVFrame* src_frame, int camera_id) {
    if (!src_frame) {
        log_message(LOG_LEVEL_ERROR, "[Callback Utils] Tentativa de criar dados a partir de AVFrame nulo.");
        return NULL;
    }

    // Verificar se o formato é o esperado (BGR24)
    if (src_frame->format != AV_PIX_FMT_BGR24) {
         log_message(LOG_LEVEL_WARNING, "[Callback Utils] AVFrame não está no formato BGR24 esperado (formato: %d).", src_frame->format);
         // Decidir se retorna NULL ou tenta processar mesmo assim?
         // Retornar NULL é mais seguro por enquanto.
         return NULL;
    }
    if (src_frame->width <= 0 || src_frame->height <= 0) {
         log_message(LOG_LEVEL_WARNING, "[Callback Utils] AVFrame com dimensões inválidas (W: %d, H: %d).", src_frame->width, src_frame->height);
         return NULL;
    }

    // Alocar a estrutura principal
    callback_frame_data_t* cb_data = (callback_frame_data_t*)malloc(sizeof(callback_frame_data_t));
    if (!cb_data) {
        log_message(LOG_LEVEL_ERROR, "[Callback Utils] Falha ao alocar memória para callback_frame_data_t.");
        return NULL;
    }
    memset(cb_data, 0, sizeof(callback_frame_data_t)); // Zera toda a estrutura

    // Preencher metadados
    cb_data->width = src_frame->width;
    cb_data->height = src_frame->height;
    cb_data->format = src_frame->format; // AV_PIX_FMT_BGR24
    cb_data->pts = src_frame->pts;
    cb_data->camera_id = camera_id;
    cb_data->ref_count = 1;

    // --- Cópia dos Dados do Buffer (Plano BGR) ---
    int plane_index = 0; // BGR é "planar" mas usa apenas o plano 0
    
    // Validar ponteiro e linesize de origem
    if (!src_frame->data[plane_index] || src_frame->linesize[plane_index] <= 0) {
        log_message(LOG_LEVEL_ERROR, "[Callback Utils] Plano de dados BGR (índice %d) do AVFrame de origem é inválido.", plane_index);
        free(cb_data); // Liberar estrutura principal alocada
        return NULL;
    }
    
    // Calcular tamanho necessário para o buffer de destino
    // Para BGR24, a alinhamento padrão é 1
    cb_data->data_buffer_size[plane_index] = av_image_get_buffer_size(
        (enum AVPixelFormat)cb_data->format, 
        cb_data->width, 
        cb_data->height, 
        1 // Alinhamento (alignment) - 1 para BGR geralmente
    );

    if (cb_data->data_buffer_size[plane_index] <= 0) {
        log_message(LOG_LEVEL_ERROR, "[Callback Utils] Tamanho calculado para buffer BGR é inválido (%zu).", cb_data->data_buffer_size[plane_index]);
        free(cb_data);
        return NULL;
    }

    // Alocar memória para a CÓPIA do buffer
    cb_data->data[plane_index] = (uint8_t*)malloc(cb_data->data_buffer_size[plane_index]);
    if (!cb_data->data[plane_index]) {
        log_message(LOG_LEVEL_ERROR, "[Callback Utils] Falha ao alocar %zu bytes para cópia do plano BGR.", cb_data->data_buffer_size[plane_index]);
        free(cb_data);
        return NULL;
    }
    
    // Definir o linesize de destino (para BGR é width * 3)
    cb_data->linesize[plane_index] = cb_data->width * 3;
    
    // Copiar os dados do AVFrame para o novo buffer
    int bytes_per_line_src = src_frame->linesize[plane_index];
    int bytes_per_line_dst = cb_data->linesize[plane_index]; // Geralmente iguais para BGR
    int bytes_to_copy_per_line = cb_data->width * 3;
    
    // Se os linesizes forem idênticos, podemos usar memcpy único (mais rápido)
    if (bytes_per_line_src == bytes_per_line_dst && 
        (size_t)bytes_per_line_src * cb_data->height == cb_data->data_buffer_size[plane_index]) 
    {
         memcpy(cb_data->data[plane_index], 
                src_frame->data[plane_index], 
                cb_data->data_buffer_size[plane_index]);
         log_message(LOG_LEVEL_TRACE, "[Callback Utils] Cópia BGR realizada com memcpy único.");
    } else {
        // Se houver padding (linesize > width * 3) ou outra diferença,
        // copiar linha por linha.
        log_message(LOG_LEVEL_TRACE, "[Callback Utils] Cópia BGR linha por linha (src_stride=%d, dst_stride=%d).", 
                    bytes_per_line_src, bytes_per_line_dst);
        for (int y = 0; y < cb_data->height; ++y) {
            memcpy(cb_data->data[plane_index] + y * bytes_per_line_dst,    // Destino
                   src_frame->data[plane_index] + y * bytes_per_line_src, // Origem
                   bytes_to_copy_per_line);                              // Quantidade
        }
    }

    // Validar se a cópia foi bem sucedida (verificação extra)
    if (!cb_data->data[plane_index]) { // Improvável chegar aqui se malloc funcionou
        log_message(LOG_LEVEL_ERROR, "[Callback Utils] ERRO CRÍTICO: Ponteiro de dados nulo após cópia!");
        // Tentar liberar o que foi alocado
        if(cb_data->data[plane_index]) free(cb_data->data[plane_index]);
        free(cb_data);
        return NULL;
    }

    log_message(LOG_LEVEL_TRACE, "[Callback Utils] Estrutura callback_frame_data_t criada com sucesso (Ref: %p, PTS: %ld).", cb_data, cb_data->pts);
    return cb_data;
}

void callback_utils_free_data(callback_frame_data_t* data) {
    if (!data) {
        // log_message(LOG_LEVEL_TRACE, "[Callback Utils] Tentativa de liberar dados nulos.");
        return; // Seguro chamar com NULL
    }

    log_message(LOG_LEVEL_TRACE, "[Callback Utils] Liberando estrutura callback_frame_data_t (Ref: %p, PTS: %ld)...", data, data->pts);
    
    // Liberar cada buffer de dados alocado
    for (int i = 0; i < 4; ++i) { // Itera sobre os 4 possíveis ponteiros de plano
        if (data->data[i]) {
            // log_message(LOG_LEVEL_TRACE, "[Callback Utils] Liberando buffer do plano %d (Ref: %p).", i, data->data[i]);
            free(data->data[i]);
            data->data[i] = NULL; // Boa prática zerar ponteiro após free
        }
    }
    
    // Liberar a estrutura principal
    free(data);
    log_message(LOG_LEVEL_TRACE, "[Callback Utils] Estrutura liberada.");
} 