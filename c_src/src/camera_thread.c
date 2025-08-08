#define _POSIX_C_SOURCE 200809L // Define before including headers for clock_gettime

#include "camera_thread.h"
#include "logger.h"
#include "callback_utils.h"
#include "camera_context.h"

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
#include <libavutil/frame.h>

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
        // NÃO marcar stop_requested aqui; apenas interromper a chamada bloqueante
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

    // Preparar dicionário de opções
    if (opts) {
        if (!*opts) { *opts = NULL; }
        // Flags de baixa latência e buffering mínimo (quando suportado)
        av_dict_set(opts, "fflags", "nobuffer", 0);
        av_dict_set(opts, "flags", "low_delay", 0);
        av_dict_set(opts, "avioflags", "direct", 0);
        av_dict_set(opts, "reorder_queue_size", "0", 0);
        av_dict_set(opts, "probesize", "32000", 0);
        av_dict_set(opts, "analyzeduration", "0", 0);

        // --- NOVO: Otimizações de rede/HTTP/TCP ---
        av_dict_set(opts, "user_agent", "camera-pipeline/1.0", 0);
        // Evitar operações de seek em streams ao vivo
        av_dict_set(opts, "seekable", "0", 0);
        // Timeout de leitura/escrita (microsegundos)
        av_dict_set(opts, "rw_timeout", "10000000", 0); // 10s
        // Reconnect agressivo para HTTP/HLS
        av_dict_set(opts, "reconnect", "1", 0);
        av_dict_set(opts, "reconnect_streamed", "1", 0);
        av_dict_set(opts, "reconnect_delay_max", "2", 0);
        // Manter conexões HTTP persistentes e permitir múltiplas requisições
        av_dict_set(opts, "http_persistent", "1", 0);
        av_dict_set(opts, "multiple_requests", "1", 0);
        // TCP: reduzir latência
        av_dict_set(opts, "tcp_nodelay", "1", 0);
        // TLS (streams de teste usam HTTPS) – desabilitar verificação em ambiente de teste pode reduzir falhas
        av_dict_set(opts, "tls_verify", "0", 0);
        // --- FIM NOVO ---
    }

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
    ctx->codec_ctx->thread_count = 1; // Mais previsível e leve para multi-câmeras
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

    // --- Inicialização de FPS/skip existentes ---
    AVStream* video_stream = ctx->fmt_ctx->streams[ctx->video_stream_index];
    AVRational frame_rate = av_guess_frame_rate(ctx->fmt_ctx, video_stream, NULL);
    double detected_fps_from_metadata = av_q2d(frame_rate);

    if (detected_fps_from_metadata > 4.0 && detected_fps_from_metadata < 65.0) {
        ctx->estimated_source_fps = detected_fps_from_metadata;
        log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] FPS inicial da fonte de metadados: %.2f (será ajustado com medição real).", 
                    ctx->camera_id, ctx->estimated_source_fps);
    } else {
        ctx->estimated_source_fps = 30.0;
        log_message(LOG_LEVEL_WARNING, "[Frame Skip ID %d] FPS da fonte de metadados (%.2f) parece irreal. Usando 30.0 FPS inicial (será ajustado com medição real).", 
                    ctx->camera_id, detected_fps_from_metadata);
    }

    ctx->frame_input_counter = 0;
    clock_gettime(CLOCK_MONOTONIC, &ctx->last_input_fps_calc_time);
    ctx->calculated_input_fps = 0.0;
    ctx->frame_skip_ratio = 1.0; // Começar sem skip
    ctx->frame_skip_count = 1;
    ctx->frame_skip_accumulator = 0.0;
    ctx->frame_process_counter = 0;
    ctx->has_real_fps_measurement = false;

    if (ctx->target_fps <= 0 || ctx->estimated_source_fps <= 0 || ctx->target_fps >= ctx->estimated_source_fps) {
        ctx->frame_skip_ratio = 1.0;
        ctx->frame_skip_count = 1; // Enviar todos
        ctx->frame_skip_accumulator = 0.0;
        log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] TargetFPS (%d) desativado ou >= FPS da fonte (%.2f). FrameSkipRatio=1.0 (Não pular).",
                    ctx->camera_id, ctx->target_fps, ctx->estimated_source_fps);
    } else {
        ctx->frame_skip_ratio = ctx->estimated_source_fps / ctx->target_fps;
        ctx->frame_skip_count = (int)floor(ctx->frame_skip_ratio);
        ctx->frame_skip_accumulator = 0.0;
        log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] Configurado: TargetFPS=%d, SourceFPS=%.2f, FrameSkipRatio=%.3f (Parte inteira=%d)",
                    ctx->camera_id, ctx->target_fps, ctx->estimated_source_fps, ctx->frame_skip_ratio, ctx->frame_skip_count);
    }

    // --- NOVO: Inicializar sincronização por PTS e thresholds ---
    ctx->pts_time_base = av_q2d(video_stream->time_base);
    ctx->first_pts = AV_NOPTS_VALUE;
    ctx->last_sent_pts = AV_NOPTS_VALUE;
    ctx->last_sent_pts_sec = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &ctx->playback_anchor_mono);

    // Thresholds default
    ctx->early_sleep_threshold_sec = 0.050;       // dormir se adiantado mais que 50ms
    ctx->lateness_catchup_threshold_sec = 0.200;  // não dormir se atrasado mais que 200ms
    ctx->pts_jump_reset_threshold_sec = 1.000;    // reset de âncora se salto de PTS > 1s
    ctx->stall_timeout_sec = 30.0;                // stall após 30s sem atividade

    clock_gettime(CLOCK_MONOTONIC, &ctx->last_activity_mono);

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
        // Se dimensões/format não mudaram, tentar reutilizar buffer
        if (ctx->frame_bgr->width == frame_to_convert->width &&
            ctx->frame_bgr->height == frame_to_convert->height &&
            ctx->frame_bgr->format == AV_PIX_FMT_BGR24) {
            if (av_frame_make_writable(ctx->frame_bgr) < 0) {
                av_frame_unref(ctx->frame_bgr);
            }
        } else {
            av_frame_unref(ctx->frame_bgr);
        }
    }

    ctx->frame_bgr->width = frame_to_convert->width;
    ctx->frame_bgr->height = frame_to_convert->height;
    ctx->frame_bgr->format = AV_PIX_FMT_BGR24;
    ctx->frame_bgr->pts = frame_to_convert->pts; 
    log_message(LOG_LEVEL_TRACE, "[Dispatch] Garantindo buffer BGR...");
    if (!ctx->frame_bgr->data[0]) {
        ret = av_frame_get_buffer(ctx->frame_bgr, 1); 
        if (ret < 0) {
            log_ffmpeg_error(LOG_LEVEL_ERROR, "[SWS] Falha ao alocar buffer para frame BGR", ret); 
            goto dispatch_cleanup_and_fail;
        }
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
    LOG_HEARTBEAT(ctx->camera_id, "stream_processor");
    int ret = 0;

    // --- MODIFICADO: Reiniciar o frame_process_counter ao entrar em process_stream ---
    ctx->frame_process_counter = 0; 
    // --- FIM DA MODIFICAÇÃO ---

    while (true) {
        if (ctx->stop_requested) {
            log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Parada solicitada no início do loop.", ctx->camera_id);
            return true; // Parada solicitada, não é um erro
        }

        // --- Verificar paradas de processamento ---
        if (check_processing_stall(ctx->camera_id, 30)) { // 30 segundos de timeout
            log_message(LOG_LEVEL_ERROR, "[Stream Processing ID %d] PARADA CRÍTICA detectada - forçando reconexão", ctx->camera_id);
            LOG_ACTIVITY(ctx->camera_id, "stall_recovery", 0.0);
            return false; // Forçar reconexão imediata
        }

        // --- Leitura --- 
        struct timespec read_start, read_end;
        clock_gettime(CLOCK_MONOTONIC, &read_start);
        
        log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] Aguardando av_read_frame...", ctx->camera_id);
        ret = av_read_frame(ctx->fmt_ctx, ctx->packet);
        
        clock_gettime(CLOCK_MONOTONIC, &read_end);
        double read_time_ms = timespec_diff_s(&read_start, &read_end) * 1000.0;
        clock_gettime(CLOCK_MONOTONIC, &ctx->last_activity_mono);
        
        log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] av_read_frame retornou %d (%.2fms)", ctx->camera_id, ret, read_time_ms);
        
        // Log de atividade para tracking
        if (ret >= 0) {
            LOG_ACTIVITY(ctx->camera_id, "frame_read", read_time_ms);
        } else {
            LOG_ACTIVITY(ctx->camera_id, "error", 0.0);
        }

        if (ret == AVERROR_EOF) {
            log_message(LOG_LEVEL_INFO, "[Stream Processing ID %d] Fim do stream (EOF).", ctx->camera_id);
            LOG_ACTIVITY(ctx->camera_id, "eof", 0.0);
            return false; // Fim do stream, precisa reconectar (ou parar se stop_requested for true logo após)
        } else if (ret == AVERROR(EAGAIN)) {
            log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] av_read_frame retornou EAGAIN, tentando novamente...", ctx->camera_id);
            LOG_ACTIVITY(ctx->camera_id, "eagain", 0.0);
            av_packet_unref(ctx->packet);
            continue; // Tentar ler novamente
        } else if (ret < 0) {
            log_ffmpeg_error(LOG_LEVEL_ERROR, "[Stream Processing ID %d] Falha ao ler frame", ctx->camera_id);
            LOG_ACTIVITY(ctx->camera_id, "error", 0.0);
            return false; // Erro de leitura, precisa reconectar
        }

        // --- Processamento do Pacote --- 
        if (ctx->packet->stream_index == ctx->video_stream_index) {
            struct timespec decode_start, decode_end;
            clock_gettime(CLOCK_MONOTONIC, &decode_start);
            
            log_message(LOG_LEVEL_TRACE, "[Stream Processing ID %d] Enviando pacote (PTS: %ld)", ctx->camera_id, ctx->packet->pts);
            ret = avcodec_send_packet(ctx->codec_ctx, ctx->packet);
            
            clock_gettime(CLOCK_MONOTONIC, &decode_end);
            double decode_time_ms = timespec_diff_s(&decode_start, &decode_end) * 1000.0;
            
            // Consumir o pacote imediatamente após avcodec_send_packet
            av_packet_unref(ctx->packet); // Libera o pacote, seja qual for o resultado de send_packet
            
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                 log_ffmpeg_error(LOG_LEVEL_WARNING, "[Stream Processing ID %d] Erro ao enviar pacote para decodificador", ctx->camera_id);
                 LOG_ACTIVITY(ctx->camera_id, "warning", 0.0);
                 // Continuar para tentar receber frames que já possam estar no buffer, mas logar o aviso
            } else {
                LOG_ACTIVITY(ctx->camera_id, "packet_decode", decode_time_ms);
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
                    LOG_ACTIVITY(ctx->camera_id, "error", 0.0);
                    goto process_stream_error; 
                }

                // CORREÇÃO: Usar decoded_frame nas validações e logs
                log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Frame DECODIFICADO. PTS: %ld", 
                            ctx->camera_id, ctx->decoded_frame->pts);
                clock_gettime(CLOCK_MONOTONIC, &ctx->last_activity_mono);
                
                // Log de atividade para frame decodificado
                LOG_ACTIVITY(ctx->camera_id, "frame", 0.0);
                
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
                
                // --- LÓGICA FRAME SKIPPING MELHORADA COM BASE EM TIMESTAMPS ---
                ctx->frame_process_counter++; // Incrementa para cada frame DECODIFICADO
                
                // Determinar se devemos enviar este frame
                bool should_send = false;
                
                // Obter o timestamp atual do frame
                int64_t current_pts = ctx->decoded_frame->pts;
                
                // Se não temos um PTS válido, usar o contador como fallback
                if (current_pts == AV_NOPTS_VALUE) {
                    // Usar a lógica antiga baseada em contador
                    ctx->frame_skip_accumulator += 1.0;
                    
                    if (ctx->frame_skip_ratio <= 1.0) {
                        // Se a razão é <= 1, enviamos todos os frames
                        should_send = true;
                    } else {
                        // Se acumulamos o suficiente para um frame inteiro
                        if (ctx->frame_skip_accumulator >= ctx->frame_skip_ratio) {
                            should_send = true;
                            // Subtrair um frame inteiro do acumulador
                            ctx->frame_skip_accumulator -= ctx->frame_skip_ratio;
                        }
                    }
                    
                    log_message(LOG_LEVEL_DEBUG, "[Frame Skip ID %d] Frame %ld: PTS inválido, usando acumulador=%.3f, Ratio=%.3f, Send=%d",
                                ctx->camera_id, ctx->frame_process_counter,
                                ctx->frame_skip_accumulator, ctx->frame_skip_ratio, should_send);
                } else {
                    // Usar lógica baseada em timestamp para evitar pulos no relógio
                    
                    // Se é o primeiro frame ou se o último PTS enviado não é válido
                    if (ctx->first_pts == AV_NOPTS_VALUE) {
                        should_send = true;
                        log_message(LOG_LEVEL_DEBUG, "[Frame Skip ID %d] Primeiro frame ou reset, enviando frame com PTS: %ld",
                                    ctx->camera_id, current_pts);
                    } else {
                        // Calcular o intervalo de tempo desde o último frame enviado
                        // Nota: Isso assume que os PTS estão em unidades de tempo consistentes
                        AVStream* video_stream = ctx->fmt_ctx->streams[ctx->video_stream_index];
                        double pts_time_base = av_q2d(video_stream->time_base);
                        double pts_diff = (current_pts - ctx->last_sent_pts) * pts_time_base;
                        
                        // Calcular o intervalo de tempo desejado entre frames enviados
                        double target_interval = (ctx->target_fps > 0) ? (1.0 / ctx->target_fps) :
                                                 (ctx->estimated_source_fps > 0 ? 1.0 / ctx->estimated_source_fps : 0.033);
                        
                        // Enviar o frame se o intervalo de tempo desde o último frame enviado
                        // é maior ou igual ao intervalo desejado
                        if (ctx->last_sent_pts == AV_NOPTS_VALUE || pts_diff >= target_interval) {
                            should_send = true;
                            log_message(LOG_LEVEL_DEBUG, "[Frame Skip ID %d] Intervalo PTS: %.3fs >= Target: %.3fs, enviando frame com PTS: %ld",
                                        ctx->camera_id, pts_diff, target_interval, current_pts);
                        } else {
                            log_message(LOG_LEVEL_DEBUG, "[Frame Skip ID %d] Intervalo PTS: %.3fs < Target: %.3fs, pulando frame com PTS: %ld",
                                        ctx->camera_id, pts_diff, target_interval, current_pts);
                        }
                    }
                }

                log_message(LOG_LEVEL_DEBUG, "[Frame Skip ID %d] Frame %ld: Accumulator=%.3f, Ratio=%.3f, Send=%d", 
                            ctx->camera_id, ctx->frame_process_counter, 
                            ctx->frame_skip_accumulator, ctx->frame_skip_ratio, should_send);

                if (should_send) {
                    // Log de ENVIO
                    log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] ENVIANDO frame (PTS: %ld)",
                                ctx->camera_id, ctx->decoded_frame->pts);
                    
                    struct timespec dispatch_start, dispatch_end;
                    clock_gettime(CLOCK_MONOTONIC, &dispatch_start);
                    
                    // --- NOVO: Apresentação baseada em PTS + âncora monotônica com catch-up ---
                    if (current_pts != AV_NOPTS_VALUE && ctx->pts_time_base > 0.0) {
                        if (ctx->first_pts == AV_NOPTS_VALUE) {
                            ctx->first_pts = current_pts;
                            clock_gettime(CLOCK_MONOTONIC, &ctx->playback_anchor_mono);
                        }
                        double pts_sec = (current_pts - ctx->first_pts) * ctx->pts_time_base;

                        // Detectar salto grande de PTS e realinhar âncora
                        double last_sec = ctx->last_sent_pts_sec;
                        if (ctx->last_sent_pts != AV_NOPTS_VALUE) {
                            double jump = fabs(pts_sec - last_sec);
                            if (jump > ctx->pts_jump_reset_threshold_sec) {
                                // Realinhar âncora agora para evitar salto aparente
                                clock_gettime(CLOCK_MONOTONIC, &ctx->playback_anchor_mono);
                                ctx->first_pts = current_pts;
                                pts_sec = 0.0;
                            }
                        }

                        // Tempo alvo de apresentação absoluto
                        struct timespec target_ts = ctx->playback_anchor_mono;
                        time_t add_sec = (time_t)pts_sec;
                        long add_nsec = (long)((pts_sec - (double)add_sec) * 1000000000L);
                        target_ts.tv_sec += add_sec;
                        target_ts.tv_nsec += add_nsec;
                        if (target_ts.tv_nsec >= 1000000000L) {
                            target_ts.tv_sec += target_ts.tv_nsec / 1000000000L;
                            target_ts.tv_nsec = target_ts.tv_nsec % 1000000000L;
                        }

                        struct timespec now_ts;
                        clock_gettime(CLOCK_MONOTONIC, &now_ts);

                        double lateness_sec = (double)(now_ts.tv_sec - target_ts.tv_sec) +
                                              (double)(now_ts.tv_nsec - target_ts.tv_nsec) / 1e9;

                        if (lateness_sec < -ctx->early_sleep_threshold_sec) {
                            (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target_ts, NULL);
                        }
                        // Se atrasado muito, envía já (sem dormir)
                        ctx->last_sent_pts_sec = pts_sec;
                    }

                    // CHAVE: O frame_decoded É consumido por convert_and_dispatch_frame, que também libera seu buffer
                    bool callback_ok = convert_and_dispatch_frame(ctx, ctx->decoded_frame);
                    
                    clock_gettime(CLOCK_MONOTONIC, &dispatch_end);
                    double dispatch_time_ms = timespec_diff_s(&dispatch_start, &dispatch_end) * 1000.0;
                    
                    // Atualizar o último PTS enviado para evitar pulos no relógio
                    if (callback_ok && current_pts != AV_NOPTS_VALUE) {
                        ctx->last_sent_pts = current_pts;
                    }
                    clock_gettime(CLOCK_MONOTONIC, &ctx->last_activity_mono);
                    
                    av_frame_unref(ctx->decoded_frame); // Libera o frame *APÓS* o dispatch (se não foi movido, etc.)
                    
                    // Log de atividade para dispatch
                    LOG_ACTIVITY(ctx->camera_id, "frame_dispatch", dispatch_time_ms);
                    
                    // Resetar contador APÓS envio bem sucedido
                    ctx->frame_process_counter = 0;

                    if (!callback_ok) { // Se o callback/conversão falhar, é um erro a ser tratado
                        log_message(LOG_LEVEL_ERROR, "[Loop Leitura ID %d] Falha na conversão ou callback após seleção de frame.", ctx->camera_id);
                        LOG_ACTIVITY(ctx->camera_id, "error", 0.0);
                        goto process_stream_error;
                    }
                    
                    // --- Pacing (Tempo Real) APÓS envio --- 
                    // Evitar dupla espera: se sincronizado por PTS, NÃO aplicar pacing adicional
                    struct timespec now; // garantir existência para usos seguintes
                    if (!(current_pts != AV_NOPTS_VALUE && ctx->pts_time_base > 0.0)) {
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        long elapsed_since_last_sent_ns = (long)(timespec_diff_s(&ctx->last_frame_sent_time, &now) * 1e9);
                        long sleep_needed_ns = ctx->target_interval_ns - elapsed_since_last_sent_ns;
                        if (sleep_needed_ns > 0) {
                            time_t sec = (time_t)(sleep_needed_ns / 1000000000L);
                            long nsec = (long)(sleep_needed_ns % 1000000000L);
                            struct timespec remaining_sleep = {sec, nsec};
                            struct timespec actual_slept = {0, 0};
                            while (nanosleep(&remaining_sleep, &actual_slept) == -1 && errno == EINTR) {
                                if (ctx->stop_requested) {
                                    return true; // Sair limpo
                                }
                                remaining_sleep = actual_slept; 
                            }
                        }
                    } else {
                        // se sincronizado por PTS, ainda precisamos de 'now' para métricas
                        clock_gettime(CLOCK_MONOTONIC, &now);
                    }

                    // --- Calcular FPS de Saída --- 
                    ctx->frame_send_counter++;
                    double elapsed_s_output = timespec_diff_s(&ctx->last_output_fps_calc_time, &now);
                    if (elapsed_s_output >= FPS_CALC_INTERVAL_S) {
                        if (elapsed_s_output > 0) {
                            ctx->calculated_output_fps = (double)ctx->frame_send_counter / elapsed_s_output;
                            log_message(LOG_LEVEL_INFO, "[FPS Real ID %d] FPS de Saída Calculado (últimos %.1fs): %.2f", ctx->camera_id, elapsed_s_output, ctx->calculated_output_fps);
                        } else {
                            log_message(LOG_LEVEL_ERROR, "[FPS Real ID %d] Tempo decorrido para cálculo de FPS de saída é zero ou negativo (%.3f s).", ctx->camera_id, elapsed_s_output);
                        }
                        ctx->frame_send_counter = 0;
                        ctx->last_output_fps_calc_time = now;
                    }
                    // --- Fim Cálculo FPS de Saída ---

                } else {
                    // Log de SKIP
                    log_message(LOG_LEVEL_DEBUG, "[Frame Skip ID %d] PULANDO frame (Contador %ld < Skip %d, PTS: %ld)",
                                ctx->camera_id, ctx->frame_process_counter, ctx->frame_skip_count, ctx->decoded_frame->pts);
                                
                    // CORREÇÃO: Liberar decoded_frame ao pular
                    av_frame_unref(ctx->decoded_frame);
                }

                // Alocar um novo decoded_frame para a próxima iteração do loop avcodec_receive_frame
                // Reutilizar o mesmo AVFrame; apenas limpar referência
                av_frame_unref(ctx->decoded_frame);
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
    
    // Inicializar logger para esta câmera
    char log_filename[256];
    snprintf(log_filename, sizeof(log_filename), "camera_pipeline_%d.log", ctx->camera_id);
    if (logger_init(log_filename, 100, true)) { // 100MB max, performance tracking habilitado
        log_message(LOG_LEVEL_INFO, "[Logger] Sistema de logging inicializado para câmera %d", ctx->camera_id);
    } else {
        log_message(LOG_LEVEL_WARNING, "[Logger] Falha ao inicializar logging em disco para câmera %d", ctx->camera_id);
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

    ctx->state = CAM_STATE_CONNECTING; // Estado inicial
    ctx->last_sent_pts = AV_NOPTS_VALUE; // Inicializar PTS (se usado)

    AVDictionary *opts = NULL; // Mover declaração para fora do loop (já está)

    // Configurar handler de sinais para permitir interrupção
    setup_signal_handler();

    while (1) { // Loop externo para reconexão infinita, só sai se for stop_requested
        log_message(LOG_LEVEL_INFO, "[Thread ID %d] >>>>> INÍCIO DO LOOP EXTERNO DE RECONEXÃO <<<<<", ctx->camera_id);
        while (true) {
            if (ctx->stop_requested) { 
                log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested at loop start. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; 
            }
            
            // Marcar início da tentativa de inicialização
            ctx->is_initializing = true;
            clock_gettime(CLOCK_MONOTONIC, &ctx->initialization_start_time);
            log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Starting initialization attempt.", ctx->camera_id);
            
            update_camera_status(ctx, CAM_STATE_CONNECTING, "Conectando...");
            if (ctx->stop_requested) { 
                log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after connect status update. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; 
            }
            
            AVDictionary* opts_copy = NULL; // Declaração aqui para cada iteração do loop

            // --- Conexão, Configuração, Alocação --- 
            if (!initialize_ffmpeg_connection(ctx, &opts_copy)) { 
                log_message(LOG_LEVEL_WARNING, "[Thread Loop ID %d] Failed to initialize FFmpeg connection. Indo para reconexão.", ctx->camera_id);
                ctx->is_initializing = false; // Resetar flag antes de ir para reconexão
                goto handle_reconnect; 
            }
            if (!setup_video_decoder(ctx)) { 
                log_message(LOG_LEVEL_WARNING, "[Thread Loop ID %d] Failed to setup video decoder. Indo para reconexão.", ctx->camera_id);
                ctx->is_initializing = false; // Resetar flag antes de ir para reconexão
                goto handle_reconnect; 
            }
            if (!allocate_packets_and_frames(ctx)) { 
                log_message(LOG_LEVEL_ERROR, "[Thread Loop ID %d] Failed to allocate packets/frames. Indo para reconexão.", ctx->camera_id);
                ctx->is_initializing = false; // Resetar flag antes de ir para reconexão
                goto handle_reconnect; 
            }
            
            // --- Conectado --- 
            ctx->is_initializing = false; // Conexão/Inicialização concluída com sucesso!
            log_message(LOG_LEVEL_INFO, "[Thread ID %d] Initialization successful.", ctx->camera_id);
            ctx->reconnect_attempts = 0;
            ctx->last_sent_pts = AV_NOPTS_VALUE; // Resetar PTS ao conectar
            update_camera_status(ctx, CAM_STATE_CONNECTED, "Conectado");
            if (ctx->stop_requested) { 
                log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after connected status update. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; 
            }

            // --- Processar --- 
            LOG_HEARTBEAT(ctx->camera_id, "main_loop");
            bool stop_req = process_stream(ctx);
            if (stop_req) {
                log_message(LOG_LEVEL_WARNING, "[Thread ID %d] process_stream retornou stop_req=true. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; // Sair limpo se process_stream pediu parada
            }
            // Se retornou false, é erro/EOF, então irá para reconexão
            log_message(LOG_LEVEL_WARNING, "[Thread ID %d] process_stream retornou false (erro/EOF). Indo para reconexão.", ctx->camera_id);
            goto handle_reconnect;

        handle_reconnect:
            ctx->is_initializing = false; // Garantir que está falso ao entrar na reconexão
            log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Cleaning up for reconnection...", ctx->camera_id);
            cleanup_ffmpeg_resources(ctx); // Passar ctx
            if (ctx->stop_requested) { 
                log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested during reconnection. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; 
            }
            
            update_camera_status(ctx, CAM_STATE_DISCONNECTED, "Conexão perdida/finalizada"); 
            if (ctx->stop_requested) { 
                log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after disconnected status update. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; 
            }
            ctx->reconnect_attempts++; 
            
            // Calcular delay_seconds AQUI
            int delay_seconds = RECONNECT_DELAY_BASE * ctx->reconnect_attempts;
            if (delay_seconds < MIN_RECONNECT_DELAY) delay_seconds = MIN_RECONNECT_DELAY;
            if (delay_seconds > MAX_RECONNECT_DELAY) delay_seconds = MAX_RECONNECT_DELAY;
            
            char reconnect_msg[128]; 
            snprintf(reconnect_msg, sizeof(reconnect_msg), "Aguardando %d s para reconectar (Tentativa %d)", delay_seconds, ctx->reconnect_attempts);
            update_camera_status(ctx, CAM_STATE_WAITING_RECONNECT, reconnect_msg);
            if (ctx->stop_requested) { 
                log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after waiting status update. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; 
            }
            log_message(LOG_LEVEL_INFO, "[Thread ID %d] %s", ctx->camera_id, reconnect_msg);
            
            struct timespec short_sleep = {0, 100 * 1000000}; // 100ms
            time_t start_wait_time = time(NULL);
            while (difftime(time(NULL), start_wait_time) < delay_seconds) {
                if (ctx->stop_requested) { 
                    log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested during reconnect wait. Saindo do loop principal.", ctx->camera_id);
                    goto thread_exit_cleanup; 
                }
                nanosleep(&short_sleep, NULL); 
            }
            if (ctx->stop_requested) { 
                log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after reconnect wait. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; 
            }
            update_camera_status(ctx, CAM_STATE_RECONNECTING, "Reconectando..."); 
            if (ctx->stop_requested) { 
                log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after reconnecting status update. Saindo do loop principal.", ctx->camera_id);
                goto thread_exit_cleanup; 
            }
            // Após reconexão, volta para o início do while(true) interno
            log_message(LOG_LEVEL_INFO, "[Thread ID %d] Tentando reconectar agora...", ctx->camera_id);
        } // Fim while(true) interno
        // Se saiu do while(true) interno sem ser por stop_requested, logar e reiniciar loop externo
        log_message(LOG_LEVEL_ERROR, "[Thread ID %d] Saiu do loop principal sem stop_requested! Reiniciando loop externo para reconexão.", ctx->camera_id);
    } // Fim while(1) externo

thread_exit_cleanup:
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Saindo do loop principal. Limpando final...", ctx->camera_id);
    cleanup_ffmpeg_resources(ctx); // Passar ctx
    update_camera_status(ctx, CAM_STATE_STOPPED, "Thread encerrada"); // Passar ctx
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Encerrada completamente.", ctx->camera_id);
    if (opts) av_dict_free(&opts); // Este opts não está sendo usado, pode ser removido
    return NULL;
}