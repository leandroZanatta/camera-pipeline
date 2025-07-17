#define _POSIX_C_SOURCE 200809L // Define before including headers for clock_gettime

#include "camera_thread.h"
#include "logger.h"
#include "callback_utils.h"
#include "camera_context.h"
#include "frame_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <pthread.h>
#include <unistd.h> 
#include <time.h>   
#include <errno.h> // Incluir para strerror
#include <signal.h>
#include <fcntl.h>
#include <sys/select.h>

// FFmpeg includes necessários para operações de decodificação completas
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>

#define RECONNECT_DELAY_BASE 2
#define MIN_RECONNECT_DELAY 1 
#define MAX_RECONNECT_DELAY 30
#define INITIALIZATION_TIMEOUT_SECONDS 30

// --- ADICIONADO: Constante para intervalo de cálculo de FPS ---
#define FPS_CALC_INTERVAL_S 5.0
// --- FIM DA ADIÇÃO ---

// Função utilitária para calcular diferença de tempo (movida para cá, se já tiver uma, remova esta)
double timespec_diff_s(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1.0e9;
}

#define CAMERA_ID 0 // ID fixo (considerar mover para o contexto ou remover se não for fixo)

// Declaração da função externa para enviar frames para Python
extern void send_frame_to_python(void* camera_context);

// Declaração da função convert_and_dispatch_frame
static bool convert_and_dispatch_frame(camera_thread_context_t* ctx, AVFrame* frame_to_convert);

static inline int64_t get_monotonic_time_ns() { /* ... */ } // Esta função não foi fornecida, mantendo o placeholder

static void cleanup_ffmpeg_resources(camera_thread_context_t* ctx) { // Recebe ctx
    if (!ctx) return;
    log_message(LOG_LEVEL_DEBUG, "[Cleanup ID %d] Limpando recursos FFmpeg...", ctx->camera_id); 
    if (ctx->sws_ctx) { sws_freeContext(ctx->sws_ctx); ctx->sws_ctx = NULL; }
    if (ctx->frame_bgr) { av_frame_free(&ctx->frame_bgr); ctx->frame_bgr = NULL; }
    if (ctx->decoded_frame) { av_frame_free(&ctx->decoded_frame); ctx->decoded_frame = NULL; }
    if (ctx->packet) { av_packet_free(&ctx->packet); ctx->packet = NULL; }
    if (ctx->codec_ctx) { 
        if(avcodec_is_open(ctx->codec_ctx)) {
            avcodec_close(ctx->codec_ctx);
        }
        avcodec_free_context(&ctx->codec_ctx);
        ctx->codec_ctx = NULL; 
    }
    if (ctx->fmt_ctx) { 
        avformat_close_input(&ctx->fmt_ctx); 
        ctx->fmt_ctx = NULL; 
    }
    ctx->video_stream_index = -1;
    ctx->codec = NULL;
    log_message(LOG_LEVEL_DEBUG, "[Cleanup ID %d] Recursos FFmpeg limpos.", ctx->camera_id);
}

// Atualiza estado e chama callback de status
static void update_camera_status(camera_thread_context_t* ctx, camera_state_t new_state, const char* message) { // Recebe ctx
    if (!ctx) return;
    if (ctx->state == new_state /* && comparar mensagem se relevante */) { 
        return; 
    }
    ctx->state = new_state;
    log_message(LOG_LEVEL_INFO, "[Status ID %d] Novo estado: %d (%s)", ctx->camera_id, new_state, message ? message : "");
    if (ctx->status_cb) { 
        log_message(LOG_LEVEL_DEBUG, "[Status ID %d] Chamando status_cb para Python...", ctx->camera_id);
        ctx->status_cb(ctx->camera_id, (int)new_state, message ? message : "", ctx->status_cb_user_data);
        log_message(LOG_LEVEL_DEBUG, "[Status ID %d] Retornou do status_cb Python.", ctx->camera_id);
    }
}

// Tratador de sinal para SIGUSR1 
static void handle_sigusr1(int sig) {
    // Este handler não precisa fazer nada além de existir
    // O objetivo é apenas interromper chamadas bloqueantes como av_read_frame
    log_message(LOG_LEVEL_DEBUG, "[Signal Handler] Recebido sinal SIGUSR1 para interrupção de thread");
}

// Configurar o tratador de sinais para a thread
static void setup_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        log_message(LOG_LEVEL_ERROR, "[Camera Thread] Falha ao configurar handler para SIGUSR1: %s", strerror(errno));
    } else {
        log_message(LOG_LEVEL_DEBUG, "[Camera Thread] Handler para SIGUSR1 configurado com sucesso");
    }
}

// Função para verificar se há interrupção no pipe
static bool check_interrupt_pipe(int fd) {
    if (fd < 0) return false;
    
    fd_set readfds;
    struct timeval tv = {0, 0}; // Poll não-bloqueante
    
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    
    int ret = select(fd + 1, &readfds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(fd, &readfds)) {
        char buf[10];
        // Ler e descartar dados do pipe
        if (read(fd, buf, sizeof(buf)) > 0) {
            log_message(LOG_LEVEL_DEBUG, "[Camera Thread] Interrupção detectada pelo pipe");
            return true;
        }
    }
    
    return false;
}

// Modificar a função interrupt_callback para verificar o pipe
static int interrupt_callback(void* opaque) {
    camera_thread_context_t* ctx = (camera_thread_context_t*)opaque;
    if (!ctx) return 0; // Sem interrupção se contexto inválido
    
    // Verificar se há pedido explícito de parada
    if (ctx->stop_requested) {
        log_message(LOG_LEVEL_DEBUG, "[Camera Thread] Interrupção solicitada via stop_requested");
        return 1; // Interromper operações FFmpeg
    }
    
    // Verificar se há interrupção via pipe
    if (check_interrupt_pipe(ctx->interrupt_read_fd)) {
        log_message(LOG_LEVEL_DEBUG, "[Camera Thread] Interrupção via pipe detectada");
        ctx->stop_requested = true; // Marcar como solicitação de parada
        return 1;
    }
    
    return 0; // Continuar operação
}

