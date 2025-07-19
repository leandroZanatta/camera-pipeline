#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libavutil/error.h> // Para av_make_error_string
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

// --- Variáveis Estáticas Globais --- 
static log_level_t g_log_level = LOG_LEVEL_INFO; // Nível padrão
static const char* LOG_LEVEL_NAMES[] = {"ERRO", "ALERTA", "INFO", "DEBUG", "TRACE"};

// --- Variáveis para logging em disco ---
static FILE* g_log_file = NULL;
static char* g_log_file_path = NULL;
static size_t g_max_file_size_mb = 0;
static bool g_performance_tracking_enabled = false;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Hash table para performance stats por camera_id ---
typedef struct performance_entry {
    int camera_id;
    performance_stats_t stats;
    struct performance_entry* next;
} performance_entry_t;

static performance_entry_t* g_performance_table = NULL;
static pthread_mutex_t g_performance_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Funções auxiliares ---

// Função para calcular diferença de tempo em segundos
static double timespec_diff_s(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1.0e9;
}

// Função para obter ou criar entry de performance
static performance_entry_t* get_or_create_performance_entry(int camera_id) {
    performance_entry_t* entry = g_performance_table;
    
    // Procurar entry existente
    while (entry) {
        if (entry->camera_id == camera_id) {
            return entry;
        }
        entry = entry->next;
    }
    
    // Criar nova entry
    entry = malloc(sizeof(performance_entry_t));
    if (!entry) return NULL;
    
    entry->camera_id = camera_id;
    memset(&entry->stats, 0, sizeof(performance_stats_t));
    pthread_mutex_init(&entry->stats.stats_mutex, NULL);
    clock_gettime(CLOCK_MONOTONIC, &entry->stats.last_frame_time);
    clock_gettime(CLOCK_MONOTONIC, &entry->stats.last_activity_time);
    
    // Inserir no início da lista
    entry->next = g_performance_table;
    g_performance_table = entry;
    
    return entry;
}

// Função para verificar e rotacionar arquivo de log se necessário
static void check_and_rotate_log_file(void) {
    if (!g_log_file || !g_log_file_path || g_max_file_size_mb == 0) {
        return;
    }
    
    // Obter tamanho atual do arquivo
    fflush(g_log_file);
    struct stat st;
    if (stat(g_log_file_path, &st) != 0) {
        return;
    }
    
    size_t current_size_mb = st.st_size / (1024 * 1024);
    if (current_size_mb < g_max_file_size_mb) {
        return;
    }
    
    // Rotacionar arquivo
    char backup_path[1024];
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    
    snprintf(backup_path, sizeof(backup_path), "%s.%04d%02d%02d_%02d%02d%02d",
             g_log_file_path,
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec);
    
    // Fechar arquivo atual
    fclose(g_log_file);
    
    // Renomear arquivo atual para backup
    if (rename(g_log_file_path, backup_path) == 0) {
        // Reabrir arquivo
        g_log_file = fopen(g_log_file_path, "a");
        if (g_log_file) {
            setvbuf(g_log_file, NULL, _IOLBF, 0); // Line buffering
        }
    } else {
        // Se falhou ao renomear, reabrir o arquivo original
        g_log_file = fopen(g_log_file_path, "a");
        if (g_log_file) {
            setvbuf(g_log_file, NULL, _IOLBF, 0);
        }
    }
}

// --- Implementação das Funções Públicas ---

bool logger_init(const char* log_file_path, size_t max_file_size_mb, bool enable_performance_tracking) {
    pthread_mutex_lock(&g_log_mutex);
    
    // Se já inicializado, limpar primeiro
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    if (g_log_file_path) {
        free(g_log_file_path);
        g_log_file_path = NULL;
    }
    
    // Configurar novo arquivo
    if (log_file_path) {
        g_log_file_path = strdup(log_file_path);
        g_log_file = fopen(log_file_path, "a");
        if (g_log_file) {
            setvbuf(g_log_file, NULL, _IOLBF, 0); // Line buffering
        } else {
            pthread_mutex_unlock(&g_log_mutex);
            return false;
        }
    }
    
    g_max_file_size_mb = max_file_size_mb;
    g_performance_tracking_enabled = enable_performance_tracking;
    
    // Log inicial
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    if (g_log_file) {
        fprintf(g_log_file, "\n=== LOG INICIADO EM %s ===\n", timestamp);
        fprintf(g_log_file, "Arquivo: %s\n", log_file_path ? log_file_path : "stdout/stderr");
        fprintf(g_log_file, "Tamanho máximo: %zu MB\n", max_file_size_mb);
        fprintf(g_log_file, "Performance tracking: %s\n", enable_performance_tracking ? "HABILITADO" : "DESABILITADO");
        fprintf(g_log_file, "=====================================\n\n");
        fflush(g_log_file);
    }
    
    pthread_mutex_unlock(&g_log_mutex);
    return true;
}

