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

// Constantes para lógica de reconexão
#define RECONNECT_DELAY_BASE 2
#define MIN_RECONNECT_DELAY 1 
#define MAX_RECONNECT_DELAY 30
// Nova constante para timeout de inicialização
#define INITIALIZATION_TIMEOUT_SECONDS 30

// --- Protótipo de Função Auxiliar ---
double timespec_diff_s(struct timespec *start, struct timespec *end); // Declaração antecipada

// --- Constantes e Tipos Internos --- 
#define CAMERA_ID 0 // ID fixo

// --- Funções Auxiliares Internas (Adaptadas para receber ctx) --- 

// Helper function to get monotonic time (já é estática, sem globais)
static inline int64_t get_monotonic_time_ns() { /* ... */ }

// Limpa recursos FFmpeg de um contexto específico
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

// Função utilitária para calcular diferença de tempo (VERIFICAR SE JÁ EXISTE)
// Se já existir, não precisa adicionar novamente.
// double timespec_diff_s(struct timespec *start, struct timespec *end) {
//     return (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1.0e9;
// }

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

// --- Novas Funções Auxiliares para a Lógica da Thread (já recebem ctx) ---

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
    int ret = avformat_open_input(&ctx->fmt_ctx, ctx->url, NULL, opts); // Passa opts aqui
    if (ret < 0) {
        log_ffmpeg_error(LOG_LEVEL_ERROR, "[FFmpeg Init ID %d] Falha ao abrir input", ret);
        if (ctx->fmt_ctx) avformat_close_input(&ctx->fmt_ctx); 
        ctx->fmt_ctx = NULL;
        av_dict_free(opts); // Liberar opts em caso de falha aqui
        *opts = NULL;
        return false;
    }
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] avformat_open_input SUCESSO.", ctx->camera_id);

    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Init ID %d] Buscando stream info...", ctx->camera_id);
    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    av_dict_free(opts); // Liberar opts após o uso bem-sucedido ou falha de find_stream_info
    *opts = NULL;
    if (ret < 0) {
        log_ffmpeg_error(LOG_LEVEL_ERROR, "[FFmpeg Init ID %d] Falha ao buscar stream info", ret);
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
    ctx->codec_ctx->thread_count = 0;
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder ID %d] Thread count definido para %d.", ctx->camera_id, ctx->codec_ctx->thread_count);

    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder ID %d] Abrindo codec...", ctx->camera_id);
    ret = avcodec_open2(ctx->codec_ctx, ctx->codec, NULL);
    if (ret < 0) {
        log_ffmpeg_error(LOG_LEVEL_ERROR, "[FFmpeg Decoder ID %d] Falha ao abrir codec", ret);
        avcodec_free_context(&ctx->codec_ctx);
        ctx->codec_ctx = NULL;
        return false;
    }
    log_message(LOG_LEVEL_DEBUG, "[FFmpeg Decoder ID %d] avcodec_open2 SUCESSO.", ctx->camera_id);

    // --- Estimar FPS da Fonte --- 
    AVStream* video_stream = ctx->fmt_ctx->streams[ctx->video_stream_index];
    AVRational frame_rate = av_guess_frame_rate(ctx->fmt_ctx, video_stream, NULL);
    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        log_message(LOG_LEVEL_WARNING, "[Frame Skip] FPS da fonte não detectado. Usando padrão 30 FPS para cálculo de skip.");
        ctx->estimated_source_fps = 30.0;
    } else {
        ctx->estimated_source_fps = av_q2d(frame_rate);
        if (ctx->estimated_source_fps <= 0) {
             log_message(LOG_LEVEL_WARNING, "[Frame Skip] FPS da fonte calculado inválido (%.2f). Usando padrão 30 FPS.", ctx->estimated_source_fps);
             ctx->estimated_source_fps = 30.0;
        }
    }
    log_message(LOG_LEVEL_INFO, "[Frame Skip] FPS estimado da fonte: %.2f", ctx->estimated_source_fps);

    // --- Calcular Frame Skip Count --- 
    if (ctx->target_fps <= 0 || ctx->estimated_source_fps <= 0 || ctx->target_fps >= ctx->estimated_source_fps) {
        ctx->frame_skip_count = 1; // Enviar todos
    } else {
        // Calcular quantos processar para enviar 1 (arredondando)
        ctx->frame_skip_count = (int)round(ctx->estimated_source_fps / ctx->target_fps);
        if (ctx->frame_skip_count < 1) { // Garantir que seja pelo menos 1
            ctx->frame_skip_count = 1;
        }
    }
    log_message(LOG_LEVEL_INFO, "[Frame Skip] Configurado: TargetFPS=%d, SourceFPS=%.2f, FrameSkipCount=%d (Enviar 1 a cada %d)", 
                ctx->target_fps, ctx->estimated_source_fps, ctx->frame_skip_count, ctx->frame_skip_count);

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
        if (frame_to_convert) av_frame_unref(frame_to_convert);
        return false; 
    }
    if (!frame_to_convert->data[0]) {
        log_message(LOG_LEVEL_ERROR, "[Dispatch] Frame decodificado inválido na entrada (data[0] é NULL).");
        av_frame_unref(frame_to_convert);
        return false;
    }
    if (frame_to_convert->linesize[0] <= 0) {
        log_message(LOG_LEVEL_ERROR, "[Dispatch] Frame decodificado inválido na entrada (linesize[0]=%d <= 0).");
        av_frame_unref(frame_to_convert);
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
                    frame_to_convert->width, frame_to_convert->height);
        ctx->sws_ctx = sws_getContext(frame_to_convert->width, frame_to_convert->height, frame_to_convert->format, 
                                  frame_to_convert->width, frame_to_convert->height, AV_PIX_FMT_BGR24,
                                  SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!ctx->sws_ctx) { 
            log_message(LOG_LEVEL_ERROR, "[SWS] Falha ao criar SwsContext"); 
            goto dispatch_cleanup_and_fail;
        }
        ctx->sws_ctx_width = frame_to_convert->width;
        ctx->sws_ctx_height = frame_to_convert->height;
        ctx->sws_ctx_in_fmt = frame_to_convert->format;
    }
    log_message(LOG_LEVEL_TRACE, "[Dispatch] SwsContext OK.");

    // --- Alocação BGR --- 
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
                ctx->frame_bgr->linesize[0]);
    
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
        } else { 
            log_message(LOG_LEVEL_ERROR, "[Dispatch ID %d] Falha ao criar dados de callback (cb_data nulo).", ctx->camera_id); 
        }
    } else { 
        log_message(LOG_LEVEL_TRACE, "[Dispatch] Callback Python não definido."); 
    }