// --- Funções Auxiliares para a Lógica da Thread (já recebem ctx) ---

static bool initialize_ffmpeg_connection(camera_thread_context_t* ctx, AVDictionary** opts) {
    if (!ctx) return false;
    
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init] Alocando AVFormatContext...");
    ctx->fmt_ctx = avformat_alloc_context();
    if (!ctx->fmt_ctx) {
        log_message(LOG_LEVEL_ERROR, "[FFmpeg Init] Falha fatal ao alocar AVFormatContext.");
        // Liberar opts se ele foi passado (embora seja improvável aqui)
        if (opts && *opts) av_dict_free(opts);
        *opts = NULL;
        return false;
    }
    // Agora é seguro configurar o callback de interrupção
    ctx->fmt_ctx->interrupt_callback.callback = interrupt_callback;
    ctx->fmt_ctx->interrupt_callback.opaque = ctx; 

    // Adicionar opções específicas para RTSP se necessário
    if (strncmp(ctx->url, "rtsp://", 7) == 0) {
        log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] URL é RTSP. Definindo transporte para TCP.", ctx->camera_id);
        if (av_dict_set(opts, "rtsp_transport", "tcp", 0) < 0) {
            log_message(LOG_LEVEL_WARNING, "[FFmpeg Init ID %d] Falha ao definir rtsp_transport=tcp.", ctx->camera_id);
        }
        // Aumentar timeout para RTSP (ex: 10 segundos)
        log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] Definindo stimeout para 10s.", ctx->camera_id);
        if (av_dict_set(opts, "stimeout", "10000000", 0) < 0) { 
             log_message(LOG_LEVEL_WARNING, "[FFmpeg Init ID %d] Falha ao definir stimeout.", ctx->camera_id);
        }
    }

    log_message(LOG_LEVEL_INFO, "[FFmpeg Init ID %d] Abrindo input: %s", ctx->camera_id, ctx->url);
    
    // Implementar retry INFINITO para "Immediate exit requested" - THREAD NUNCA PARA!
    int retry_count = 0;
    int ret;
    
    while (true) { // Loop INFINITO - nunca para!
        ret = avformat_open_input(&ctx->fmt_ctx, ctx->url, NULL, opts);
        
        if (ret == 0) {
            // Sucesso!
            if (retry_count > 0) {
                log_message(LOG_LEVEL_INFO, "[FFmpeg Init ID %d] Sucesso na tentativa %d após %d retries", 
                           ctx->camera_id, retry_count + 1, retry_count);
            }
            break;
        }
        
        // Verificar se é o erro específico "Immediate exit requested" ou outros erros de rede temporários
        if (ret == AVERROR_EXIT || ret == -1414092869 || ret == AVERROR(EIO) || ret == AVERROR(ENETUNREACH)) {
            retry_count++;
            
            // Calcular tempo de espera progressivo (1s, 2s, 3s, 4s, 5s...)
            int wait_seconds = (retry_count <= 5) ? retry_count : 5; // Máximo 5 segundos
            log_message(LOG_LEVEL_WARNING, "[FFmpeg Init ID %d] Retry %d: Immediate exit requested, aguardando %ds... (THREAD NUNCA PARA!)", 
                       ctx->camera_id, retry_count, wait_seconds);
            
            // Aguardar tempo progressivo antes de tentar novamente
            struct timespec sleep_time = {wait_seconds, 0};
            nanosleep(&sleep_time, NULL);
            
            // Verificar se foi solicitado para parar durante o retry
            if (ctx->stop_requested) {
                log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] Parada solicitada durante retry", ctx->camera_id);
                if (ctx->fmt_ctx) avformat_close_input(&ctx->fmt_ctx);
                ctx->fmt_ctx = NULL;
                av_dict_free(opts);
                *opts = NULL;
                return false;
            }
            continue; // Tentar novamente infinitamente
        } else {
            // Outro tipo de erro, não tentar retry infinito
            log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] Erro não é 'Immediate exit' (%d), não tentando retry infinito", ctx->camera_id, ret);
            break;
        }
    }
    
    if (ret < 0) {
        log_ffmpeg_error(LOG_LEVEL_ERROR, "[FFmpeg Init ID %d] Falha ao abrir input (erro não retryável)", ret);
        if (ctx->fmt_ctx) avformat_close_input(&ctx->fmt_ctx); 
        ctx->fmt_ctx = NULL;
        av_dict_free(opts); // Liberar opts em caso de falha aqui
        *opts = NULL;
        return false;
    }
    
    // Se chegou aqui, o retry infinito foi bem-sucedido!
    log_message(LOG_LEVEL_INFO, "[FFmpeg Init ID %d] Retry infinito bem-sucedido!", ctx->camera_id);
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] avformat_open_input SUCESSO.", ctx->camera_id);

    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] Buscando stream info...", ctx->camera_id);
    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    av_dict_free(opts); // Liberar opts após o uso bem-sucedido ou falha de find_stream_info
    *opts = NULL;
    if (ret < 0) {
        log_ffmpeg_error(LOG_LEVEL_ERROR, "[FFmpeg Init ID %d] Falha ao buscar stream info (erro não retryável)", ret);
        if (ctx->fmt_ctx) avformat_close_input(&ctx->fmt_ctx);
        ctx->fmt_ctx = NULL;
        return false;
    }
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] avformat_find_stream_info SUCESSO.", ctx->camera_id);

    log_message(LOG_LEVEL_INFO, "[FFmpeg Init] Conexão inicializada com sucesso.");
    return true;
}

