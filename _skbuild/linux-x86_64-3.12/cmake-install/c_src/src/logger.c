#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libavutil/error.h> // Para av_make_error_string

// --- Variáveis Estáticas Globais --- 
static log_level_t g_log_level = LOG_LEVEL_INFO; // Nível padrão
static const char* LOG_LEVEL_NAMES[] = {"ERRO", "ALERTA", "INFO", "DEBUG", "TRACE"};

// --- Implementação das Funções --- 

void logger_set_level(log_level_t level) {
    if (level >= LOG_LEVEL_ERROR && level <= LOG_LEVEL_TRACE) {
        // Verifica se o nível realmente mudou para evitar log repetido
        if (level != g_log_level) {
             g_log_level = level;
             // Loga a mudança APENAS se o novo nível permitir INFO
             if (g_log_level >= LOG_LEVEL_INFO) {
                 log_message(LOG_LEVEL_INFO, "[Logger] Nível de log definido para %s (%d)", LOG_LEVEL_NAMES[level], level);
             }
        } // else: Nível já era o mesmo, não faz nada
    } else {
        // Loga o aviso APENAS se o nível atual permitir WARNING
        if (g_log_level >= LOG_LEVEL_WARNING) {
             log_message(LOG_LEVEL_WARNING, "[Logger] Tentativa de definir nível de log inválido: %d", level);
        }
    }
}

void log_message(log_level_t level, const char* format, ...) {
    // Checa o nível ANTES de qualquer outra operação
    if (level > g_log_level) return;
    
    // Buffer para a mensagem formatada + timestamp + prefixo
    char log_buffer[1024]; 
    char* current_pos = log_buffer;
    size_t remaining_size = sizeof(log_buffer);

    // Adicionar Timestamp
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    int written = strftime(current_pos, remaining_size, "%Y-%m-%d %H:%M:%S", tm_info);
    if (written <= 0) {
        // Falha ao formatar timestamp, usar fallback?
        snprintf(current_pos, remaining_size, "[timestamp_err]");
        written = strlen(current_pos);
    }
    current_pos += written;
    remaining_size -= written;

    // Adicionar Prefixo [Nível]
    written = snprintf(current_pos, remaining_size, " [%-6s] ", LOG_LEVEL_NAMES[level]);
    if (written < 0 || (size_t)written >= remaining_size) goto buffer_full;
    current_pos += written;
    remaining_size -= written;

    // Adicionar Mensagem Formatada
    va_list args;
    va_start(args, format);
    written = vsnprintf(current_pos, remaining_size, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= remaining_size) goto buffer_full;
    current_pos += written;
    remaining_size -= written;

    // Garantir Nova Linha no Final
    size_t current_len = strlen(log_buffer);
    if (current_len > 0 && log_buffer[current_len - 1] != '\n') {
        if (remaining_size > 1) {
            *current_pos = '\n';
            current_pos++;
            *current_pos = '\0'; // Termina a string
        } else {
            // Não há espaço para \n, truncar o último caractere (improvável)
            if (current_len > 0) log_buffer[current_len - 1] = '\n';
        }
    } else if (current_len == 0) {
        // String vazia? Adiciona só a nova linha
        if (remaining_size > 1) {
            *current_pos = '\n';
            current_pos++;
            *current_pos = '\0';
        } // else: Sem espaço
    }

// Ir para stdout/stderr
    FILE* output = (level <= LOG_LEVEL_WARNING) ? stderr : stdout;
    fprintf(output, "%s", log_buffer);
    fflush(output);
    return;

buffer_full:
    {
        // Se o buffer estourou, imprime uma mensagem truncada ou de erro
        FILE* err_output = stderr;
        fprintf(err_output, "[LOGGER_ERR] Mensagem de log truncada ou erro de formatação! Nível: %d\n", level);
        fflush(err_output);
    }
}

void log_ffmpeg_error(log_level_t level, const char* prefix, int error_code) {
    // Checa o nível antes de fazer qualquer coisa
    if (level > g_log_level) return;
    
    char error_buffer[AV_ERROR_MAX_STRING_SIZE];
    memset(error_buffer, 0, sizeof(error_buffer));
    av_make_error_string(error_buffer, sizeof(error_buffer), error_code);
    
    // Reutiliza log_message para formatação consistente
    log_message(level, "%s: %s (code %d / 0x%x)", 
                prefix ? prefix : "Erro FFmpeg", 
                error_buffer, 
                error_code, error_code);
} 