void logger_cleanup(void) {
    pthread_mutex_lock(&g_log_mutex);
    
    // Limpar performance table
    pthread_mutex_lock(&g_performance_mutex);
    performance_entry_t* entry = g_performance_table;
    while (entry) {
        performance_entry_t* next = entry->next;
        pthread_mutex_destroy(&entry->stats.stats_mutex);
        free(entry);
        entry = next;
    }
    g_performance_table = NULL;
    pthread_mutex_unlock(&g_performance_mutex);
    
    // Fechar arquivo de log
    if (g_log_file) {
        time_t now = time(NULL);
        struct tm* tm_info = localtime(&now);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
        fprintf(g_log_file, "\n=== LOG FINALIZADO EM %s ===\n", timestamp);
        fclose(g_log_file);
        g_log_file = NULL;
    }
    
    if (g_log_file_path) {
        free(g_log_file_path);
        g_log_file_path = NULL;
    }
    
    pthread_mutex_unlock(&g_log_mutex);
}

void logger_set_level(log_level_t level) {
    if (level >= LOG_LEVEL_ERROR && level <= LOG_LEVEL_TRACE) {
        if (level != g_log_level) {
             g_log_level = level;
             if (g_log_level >= LOG_LEVEL_INFO) {
                 log_message(LOG_LEVEL_INFO, "[Logger] Nível de log definido para %s (%d)", LOG_LEVEL_NAMES[level], level);
             }
        }
    } else {
        if (g_log_level >= LOG_LEVEL_WARNING) {
             log_message(LOG_LEVEL_WARNING, "[Logger] Tentativa de definir nível de log inválido: %d", level);
        }
    }
}

void log_message(log_level_t level, const char* format, ...) {
    if (level > g_log_level) return;
    
    pthread_mutex_lock(&g_log_mutex);
    
    // Buffer para a mensagem formatada
    char log_buffer[2048]; 
    char* current_pos = log_buffer;
    size_t remaining_size = sizeof(log_buffer);

    // Adicionar Timestamp com microssegundos
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm* tm_info = localtime(&ts.tv_sec);
    int written = strftime(current_pos, remaining_size, "%Y-%m-%d %H:%M:%S", tm_info);
    if (written > 0) {
        current_pos += written;
        remaining_size -= written;
        written = snprintf(current_pos, remaining_size, ".%06ld", ts.tv_nsec / 1000);
        if (written > 0) {
            current_pos += written;
            remaining_size -= written;
        }
    }

    // Adicionar Prefixo [Nível]
    written = snprintf(current_pos, remaining_size, " [%-6s] ", LOG_LEVEL_NAMES[level]);
    if (written > 0) {
        current_pos += written;
        remaining_size -= written;
    }

    // Adicionar Mensagem Formatada
    va_list args;
    va_start(args, format);
    written = vsnprintf(current_pos, remaining_size, format, args);
    va_end(args);
    if (written > 0) {
        current_pos += written;
        remaining_size -= written;
    }

    // Garantir Nova Linha
    if (remaining_size > 1) {
        *current_pos = '\n';
        current_pos++;
        *current_pos = '\0';
    }

    // Escrever no arquivo e stdout/stderr
    if (g_log_file) {
        fprintf(g_log_file, "%s", log_buffer);
        fflush(g_log_file);
        check_and_rotate_log_file();
    }
    
    // Também escrever no console
    FILE* console_output = (level <= LOG_LEVEL_WARNING) ? stderr : stdout;
    fprintf(console_output, "%s", log_buffer);
    fflush(console_output);
    
    pthread_mutex_unlock(&g_log_mutex);
}

void log_ffmpeg_error(log_level_t level, const char* prefix, int error_code) {
    if (level > g_log_level) return;
    
    char error_buffer[AV_ERROR_MAX_STRING_SIZE];
    memset(error_buffer, 0, sizeof(error_buffer));
    av_make_error_string(error_buffer, sizeof(error_buffer), error_code);
    
    log_message(level, "%s: %s (code %d / 0x%x)", 
                prefix ? prefix : "Erro FFmpeg", 
                error_buffer, 
                error_code, error_code);
}