static bool setup_video_decoder(camera_thread_context_t* ctx) {
    if (!ctx) return false;
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder] Procurando melhor stream de vídeo...");
    ctx->video_stream_index = av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &ctx->codec, 0);
    if (ctx->video_stream_index < 0 || !ctx->codec) {
        log_message(LOG_LEVEL_ERROR, "[FFmpeg Decoder] Nenhum stream de vídeo válido encontrado.");
        return false;
    }
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder] Stream de vídeo encontrado (índice %d, codec: %s).", 
                ctx->video_stream_index, ctx->codec->name);

    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder] Alocando AVCodecContext...");
    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->codec_ctx) {
        log_message(LOG_LEVEL_ERROR, "[FFmpeg Decoder] Falha ao alocar AVCodecContext.");
        return false;
    }

    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder ID %d] Copiando parâmetros do codec...", ctx->camera_id);
    int ret = avcodec_parameters_to_context(ctx->codec_ctx, ctx->fmt_ctx->streams[ctx->video_stream_index]->codecpar);
    if (ret < 0) {
        log_ffmpeg_error(LOG_LEVEL_ERROR, "[FFmpeg Decoder ID %d] Falha ao copiar parâmetros", ret);
        avcodec_free_context(&ctx->codec_ctx); // Liberar contexto alocado
        ctx->codec_ctx = NULL;
        return false;
    }
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder ID %d] avcodec_parameters_to_context SUCESSO.", ctx->camera_id);

    // Otimização de threads
    ctx->codec_ctx->thread_count = 0; // Deixar FFmpeg decidir ou 1 se for uma única thread
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder ID %d] Thread count definido para %d.", ctx->camera_id, ctx->codec_ctx->thread_count);

    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder ID %d] Abrindo codec...", ctx->camera_id);
    ret = avcodec_open2(ctx->codec_ctx, ctx->codec, NULL);
    if (ret < 0) {
        log_ffmpeg_error(LOG_LEVEL_ERROR, "[FFmpeg Decoder ID %d] Falha ao abrir codec", ret);
        avcodec_free_context(&ctx->codec_ctx);
        ctx->codec_ctx = NULL;
        return false;
    }
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder ID %d] avcodec_open2 SUCESSO. Resolução: %dx%d", 
                ctx->camera_id, ctx->codec_ctx->width, ctx->codec_ctx->height);

    // --- MODIFICADO: Estimar FPS da Fonte e Calcular Frame Skip Count --- 
    AVStream* video_stream = ctx->fmt_ctx->streams[ctx->video_stream_index];
    AVRational frame_rate = av_guess_frame_rate(ctx->fmt_ctx, video_stream, NULL);
    
    // Estimar FPS de entrada REALISTA para o controle de descarte
    double detected_fps_from_metadata = av_q2d(frame_rate);

    // Inicializar com um valor razoável, mas vamos medir o real
    if (detected_fps_from_metadata > 4.0 && detected_fps_from_metadata < 65.0) {
        ctx->estimated_source_fps = detected_fps_from_metadata;
        log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] FPS inicial da fonte de metadados: %.2f (será ajustado com medição real).", 
                    ctx->camera_id, ctx->estimated_source_fps);
    } else {
        ctx->estimated_source_fps = 30.0; // Padrão razoável para a maioria das câmeras
        log_message(LOG_LEVEL_WARNING, "[Frame Skip ID %d] FPS da fonte de metadados (%.2f) parece irreal. Usando 30.0 FPS inicial (será ajustado com medição real).", 
                    ctx->camera_id, detected_fps_from_metadata);
    }

    // Inicializar contadores e tempos para medição real
    ctx->frame_input_counter = 0;
    clock_gettime(CLOCK_MONOTONIC, &ctx->last_input_fps_calc_time);
    ctx->calculated_input_fps = 0.0;
    ctx->frame_skip_ratio = 1.0; // Começar sem skip
    ctx->frame_skip_count = 1;
    ctx->frame_skip_accumulator = 0.0;
    ctx->frame_process_counter = 0;

    // Flag para indicar que ainda não temos medição real
    ctx->has_real_fps_measurement = false;

    // Calcular Frame Skip Ratio e inicializar acumulador
    if (ctx->target_fps <= 0 || ctx->estimated_source_fps <= 0 || ctx->target_fps >= ctx->estimated_source_fps) {
        // Se target_fps não está definido, ou é igual/maior que a fonte, não pule quadros.
        ctx->frame_skip_ratio = 1.0;
        ctx->frame_skip_count = 1; // Enviar todos
        ctx->frame_skip_accumulator = 0.0;
        log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] TargetFPS (%d) desativado ou >= FPS da fonte (%.2f). FrameSkipRatio=1.0 (Não pular).",
                    ctx->camera_id, ctx->target_fps, ctx->estimated_source_fps);
    } else {
        // A fonte é mais rápida que o target. Calcular razão exata de skip.
        ctx->frame_skip_ratio = ctx->estimated_source_fps / ctx->target_fps;
        ctx->frame_skip_count = (int)floor(ctx->frame_skip_ratio); // Parte inteira
        ctx->frame_skip_accumulator = 0.0; // Inicializar acumulador
        log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] Configurado: TargetFPS=%d, SourceFPS=%.2f, FrameSkipRatio=%.3f (Parte inteira=%d)",
                    ctx->camera_id, ctx->target_fps, ctx->estimated_source_fps, ctx->frame_skip_ratio, ctx->frame_skip_count);
    }
    
    // Inicializar contadores de FPS e skip
    ctx->frame_process_counter = 0; // Contagem de quadros DECODIFICADOS para a lógica de descarte
    ctx->frame_input_counter = 0;   // Contagem de quadros DECODIFICADOS *recebidos* para medir FPS real de entrada

    // --- FIM DA MODIFICAÇÃO: Estimar FPS da Fonte e Calcular Frame Skip Count --- 

    return true;
}

static bool allocate_packets_and_frames(camera_thread_context_t* ctx) {
    if (!ctx) return false;
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Alloc] Alocando packet e frames...");
    ctx->packet = av_packet_alloc();
    ctx->decoded_frame = av_frame_alloc();
    ctx->frame_bgr = av_frame_alloc(); 
    if (!ctx->packet || !ctx->decoded_frame || !ctx->frame_bgr) {
        log_message(LOG_LEVEL_ERROR, "[FFmpeg Alloc] Falha fatal ao alocar Packet/Frames.");
        if (ctx->packet) av_packet_free(&ctx->packet);
        if (ctx->decoded_frame) av_frame_free(&ctx->decoded_frame);
        if (ctx->frame_bgr) av_frame_free(&ctx->frame_bgr);
        ctx->packet = NULL; ctx->decoded_frame = NULL; ctx->frame_bgr = NULL;
        return false;
    }
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Alloc] Packet e frames alocados.");
    return true;
}

