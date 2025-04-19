#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h> // Para va_list

// Níveis de Log (espelhados do camera_processor.h original)
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARNING = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_TRACE = 4
} log_level_t;

/**
 * @brief Define o nível global de log.
 * 
 * @param level O nível de log desejado.
 */
void logger_set_level(log_level_t level);

/**
 * @brief Registra uma mensagem de log formatada.
 * 
 * @param level O nível da mensagem.
 * @param format A string de formato (como printf).
 * @param ... Argumentos variáveis para o formato.
 */
void log_message(log_level_t level, const char* format, ...);

/**
 * @brief Registra uma mensagem de erro específica do FFmpeg.
 * 
 * @param level O nível da mensagem.
 * @param prefix Um prefixo opcional para a mensagem.
 * @param error_code O código de erro retornado pelo FFmpeg.
 */
void log_ffmpeg_error(log_level_t level, const char* prefix, int error_code);

#endif // LOGGER_H 