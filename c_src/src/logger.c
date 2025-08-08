#define _POSIX_C_SOURCE 200809L // Para CLOCK_MONOTONIC e CLOCK_REALTIME

#include "../include/logger.h"
#include <stdbool.h>
#include <stddef.h>

// Stubs de logger: removem completamente qualquer saída de log

bool logger_init(const char* log_file_path, size_t max_file_size_mb, bool enable_performance_tracking) {
    (void)log_file_path;
    (void)max_file_size_mb;
    (void)enable_performance_tracking;
    return true;
}

void logger_cleanup(void) {
    // no-op
}

void logger_set_level(log_level_t level) {
    (void)level;
}

// Função para extrair camera_id da mensagem
static int extract_camera_id_from_message(const char* format) {
    // Procurar por padrões como "[Thread ID X]", "[Stream Processing ID X]", etc.
    const char* patterns[] = {
        "[Thread ID ",
        "[Stream Processing ID ",
        "[Status ID ",
        "[Frame Skip ID ",
        "[FPS Real ID ",
        "[Performance ID ",
        "[Stall Detection ID ",
        "[Heartbeat ID ",
        "[Logger] Sistema de logging inicializado para câmera "
    };
    
    for (int i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        const char* pos = strstr(format, patterns[i]);
        if (pos) {
            pos += strlen(patterns[i]);
            return atoi(pos);
        }
    }
    
    return 0; // Não encontrou
}

void log_message(log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
    // no-op
}

void log_ffmpeg_error(log_level_t level, const char* prefix, int error_code) {
    (void)level;
    (void)prefix;
    (void)error_code;
    // no-op
}

void log_activity(int camera_id, const char* activity_type, double processing_time_ms) {
    (void)camera_id;
    (void)activity_type;
    (void)processing_time_ms;
    // no-op
}

bool check_processing_stall(int camera_id, int timeout_seconds) {
    (void)camera_id;
    (void)timeout_seconds;
    return false;
}

bool get_performance_stats(int camera_id, performance_stats_t* stats) {
    (void)camera_id;
    (void)stats;
    return false;
}

void log_heartbeat(int camera_id, const char* component) {
    (void)camera_id;
    (void)component;
    // no-op
} 