// --- CONSUMIDOR: Nova Thread para processar frames decodificados e chamar Python ---
void* consume_frames_thread(void* arg) {
    camera_thread_context_t* ctx = (camera_thread_context_t*)arg;
    if (!ctx) {
        log_message(LOG_LEVEL_ERROR, "[Consumer Thread] Contexto NULL recebido");
        return NULL;
    }
    
    log_message(LOG_LEVEL_INFO, "[Consumer Thread ID %d] Iniciando thread consumidora", ctx->camera_id);
    
    // Inicializar contadores para FPS de saída
    ctx->frame_send_counter = 0;
    clock_gettime(CLOCK_MONOTONIC, &ctx->last_output_fps_calc_time);
    ctx->calculated_output_fps = 0.0;
    
    // Inicializar frame_bgr e sws_ctx para esta thread
    ctx->frame_bgr = av_frame_alloc();
    if (!ctx->frame_bgr) {
        log_message(LOG_LEVEL_ERROR, "[Consumer Thread ID %d] Falha ao alocar frame_bgr", ctx->camera_id);
        return NULL;
    }
    
    ctx->sws_ctx = NULL;
    ctx->sws_ctx_width = 0;
    ctx->sws_ctx_height = 0;
    ctx->sws_ctx_in_fmt = AV_PIX_FMT_NONE;
    
    while (!ctx->stop_requested) {
        // Aguardar frame da fila
        AVFrame* decoded_frame = frame_queue_pop(&ctx->decoded_frame_queue, &ctx->stop_requested);
        
        if (!decoded_frame) {
            // Timeout ou fila vazia - continuar loop
            continue;
        }
        
        // Processar frame com pacing e FPS control
        if (ctx->target_fps > 0) {
            // Calcular intervalo entre frames
            int64_t target_interval_ns = 1000000000LL / ctx->target_fps;
            
            // Verificar se é hora de enviar este frame
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            
            if (ctx->frame_send_counter > 0) {
                // Calcular tempo desde o último frame enviado
                double elapsed_s = timespec_diff_s(&ctx->last_frame_sent_time, &now);
                int64_t elapsed_ns = (int64_t)(elapsed_s * 1000000000.0);
                
                if (elapsed_ns < target_interval_ns) {
                    // Ainda não é hora de enviar - aguardar
                    int64_t sleep_ns = target_interval_ns - elapsed_ns;
                    struct timespec sleep_time = {0, sleep_ns};
                    nanosleep(&sleep_time, NULL);
                    
                    // Atualizar tempo atual após sleep
                    clock_gettime(CLOCK_MONOTONIC, &now);
                }
            }
            
            // Atualizar tempo do último frame enviado
            ctx->last_frame_sent_time = now;
        }
        
        // Converter frame para BGR e enviar para Python
        if (convert_and_dispatch_frame(ctx, decoded_frame)) {
            ctx->frame_send_counter++;
            
            // Calcular FPS de saída a cada 5 segundos
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed_s = timespec_diff_s(&ctx->last_output_fps_calc_time, &now);
            
            if (elapsed_s >= FPS_CALC_INTERVAL_S) {
                ctx->calculated_output_fps = ctx->frame_send_counter / elapsed_s;
                log_message(LOG_LEVEL_INFO, "[Consumer Thread ID %d] FPS de saída: %.2f (frames: %ld, tempo: %.2fs)", 
                           ctx->camera_id, ctx->calculated_output_fps, ctx->frame_send_counter, elapsed_s);
                
                // Resetar contadores
                ctx->frame_send_counter = 0;
                ctx->last_output_fps_calc_time = now;
            }
        }
        
        // Liberar frame decodificado
        av_frame_free(&decoded_frame);
    }
    
    // Limpeza da thread consumidora
    log_message(LOG_LEVEL_INFO, "[Consumer Thread ID %d] Finalizando thread consumidora", ctx->camera_id);
    
    if (ctx->sws_ctx) {
        sws_freeContext(ctx->sws_ctx);
        ctx->sws_ctx = NULL;
    }
    
    if (ctx->frame_bgr) {
        av_frame_free(&ctx->frame_bgr);
        ctx->frame_bgr = NULL;
    }
    
    return NULL;
}