dispatch_cleanup:
    av_frame_unref(frame_to_convert); 
    av_frame_unref(ctx->frame_bgr);
    return frame_sent;

dispatch_cleanup_and_fail:
    if (frame_to_convert) av_frame_unref(frame_to_convert);
    av_frame_unref(ctx->frame_bgr);
    return false;
}

// --- Constante para intervalo de cálculo de FPS ---
#define FPS_CALC_INTERVAL_S 5.0

// Função utilitária para calcular diferença de tempo
double timespec_diff_s(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1.0e9;
}

// Processa o stream lendo pacotes, decodificando, convertendo e chamando o callback.
// Retorna true se a parada foi solicitada, false em caso de erro que necessite reconexão.
static bool process_stream(camera_thread_context_t* ctx) {
    if (!ctx) return false;
    log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Iniciando loop de processamento...", ctx->camera_id);
    int ret = 0;

    while (true) {
        if (ctx->stop_requested) {
            log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Parada solicitada no início do loop.", ctx->camera_id);
            return true; // Parada solicitada, não é um erro
        }

        // --- Leitura --- 
        log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Aguardando av_read_frame...", ctx->camera_id);
        ret = av_read_frame(ctx->fmt_ctx, ctx->packet);
        log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] av_read_frame retornou %d", ctx->camera_id, ret);

        if (ret == AVERROR_EOF) {
            log_message(LOG_LEVEL_INFO, "[Stream Processing] Fim do stream (EOF).");
            return false; // Fim do stream, precisa reconectar (ou parar se stop_requested for true logo após)
        } else if (ret == AVERROR(EAGAIN)) {
            log_message(LOG_LEVEL_TRACE, "[Stream Processing] av_read_frame retornou EAGAIN, tentando novamente...");
            av_packet_unref(ctx->packet);
            continue; // Tentar ler novamente
        } else if (ret < 0) {
            log_ffmpeg_error(LOG_LEVEL_ERROR, "[Stream Processing] Falha ao ler frame", ret);
            return false; // Erro de leitura, precisa reconectar
        }

        // --- Processamento do Pacote --- 
        if (ctx->packet->stream_index == ctx->video_stream_index) {
            log_message(LOG_LEVEL_TRACE, "[Stream Processing] Enviando pacote (PTS: %ld)", ctx->packet->pts);
            ret = avcodec_send_packet(ctx->codec_ctx, ctx->packet);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                 log_ffmpeg_error(LOG_LEVEL_WARNING, "[Stream Processing] Erro ao enviar pacote para decodificador", ret);
                 // Continuar para tentar receber frames que já possam estar no buffer
            }

            // --- Loop de Recebimento de Frames --- 
            while (true) { // Loop para receber todos os frames gerados pelo pacote
                log_message(LOG_LEVEL_TRACE, "[Stream Processing] Tentando receber frame...");
                struct timespec pre_receive_ts;
                clock_gettime(CLOCK_MONOTONIC, &pre_receive_ts);
                ret = avcodec_receive_frame(ctx->codec_ctx, ctx->decoded_frame);
                struct timespec post_receive_ts;
                clock_gettime(CLOCK_MONOTONIC, &post_receive_ts);
                
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    log_message(LOG_LEVEL_TRACE, "[Stream Processing] avcodec_receive_frame: EAGAIN ou EOF.");
                    break; 
                }
                if (ret < 0) {
                    log_ffmpeg_error(LOG_LEVEL_ERROR, "[Stream Processing] Falha ao receber frame", ret);
                    goto process_stream_error; 
                }

                // CORREÇÃO: Usar decoded_frame nas validações e logs
                log_message(LOG_LEVEL_DEBUG, "[Stream Processing] Frame RECEBIDO ... PTS: %ld ...", 
                            ctx->decoded_frame->pts, post_receive_ts.tv_sec, post_receive_ts.tv_nsec);
                
                // Log ANTES de incrementar o contador
                log_message(LOG_LEVEL_DEBUG, "[Stream Processing ID %d] Frame Decoded PTS: %ld (Counter Before Inc: %d)", 
                            ctx->camera_id, ctx->decoded_frame->pts, ctx->frame_process_counter);
                
                ctx->frame_process_counter++;

                // --- LÓGICA FRAME SKIPPING --- 
                if (ctx->frame_process_counter >= ctx->frame_skip_count) {
                    // Log de ENVIO
                    log_message(LOG_LEVEL_INFO, "[Frame Skip ID %d] SENDING frame (Counter %d >= Skip %d, PTS: %ld)",
                                ctx->camera_id, ctx->frame_process_counter, ctx->frame_skip_count, ctx->decoded_frame->pts);
                                
                                // CORREÇÃO: Passar decoded_frame para a função de despacho
                                bool callback_ok = convert_and_dispatch_frame(ctx, ctx->decoded_frame);
                                ctx->decoded_frame = NULL; // Frame consumido
                                
                                // Resetar contador APÓS envio bem sucedido
                                ctx->frame_process_counter = 0;

                                if (callback_ok && ctx->target_interval_ns > 0) {
                                    // --- Calcular FPS de Saída --- 
                                    ctx->frame_send_counter++;
                                    struct timespec current_time;
                                    clock_gettime(CLOCK_MONOTONIC, &current_time);
                                    double elapsed_s = timespec_diff_s(&ctx->last_fps_calc_time, &current_time);

                                    if (elapsed_s >= FPS_CALC_INTERVAL_S) {
                                        if (elapsed_s > 0) { // Evitar divisão por zero
                                            ctx->calculated_output_fps = (double)ctx->frame_send_counter / elapsed_s;
                                            log_message(LOG_LEVEL_DEBUG, "[FPS Real] FPS de Saída Calculado (últimos %.1fs): %.2f", elapsed_s, ctx->calculated_output_fps);
                                        } else {
                                            log_message(LOG_LEVEL_ERROR, "[FPS Real] Tempo decorrido para cálculo de FPS é zero ou negativo (%.3f s).", elapsed_s);
                                        }
                                        // Resetar para próximo cálculo
                                        ctx->frame_send_counter = 0;
                                        ctx->last_fps_calc_time = current_time;
                                    }
                                    // --- Fim Cálculo FPS ---

                                    // --- Pacing (Tempo Real) APÓS envio --- 
                                    log_message(LOG_LEVEL_TRACE, "[Pacing RealTime] Aguardando %ld ns após envio...", ctx->target_interval_ns);
                                    const long SHORT_SLEEP_NS = 10 * 1000000L; 
                                    struct timespec sleep_ts = {0, SHORT_SLEEP_NS};
                                    int64_t slept_ns = 0;
                                    int64_t sleep_target_ns = ctx->target_interval_ns;
                                    while (slept_ns < sleep_target_ns) {
                                        if (ctx->stop_requested) {
                                            log_message(LOG_LEVEL_DEBUG, "[Pacing RealTime] Parada solicitada durante espera.");
                                            return true; // Sair limpo
                                        }
                                        int64_t remaining_ns = sleep_target_ns - slept_ns;
                                        long current_sleep_ns = (remaining_ns < SHORT_SLEEP_NS) ? (long)remaining_ns : SHORT_SLEEP_NS;
                                        if (current_sleep_ns <= 0) break; 
                                        sleep_ts.tv_nsec = current_sleep_ns;
                                        nanosleep(&sleep_ts, NULL); 
                                        slept_ns += current_sleep_ns;
                                    }
                                    log_message(LOG_LEVEL_TRACE, "[Pacing RealTime] Espera pós-envio concluída.");
                                } else {
                                    // Log de erro se callback/conversão falhar
                                    log_message(LOG_LEVEL_ERROR, "[Loop Leitura ID %d] Falha na conversão ou callback após seleção de frame.", ctx->camera_id);
                                    goto process_stream_error;
                                }

                } else {
                    // Log de SKIP
                    log_message(LOG_LEVEL_DEBUG, "[Frame Skip ID %d] SKIPPING frame (Counter %d < Skip %d, PTS: %ld)",
                                ctx->camera_id, ctx->frame_process_counter, ctx->frame_skip_count, ctx->decoded_frame->pts);
                                
                                // CORREÇÃO: Liberar decoded_frame ao pular
                                av_frame_unref(ctx->decoded_frame);
                                ctx->decoded_frame = NULL;
                }

                // CORREÇÃO: Realocar decoded_frame se necessário
                if (ctx->decoded_frame == NULL) { 
                     ctx->decoded_frame = av_frame_alloc(); 
                     if (!ctx->decoded_frame) { /* erro */ goto process_stream_error; }
                } 
            } // Fim while receive_frame

            // Verificar se a parada foi solicitada após o loop de recebimento
            if (ctx->stop_requested) {
                 log_message(LOG_LEVEL_DEBUG, "[Stream Processing] Parada solicitada após processar pacote.");
                 av_packet_unref(ctx->packet);
                 return true; // Parada solicitada
             }
                 
        } // Fim if stream_index
        av_packet_unref(ctx->packet);
    } // Fim loop leitura/decodificação (agora dentro de process_stream)