void log_activity(int camera_id, const char* activity_type, double processing_time_ms) {
    if (!g_performance_tracking_enabled) return;
    
    pthread_mutex_lock(&g_performance_mutex);
    performance_entry_t* entry = get_or_create_performance_entry(camera_id);
    if (!entry) {
        pthread_mutex_unlock(&g_performance_mutex);
        return;
    }
    
    pthread_mutex_lock(&entry->stats.stats_mutex);
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    // Atualizar última atividade
    entry->stats.last_activity_time = now;
    
    // Atualizar contadores baseado no tipo de atividade
    if (strcmp(activity_type, "frame") == 0) {
        entry->stats.frame_count++;
        entry->stats.last_frame_time = now;
        
        // Atualizar estatísticas de tempo de processamento
        if (processing_time_ms > 0) {
            if (entry->stats.frame_count == 1) {
                entry->stats.avg_processing_time_ms = processing_time_ms;
                entry->stats.max_processing_time_ms = processing_time_ms;
            } else {
                entry->stats.avg_processing_time_ms = 
                    (entry->stats.avg_processing_time_ms * (entry->stats.frame_count - 1) + processing_time_ms) / entry->stats.frame_count;
                if (processing_time_ms > entry->stats.max_processing_time_ms) {
                    entry->stats.max_processing_time_ms = processing_time_ms;
                }
            }
        }
        
        // Resetar contadores de erro consecutivo
        entry->stats.consecutive_errors = 0;
        entry->stats.consecutive_warnings = 0;
        
    } else if (strcmp(activity_type, "error") == 0) {
        entry->stats.error_count++;
        entry->stats.consecutive_errors++;
        entry->stats.consecutive_warnings = 0;
        
        // Log de erro consecutivo
        if (entry->stats.consecutive_errors >= 3) {
            log_message(LOG_LEVEL_WARNING, "[Performance ID %d] %d erros consecutivos detectados", 
                       camera_id, entry->stats.consecutive_errors);
        }
        
    } else if (strcmp(activity_type, "warning") == 0) {
        entry->stats.warning_count++;
        entry->stats.consecutive_warnings++;
        entry->stats.consecutive_errors = 0;
        
        // Log de warning consecutivo
        if (entry->stats.consecutive_warnings >= 5) {
            log_message(LOG_LEVEL_WARNING, "[Performance ID %d] %d warnings consecutivos detectados", 
                       camera_id, entry->stats.consecutive_warnings);
        }
    }
    
    pthread_mutex_unlock(&entry->stats.stats_mutex);
    pthread_mutex_unlock(&g_performance_mutex);
}

bool check_processing_stall(int camera_id, int timeout_seconds) {
    if (!g_performance_tracking_enabled) return false;
    
    pthread_mutex_lock(&g_performance_mutex);
    performance_entry_t* entry = get_or_create_performance_entry(camera_id);
    if (!entry) {
        pthread_mutex_unlock(&g_performance_mutex);
        return false;
    }
    
    pthread_mutex_lock(&entry->stats.stats_mutex);
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    double time_since_activity = timespec_diff_s(&entry->stats.last_activity_time, &now);
    double time_since_frame = timespec_diff_s(&entry->stats.last_frame_time, &now);
    
    bool stall_detected = (time_since_activity > timeout_seconds) || (time_since_frame > timeout_seconds);
    
    if (stall_detected) {
        log_message(LOG_LEVEL_ERROR, "[Stall Detection ID %d] PARADA DETECTADA! Última atividade: %.1fs atrás, Último frame: %.1fs atrás", 
                   camera_id, time_since_activity, time_since_frame);
    }
    
    pthread_mutex_unlock(&entry->stats.stats_mutex);
    pthread_mutex_unlock(&g_performance_mutex);
    
    return stall_detected;
}

bool get_performance_stats(int camera_id, performance_stats_t* stats) {
    if (!g_performance_tracking_enabled || !stats) return false;
    
    pthread_mutex_lock(&g_performance_mutex);
    performance_entry_t* entry = get_or_create_performance_entry(camera_id);
    if (!entry) {
        pthread_mutex_unlock(&g_performance_mutex);
        return false;
    }
    
    pthread_mutex_lock(&entry->stats.stats_mutex);
    memcpy(stats, &entry->stats, sizeof(performance_stats_t));
    pthread_mutex_unlock(&entry->stats.stats_mutex);
    pthread_mutex_unlock(&g_performance_mutex);
    
    return true;
}

void log_heartbeat(int camera_id, const char* component) {
    if (!g_performance_tracking_enabled) return;
    
    log_message(LOG_LEVEL_DEBUG, "[Heartbeat ID %d] %s ativo", camera_id, component ? component : "componente");
    log_activity(camera_id, "heartbeat", 0.0);
} 