// Converte o frame decodificado para BGR e chama o callback Python.
static bool convert_and_dispatch_frame(camera_thread_context_t* ctx, AVFrame* frame_to_convert) {
    if (!ctx) return false;
    int ret = 0;
    bool frame_sent = false; 

    log_message(LOG_LEVEL_TRACE, "[Dispatch] Entrando ... (PTS: %ld)", frame_to_convert ? frame_to_convert->pts : -1);

    // Verificar se o frame a converter é válido
    if (!frame_to_convert || frame_to_convert->width <= 0 || frame_to_convert->height <= 0) { 
        log_message(LOG_LEVEL_WARNING, "[Dispatch] Frame decodificado inválido na entrada (struct/dims).");
        // Não precisamos av_frame_unref aqui pois o caller é responsável por isso
        return false; 
    }
    if (!frame_to_convert->data[0]) {
        log_message(LOG_LEVEL_ERROR, "[Dispatch] Frame decodificado inválido na entrada (data[0] é NULL).");
        // Não precisamos av_frame_unref aqui
        return false;
    }
    if (frame_to_convert->linesize[0] <= 0) {
        log_message(LOG_LEVEL_ERROR, "[Dispatch] Frame decodificado inválido na entrada (linesize[0]=%d <= 0).", frame_to_convert->linesize[0]);
        // Não precisamos av_frame_unref aqui
        return false;
    }

    // --- Configuração/Reconfiguração do SwsContext --- 
    log_message(LOG_LEVEL_TRACE, "[Dispatch] Verificando/Configurando SwsContext...");
    if (!ctx->sws_ctx || ctx->sws_ctx_width != frame_to_convert->width || 
        ctx->sws_ctx_height != frame_to_convert->height || 
        ctx->sws_ctx_in_fmt != frame_to_convert->format)
    {
        if(ctx->sws_ctx) sws_freeContext(ctx->sws_ctx);
        log_message(LOG_LEVEL_DEBUG, "[SWS] Criando/Recriando SwsContext: %dx%d (%d) -> %dx%d (BGR24)",
                    frame_to_convert->width, frame_to_convert->height, frame_to_convert->format,
                    frame_to_convert->width, frame_to_convert->height, AV_PIX_FMT_BGR24);
        ctx->sws_ctx = sws_getContext(frame_to_convert->width, frame_to_convert->height, frame_to_convert->format, 
                                  frame_to_convert->width, frame_to_convert->height, AV_PIX_FMT_BGR24,
                                  SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!ctx->sws_ctx) { 
            log_message(LOG_LEVEL_ERROR, "[SWS] Falha ao criar SwsContext"); 
            goto dispatch_cleanup_and_fail; // Corrigido para ir para o label correto
        }
        ctx->sws_ctx_width = frame_to_convert->width;
        ctx->sws_ctx_height = frame_to_convert->height;
        ctx->sws_ctx_in_fmt = frame_to_convert->format;
    }
    log_message(LOG_LEVEL_TRACE, "[Dispatch] SwsContext OK.");

    // --- Alocação BGR --- 
    // Certifique-se que ctx->frame_bgr está limpo antes de alocar novo buffer
    if (ctx->frame_bgr->data[0]) {
        av_frame_unref(ctx->frame_bgr); // Libera o buffer existente
    }

    ctx->frame_bgr->width = frame_to_convert->width;
    ctx->frame_bgr->height = frame_to_convert->height;
    ctx->frame_bgr->format = AV_PIX_FMT_BGR24;
    ctx->frame_bgr->pts = frame_to_convert->pts; 
    log_message(LOG_LEVEL_TRACE, "[Dispatch] Alocando buffer BGR...");
    ret = av_frame_get_buffer(ctx->frame_bgr, 1); 
    if (ret < 0) {
        log_ffmpeg_error(LOG_LEVEL_ERROR, "[SWS] Falha ao alocar buffer para frame BGR", ret); 
        goto dispatch_cleanup_and_fail;
    }
    if (!ctx->frame_bgr->data[0]) {
        log_message(LOG_LEVEL_ERROR, "[Dispatch] Buffer BGR alocado, mas data[0] é NULL!");
        goto dispatch_cleanup_and_fail;
    }
    if (ctx->frame_bgr->linesize[0] <= 0) {
        log_message(LOG_LEVEL_ERROR, "[Dispatch] Buffer BGR alocado, mas linesize[0]=%d <= 0!", ctx->frame_bgr->linesize[0]);
        goto dispatch_cleanup_and_fail;
    }
    log_message(LOG_LEVEL_TRACE, "[Dispatch] Buffer BGR alocado OK (data[0]=%p, linesize[0]=%d).", 
                (void*)ctx->frame_bgr->data[0], ctx->frame_bgr->linesize[0]);
    
    // --- Conversão SWS --- 
    log_message(LOG_LEVEL_TRACE, "[Dispatch] Preparando para sws_scale...");
    // Log detalhado dos parâmetros ANTES de chamar sws_scale
    log_message(LOG_LEVEL_DEBUG, 
                "[Dispatch SWS Params] sws_ctx=%p, srcSlice=%p (data[0]=%p, linesize[0]=%d), srcSliceY=%d, srcSliceH=%d, dst=%p (data[0]=%p, linesize[0]=%d)",
                (void*)ctx->sws_ctx,
                (void*)frame_to_convert->data,
                (void*)frame_to_convert->data[0],
                frame_to_convert->linesize[0],
                0, // srcSliceY
                frame_to_convert->height,
                (void*)ctx->frame_bgr->data,
                (void*)ctx->frame_bgr->data[0],
                ctx->frame_bgr->linesize[0],
                ctx->frame_bgr->height // Adicione esta linha para completar o log
                );
    
    log_message(LOG_LEVEL_TRACE, "[Dispatch] Executando sws_scale...");
    sws_scale(ctx->sws_ctx, 
              (const uint8_t* const*)frame_to_convert->data, 
              frame_to_convert->linesize, 
              0, 
              frame_to_convert->height, 
              ctx->frame_bgr->data, 
              ctx->frame_bgr->linesize);
    log_message(LOG_LEVEL_TRACE, "[Dispatch] sws_scale concluído.");

    // --- Callback Python --- 
    callback_frame_data_t* cb_data = NULL;
    // Capturar o callback e user_data localmente para thread-safety se forem modificados externamente
    frame_callback_t local_frame_cb = ctx->frame_cb; 
    void* local_user_data = ctx->frame_cb_user_data;

    if (local_frame_cb) {
        // Logar o PTS ANTES de criar/enviar
        log_message(LOG_LEVEL_INFO, "[Dispatch ID %d] Preparando para enviar frame com PTS: %ld", ctx->camera_id, ctx->frame_bgr->pts);
        log_message(LOG_LEVEL_TRACE, "[Dispatch] Criando dados para callback Python...");
        cb_data = callback_pool_get_data(ctx->frame_bgr, ctx->camera_id); 
        if (cb_data) {
            log_message(LOG_LEVEL_TRACE, "[Dispatch] Dados criados. Chamando callback Python...");
            local_frame_cb(cb_data, local_user_data); 
            frame_sent = true;
            // --- MODIFICADO: Atualizar last_frame_sent_time APÓS envio bem sucedido ---
            clock_gettime(CLOCK_MONOTONIC, &ctx->last_frame_sent_time);
            // --- FIM DA MODIFICAÇÃO ---
        } else { 
            log_message(LOG_LEVEL_ERROR, "[Dispatch ID %d] Falha ao criar dados de callback (cb_data nulo).", ctx->camera_id); 
        }
    } else { 
        log_message(LOG_LEVEL_TRACE, "[Dispatch] Callback Python não definido."); 
    }

dispatch_cleanup:
    // av_frame_unref(frame_to_convert); // O caller é responsável por liberar o frame_to_convert
    av_frame_unref(ctx->frame_bgr); // Libera o buffer do frame_bgr que foi alocado com av_frame_get_buffer
    return frame_sent;

dispatch_cleanup_and_fail:
    // if (frame_to_convert) av_frame_unref(frame_to_convert); // O caller é responsável por liberar o frame_to_convert
    av_frame_unref(ctx->frame_bgr); // Libera o buffer do frame_bgr
    return false;
}