process_stream_error:
    // Se chegamos aqui, ocorreu um erro irrecuperável dentro do loop de processamento
    log_message(LOG_LEVEL_ERROR, "[Stream Processing] Erro irrecuperável no loop de processamento.");
    av_packet_unref(ctx->packet); // Garantir limpeza do pacote atual
    // CORREÇÃO: Usar decoded_frame na limpeza de erro
    if(ctx->decoded_frame) av_frame_unref(ctx->decoded_frame);
    if(ctx->frame_bgr) av_frame_unref(ctx->frame_bgr);
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
    log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Target FPS: %d, Calculated Interval (ns): %ld", 
                ctx->camera_id, ctx->target_fps, ctx->target_interval_ns);

    ctx->state = CAM_STATE_CONNECTING; // Estado inicial
    ctx->last_sent_pts = AV_NOPTS_VALUE; // Inicializar PTS

    AVDictionary *opts = NULL; // Mover declaração para fora do loop

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
        
        // ... (Preparação de opts_copy)
        AVDictionary* opts_copy = NULL;
        // Cópia de opts globais se houver (atualmente opts é sempre NULL no início)
        // av_dict_copy(&opts_copy, opts, 0); 

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

        // --- Processar --- 
        bool stop_req = process_stream(ctx);
        if (stop_req) {
            goto thread_exit_cleanup; // Sair limpo se process_stream pediu parada
        }
        // Se retornou false, é erro/EOF
        goto handle_reconnect;

handle_reconnect:
        ctx->is_initializing = false; // Garantir que está falso ao entrar na reconexão
        log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Cleaning up for reconnection...", ctx->camera_id);
        cleanup_ffmpeg_resources(ctx); // Passar ctx
        if (ctx->stop_requested) { break; }
        
        update_camera_status(ctx, CAM_STATE_DISCONNECTED, "Conexão perdida/finalizada"); 
        if (ctx->stop_requested) { log_message(LOG_LEVEL_DEBUG, "[Thread ID %d] Stop requested after disconnected status update.", ctx->camera_id); goto thread_exit_cleanup; }
        ctx->reconnect_attempts++; 
        
        // CORREÇÃO: Declarar e calcular delay_seconds AQUI
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
    cleanup_ffmpeg_resources(ctx); // Passar ctx
    update_camera_status(ctx, CAM_STATE_STOPPED, "Thread encerrada"); // Passar ctx
    log_message(LOG_LEVEL_INFO, "[Thread ID %d] Encerrada completamente.", ctx->camera_id);
    if (opts) av_dict_free(&opts);
    return NULL;
} 