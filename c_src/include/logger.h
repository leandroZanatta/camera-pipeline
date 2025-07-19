#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h> // Para va_list
#include <stdbool.h> // Para bool, true, false
#include <pthread.h> // Para thread safety

// Níveis de Log (espelhados do camera_processor.h original)
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARNING = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_TRACE = 4
} log_level_t;

// Estrutura para controle de performance e detecção de paradas
typedef struct {
    struct timespec last_frame_time;
    struct timespec last_activity_time;
    int frame_count;
    int error_count;
    int warning_count;
    double avg_processing_time_ms;
    double max_processing_time_ms;
    int consecutive_errors;
    int consecutive_warnings;
    pthread_mutex_t stats_mutex;
} performance_stats_t;

/**
 * @brief Inicializa o sistema de logging com arquivo em disco
 * 
 * @param log_file_path Caminho para o arquivo de log
 * @param max_file_size_mb Tamanho máximo do arquivo em MB (0 = sem limite)
 * @param enable_performance_tracking Habilita tracking de performance
 * @return true se inicializado com sucesso, false caso contrário
 */
bool logger_init(const char* log_file_path, size_t max_file_size_mb, bool enable_performance_tracking);

/**
 * @brief Finaliza o sistema de logging
 */
void logger_cleanup(void);

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

/**
 * @brief Registra atividade de processamento para detecção de paradas
 * 
 * @param camera_id ID da câmera
 * @param activity_type Tipo de atividade (frame, error, warning, etc.)
 * @param processing_time_ms Tempo de processamento em ms (0 se não aplicável)
 */
void log_activity(int camera_id, const char* activity_type, double processing_time_ms);

/**
 * @brief Verifica se há paradas de processamento detectadas
 * 
 * @param camera_id ID da câmera
 * @param timeout_seconds Timeout em segundos para considerar como parada
 * @return true se parada detectada, false caso contrário
 */
bool check_processing_stall(int camera_id, int timeout_seconds);

/**
 * @brief Obtém estatísticas de performance para uma câmera
 * 
 * @param camera_id ID da câmera
 * @param stats Estrutura para receber as estatísticas
 * @return true se estatísticas obtidas com sucesso
 */
bool get_performance_stats(int camera_id, performance_stats_t* stats);

/**
 * @brief Registra heartbeat para monitoramento de atividade
 * 
 * @param camera_id ID da câmera
 * @param component Nome do componente (ex: "decoder", "processor")
 */
void log_heartbeat(int camera_id, const char* component);

// --- Macros de Log de conveniência ---
#define LOGF_ERR(fmt, ...)   log_message(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOGF_WARN(fmt, ...)  log_message(LOG_LEVEL_WARNING, fmt, ##__VA_ARGS__)
#define LOGF_INFO(fmt, ...)  log_message(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOGF_DEBUG(fmt, ...) log_message(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOGF_TRACE(fmt, ...) log_message(LOG_LEVEL_TRACE, fmt, ##__VA_ARGS__)

// --- Macros para tracking de performance ---
#define LOG_ACTIVITY(camera_id, activity, time_ms) log_activity(camera_id, activity, time_ms)
#define LOG_HEARTBEAT(camera_id, component) log_heartbeat(camera_id, component)

#endif // LOGGER_H 