// Processa o stream lendo pacotes, decodificando, convertendo e chamando o callback.
// Retorna true se a parada foi solicitada, false em caso de erro que necessite reconexão.
static bool process_stream(camera_thread_context_t* ctx) {
    if (!ctx) return false;
    log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Iniciando loop de processamento...", ctx->camera_id);
    int ret = 0;

    // --- MODIFICADO: Reiniciar o frame_process_counter ao entrar em process_stream ---
    ctx->frame_process_counter = 0; 
    // --- FIM DA MODIFICAÇÃO ---

    while (true) {
        if (ctx->stop_requested) {
            log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Parada solicitada no início do loop.", ctx->camera_id);
            return true; // Parada solicitada, não é um erro
        }

        // --- Leitura --- 
        log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] Aguardando av_read_frame...", ctx->camera_id);
        ret = av_read_frame(ctx->fmt_ctx, ctx->packet);
        log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] av_read_frame retornou %d", ctx->camera_id, ret);

        if (ret == AVERROR_EOF) {
            log_message(LOG_LEVEL_INFO, "[Stream Processing ID %d] Fim do stream (EOF).", ctx->camera_id);
            return false; // Fim do stream, precisa reconectar (ou parar se stop_requested for true logo após)
        } else if (ret == AVERROR(EAGAIN)) {
            log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] av_read_frame retornou EAGAIN, tentando novamente...", ctx->camera_id);
            av_packet_unref(ctx->packet);
            continue; // Tentar ler novamente
        } else if (ret < 0) {
            log_ffmpeg_error(LOG_LEVEL_ERROR, "[Stream Processing ID %d] Falha ao ler frame", ctx->camera_id);
            return false; // Erro de leitura, precisa reconectar
        }

        // --- Processamento do Pacote --- 
        if (ctx->packet->stream_index == ctx->video_stream_index) {
            log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] Enviando pacote (PTS: %ld)", ctx->camera_id, ctx->packet->pts);
            ret = avcodec_send_packet(ctx->codec_ctx, ctx->packet);
            // Consumir o pacote imediatamente após avcodec_send_packet
            av_packet_unref(ctx->packet); // Libera o pacote, seja qual for o resultado de send_packet
            
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                 log_ffmpeg_error(LOG_LEVEL_WARNING, "[Stream Processing ID %d] Erro ao enviar pacote para decodificador", ctx->camera_id);
                 // Continuar para tentar receber frames que já possam estar no buffer, mas logar o aviso
            }

            // --- Loop de Recebimento de Frames --- 
            while (true) { // Loop para receber todos os frames gerados pelo pacote
                if (ctx->stop_requested) { // Checar interrupção dentro do loop de recebimento
                    log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Parada solicitada dentro do loop de recebimento de frames.", ctx->camera_id);
                    return true;
                }

                log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] Tentando receber frame...", ctx->camera_id);
                // struct timespec pre_receive_ts; // Não usada, pode ser removida
                // clock_gettime(CLOCK_MONOTONIC, &pre_receive_ts);
                ret = avcodec_receive_frame(ctx->codec_ctx, ctx->decoded_frame);
                struct timespec post_receive_ts;
                clock_gettime(CLOCK_MONOTONIC, &post_receive_ts);
                
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] avcodec_receive_frame: EAGAIN ou EOF.", ctx->camera_id);
                    break; // Nenhum frame disponível ou fim dos frames para o pacote atual
                }
                if (ret < 0) {
                    log_ffmpeg_error(LOG_LEVEL_ERROR, "[Stream Processing ID %d] Falha ao receber frame", ctx->camera_id);
                    goto process_stream_error; 
                }

                // CORREÇÃO: Usar decoded_frame nas validações e logs
                log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Frame DECODIFICADO. PTS: %ld", 
                            ctx->camera_id, ctx->decoded_frame->pts);
                
                // --- MODIFICADO: Contagem de frames decodificados para medição de FPS de entrada ---
                ctx->frame_input_counter++;

                // Calcular FPS de Entrada
                struct timespec current_time;
                clock_gettime(CLOCK_MONOTONIC, &current_time);
                double elapsed_s_input = timespec_diff_s(&ctx->last_input_fps_calc_time, &current_time);

                if (elapsed_s_input >= FPS_CALC_INTERVAL_S) {
                    if (elapsed_s_input > 0) {
                        // Calcular FPS real
                        ctx->calculated_input_fps = (double)ctx->frame_input_counter / elapsed_s_input;
                        
                        // Se ainda não temos medição real ou se o FPS mudou significativamente
                        if (!ctx->has_real_fps_measurement || 
                            fabs(ctx->calculated_input_fps - ctx->estimated_source_fps) > 1.0) {
                            
                            // Atualizar FPS estimado
                            ctx->estimated_source_fps = ctx->calculated_input_fps;
                            ctx->has_real_fps_measurement = true;
                            
                            // Recalcular skip ratio se necessário
                            if (ctx->target_fps > 0 && ctx->estimated_source_fps > ctx->target_fps) {
                                ctx->frame_skip_ratio = ctx->estimated_source_fps / ctx->target_fps;
                                ctx->frame_skip_count = (int)floor(ctx->frame_skip_ratio);
                                log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] FPS real medido: %.2f, novo skip ratio: %.3f (parte inteira=%d)",
                                            ctx->camera_id, ctx->estimated_source_fps, 
                                            ctx->frame_skip_ratio, ctx->frame_skip_count);
                            }
                        }
                        
                        log_message(LOG_LEVEL_INFO, "[FPS Real ID %d] FPS de Entrada Decodificado (últimos %.1fs): %.2f", 
                                    ctx->camera_id, elapsed_s_input, ctx->calculated_input_fps);
                    } else {
                        log_message(LOG_LEVEL_WARNING, "[FPS Real ID %d] Tempo decorrido para cálculo de FPS de entrada é zero ou negativo (%.3f s).", 
                                    ctx->camera_id, elapsed_s_input);
                    }
                    // Resetar para próximo cálculo
                    ctx->frame_input_counter = 0;
                    ctx->last_input_fps_calc_time = current_time;
                }
                // --- FIM DA MODIFICAÇÃO ---
                
                // --- NOVO: Adicionar frame à fila ---
                log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Adicionando frame à fila (PTS: %ld)",
                            ctx->camera_id, ctx->decoded_frame->pts);
                
                bool push_success = frame_queue_push(&ctx->decoded_frame_queue, ctx->decoded_frame, &ctx->stop_requested);
                if (!push_success) {
                    // Log menos verboso - apenas ocasionalmente
                    static int push_fail_count = 0;
                    push_fail_count++;
                    if (push_fail_count % 20 == 0) {
                        log_message(LOG_LEVEL_WARNING, "[Stream Processing ID %d] Falha ao adicionar frame à fila (fila cheia) - %d falhas", ctx->camera_id, push_fail_count);
                    }
                }
                
                // Liberar frame decodificado após adicionar à fila
                av_frame_unref(ctx->decoded_frame);
                // --- FIM NOVO ---

                // Alocar um novo decoded_frame para a próxima iteração do loop avcodec_receive_frame
                ctx->decoded_frame = av_frame_alloc();
                if (!ctx->decoded_frame) {
                    log_message(LOG_LEVEL_ERROR, "[Stream Processing ID %d] Falha ao re-alocar AVFrame para decodificação.", ctx->camera_id);
                    goto process_stream_error;
                }
            } // Fim while receive_frame (Inner loop)
                 
        } // Fim if stream_index (Outer loop)
        // av_packet_unref(ctx->packet); // Já foi unref após avcodec_send_packet
    } // Fim while (true) do loop de leitura principal

process_stream_error:
    // Se chegamos aqui, ocorreu um erro irrecuperável dentro do loop de processamento
    log_message(LOG_LEVEL_ERROR, "[Stream Processing ID %d] Erro irrecuperável no loop de processamento. Limpando e saindo.", ctx->camera_id);
    // av_packet_unref(ctx->packet); // Já foi unref
    if(ctx->decoded_frame) { av_frame_unref(ctx->decoded_frame); ctx->decoded_frame = NULL; }
    // Não unref ctx->frame_bgr aqui, ele será limpo por cleanup_ffmpeg_resources
    return false; // Indicar erro para reconexão
}

// --- Função Principal da Thread (Adaptada) --- 
void* run_camera_loop(void* arg) {
    camera_thread_context_t* ctx = (camera_thread_context_t*)arg;
    if (!ctx) { 
        log_message(LOG_LEVEL_ERROR, "[Thread] Argumento de contexto NULO recebido! Abortando.");
        return NULL; 
    }
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Iniciada para URL: %s", ctx->camera_id, ctx->url);
    
    // Calcular o intervalo de pacing aqui, baseado no target_fps definido no contexto
    ctx->target_interval_ns = (ctx->target_fps > 0) ? (int64_t)(1.0e9 / ctx->target_fps) : 0;
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Target FPS: %d, Calculated Interval (ns): %ld", 
                ctx->camera_id, ctx->target_fps, ctx->target_interval_ns);

    // --- MODIFICADO: Inicializar tempos para cálculo de FPS e pacing ---
    clock_gettime(CLOCK_MONOTONIC, &ctx->last_output_fps_calc_time); // Para FPS de saída
    clock_gettime(CLOCK_MONOTONIC, &ctx->last_input_fps_calc_time);  // Para FPS de entrada
    clock_gettime(CLOCK_MONOTONIC, &ctx->last_frame_sent_time);      // Para pacing de saída
    // --- FIM DA MODIFICAÇÃO ---

    // --- NOVO: Inicializar fila de frames ---
    if (!frame_queue_init(&ctx->decoded_frame_queue, 0)) {
        log_message(LOG_LEVEL_ERROR, "[Thread ID %d] Falha ao inicializar fila de frames", ctx->camera_id);
        return NULL;
    }
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Fila de frames inicializada", ctx->camera_id);
    // --- FIM NOVO ---

    ctx->state = CAM_STATE_CONNECTING; // Estado inicial
    ctx->last_sent_pts = AV_NOPTS_VALUE; // Inicializar PTS (se usado)

    AVDictionary *opts = NULL; // Mover declaração para fora do loop (já está)

    // Configurar handler de sinais para permitir interrupção
    setup_signal_handler();

    while (true) {
        if (ctx->stop_requested) { 
            log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested at loop start.", ctx->camera_id);
            break; 
        }
        
        // Marcar início da tentativa de inicialização
        ctx->is_initializing = true;
        clock_gettime(CLOCK_MONOTONIC, &ctx->initialization_start_time);
        log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Starting initialization attempt.", ctx->camera_id);
        
        update_camera_status(ctx, CAM_STATE_CONNECTING, "Conectando...");
        if (ctx->stop_requested) { log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after connect status update.", ctx->camera_id); goto thread_exit_cleanup; }
        
        AVDictionary* opts_copy = NULL; // Declaração aqui para cada iteração do loop

        // --- Conexão, Configuração, Alocação --- 
        if (!initialize_ffmpeg_connection(ctx, &opts_copy)) { 
             log_message(LOG_LEVEL_WARNING, "[Thread Loop ID %d] Failed to initialize FFmpeg connection.", ctx->camera_id);
             ctx->is_initializing = false; // Resetar flag antes de ir para reconexão
             goto handle_reconnect; 
        }
        if (!setup_video_decoder(ctx)) { 
             log_message(LOG_LEVEL_WARNING, "[Thread Loop ID %d] Failed to setup video decoder.", ctx->camera_id);
             ctx->is_initializing = false; // Resetar flag antes de ir para reconexão
             goto handle_reconnect; 
        }
        if (!allocate_packets_and_frames(ctx)) { 
             log_message(LOG_LEVEL_ERROR, "[Thread Loop ID %d] Failed to allocate packets/frames.", ctx->camera_id);
             ctx->is_initializing = false; // Resetar flag antes de ir para reconexão
             goto handle_reconnect; 
        }
        
        // --- Conectado --- 
        ctx->is_initializing = false; // Conexão/Inicialização concluída com sucesso!
        log_message(LOG_LEVEL_INFO, "[Thread ID %d] Initialization successful.", ctx->camera_id);
        ctx->reconnect_attempts = 0;
        ctx->last_sent_pts = AV_NOPTS_VALUE; // Resetar PTS ao conectar
        update_camera_status(ctx, CAM_STATE_CONNECTED, "Conectado");
        if (ctx->stop_requested) { log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after connected status update.", ctx->camera_id); goto thread_exit_cleanup; }

        // --- NOVO: Iniciar thread consumidora ---
        int consumer_rc = pthread_create(&ctx->consumer_thread_id, NULL, consume_frames_thread, ctx);
        if (consumer_rc != 0) {
            log_message(LOG_LEVEL_ERROR, "[Thread ID %d] Falha ao criar thread consumidora: %s", ctx->camera_id, strerror(consumer_rc));
            goto thread_exit_cleanup;
        }
        log_message(LOG_LEVEL_INFO, "[Thread ID %d] Thread consumidora iniciada", ctx->camera_id);
        // --- FIM NOVO ---

        // --- Processar --- 
        bool stop_req = process_stream(ctx);
        if (stop_req) {
            goto thread_exit_cleanup; // Sair limpo se process_stream pediu parada
        }
        // Se retornou false, é erro/EOF, então irá para reconexão
        goto handle_reconnect;

handle_reconnect:
        ctx->is_initializing = false; // Garantir que está falso ao entrar na reconexão
        log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Cleaning up for reconnection...", ctx->camera_id);
        cleanup_ffmpeg_resources(ctx); // Passar ctx
        if (ctx->stop_requested) { break; }
        
        update_camera_status(ctx, CAM_STATE_DISCONNECTED, "Conexão perdida/finalizada"); 
        if (ctx->stop_requested) { log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after disconnected status update.", ctx->camera_id); goto thread_exit_cleanup; }
        ctx->reconnect_attempts++; 
        
        // Calcular delay_seconds AQUI
        int delay_seconds = RECONNECT_DELAY_BASE * ctx->reconnect_attempts;
        if (delay_seconds < MIN_RECONNECT_DELAY) delay_seconds = MIN_RECONNECT_DELAY;
        if (delay_seconds > MAX_RECONNECT_DELAY) delay_seconds = MAX_RECONNECT_DELAY;
        
        char reconnect_msg[128]; 
        snprintf(reconnect_msg, sizeof(reconnect_msg), "Aguardando %d s para reconectar (Tentativa %d)", delay_seconds, ctx->reconnect_attempts);
        update_camera_status(ctx, CAM_STATE_WAITING_RECONNECT, reconnect_msg);
        if (ctx->stop_requested) { log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after waiting status update.", ctx->camera_id); goto thread_exit_cleanup; }
        log_message(LOG_LEVEL_INFO, "[Thread ID %d] %s", ctx->camera_id, reconnect_msg);
        
        struct timespec short_sleep = {0, 100 * 1000000}; // 100ms
        time_t start_wait_time = time(NULL);
        while (difftime(time(NULL), start_wait_time) < delay_seconds) {
            if (ctx->stop_requested) { goto thread_exit_cleanup; }
            nanosleep(&short_sleep, NULL); 
        }
        if (ctx->stop_requested) { break; }
        update_camera_status(ctx, CAM_STATE_RECONNECTING, "Reconectando..."); 
        if (ctx->stop_requested) { log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after reconnecting status update.", ctx->camera_id); goto thread_exit_cleanup; }
    } // Fim while

thread_exit_cleanup:
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Saindo do loop principal. Limpando final...", ctx->camera_id);
    
    // --- NOVO: Sinalizar e aguardar thread consumidora ---
    if (ctx->consumer_thread_id) {
        log_message(LOG_LEVEL_INFO, "[Thread ID %d] Sinalizando thread consumidora para parar...", ctx->camera_id);
        ctx->stop_requested = true;
        
        // Sinalizar a fila para desbloquear a consumidora
        pthread_cond_signal(&ctx->decoded_frame_queue.cond_not_empty);
        
        // Aguardar thread consumidora terminar
        int join_rc = pthread_join(ctx->consumer_thread_id, NULL);
        if (join_rc != 0) {
            log_message(LOG_LEVEL_WARNING, "[Thread ID %d] Erro ao aguardar thread consumidora: %s", ctx->camera_id, strerror(join_rc));
        } else {
            log_message(LOG_LEVEL_INFO, "[Thread ID %d] Thread consumidora finalizada", ctx->camera_id);
        }
    }
    // --- FIM NOVO ---
    
    cleanup_ffmpeg_resources(ctx); // Passar ctx
    
    // --- NOVO: Destruir fila de frames ---
    frame_queue_destroy(&ctx->decoded_frame_queue);
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Fila de frames destruída", ctx->camera_id);
    // --- FIM NOVO ---
    
    update_camera_status(ctx, CAM_STATE_STOPPED, "Thread encerrada"); // Passar ctx
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Encerrada completamente.", ctx->camera_id);
    if (opts) av_dict_free(&opts); // Este opts não está sendo usado, pode ser removido
    return NULL;
}