#ifndef CAMERA_CONTEXT_H
#define CAMERA_CONTEXT_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h> // For timespec

// FFmpeg includes needed for struct members
#include <libavformat/avformat.h> // For AVFormatContext
#include <libavcodec/avcodec.h>   // For AVCodecContext, AVCodec, AVPacket, AVFrame
#include <libswscale/swscale.h>   // For SwsContext
#include <libavutil/pixfmt.h> // For AVPixelFormat

// Include project definitions (MAX_URL_LENGTH, Callbacks)
// Or redefine constants here if preferred, ensure consistency
#include "camera_processor.h" // Provides MAX_URL_LENGTH and callback types

// Define camera states here if not defined elsewhere publicly
typedef enum {
    CAM_STATE_STOPPED,
    CAM_STATE_CONNECTING,
    CAM_STATE_CONNECTED,
    CAM_STATE_DISCONNECTED,
    CAM_STATE_WAITING_RECONNECT,
    CAM_STATE_RECONNECTING,
    // CAM_STATE_ERROR // Consider adding an explicit error state
} camera_state_t;


// Structure for the context of a single camera thread
typedef struct {
    int camera_id;               // ID for this instance
    pthread_t thread_id;         // Pthread thread ID
    bool active;                 // Is this context slot in use?
    bool stop_requested;         // Signal for the thread loop to stop
    char url[MAX_URL_LENGTH];    // URL for this camera

    camera_state_t state;        // Current state of this camera

    // Callbacks and user data for this instance
    status_callback_t status_cb;
    frame_callback_t frame_cb;
    void* status_cb_user_data;
    void* frame_cb_user_data;

    // FFmpeg resources for this instance
    AVFormatContext *fmt_ctx;
    AVCodecContext *codec_ctx;
    AVCodec *codec;
    int video_stream_index;
    AVPacket *packet;
    AVFrame *decoded_frame;      // Frame for decoded data
    AVFrame *frame_bgr;          // Frame for BGR conversion
    struct SwsContext *sws_ctx;
    int sws_ctx_width;           // Width used for SWS context creation
    int sws_ctx_height;          // Height used for SWS context creation
    enum AVPixelFormat sws_ctx_in_fmt; // Input format for SWS context

    // Flow control fields
    int reconnect_attempts;
    int target_fps;
    int64_t target_interval_ns;  // Real-time pacing interval (nanoseconds)
    double estimated_source_fps;
    double frame_skip_accumulator; // Acumulador fracionário para skip preciso
    double frame_skip_ratio;      // Razão exata de skip (source_fps/target_fps)
    int frame_skip_count;        // Send 1 out of N frames (1 = send all)
    int frame_process_counter;   // Counter for processed frames

    // Fields for calculating actual output FPS
    struct timespec last_fps_calc_time; // Time of last FPS calculation
    
    int64_t last_sent_pts;           // PTS do último frame enviado para Python

    // Campos para timeout de inicialização
    struct timespec initialization_start_time; // Hora que a tentativa começou
    bool is_initializing;                  // Flag para indicar se estamos na fase de init

    // Campo para interrupção de threads bloqueadas
    int interrupt_read_fd;       // Descritor de arquivo para interrupção via pipe
    struct timespec last_frame_sent_time; 
    // Campos para cálculo do FPS de SAÍDA (o que realmente enviamos para Python)
    struct timespec last_output_fps_calc_time; // Tempo do último cálculo de FPS de SAÍDA
    long frame_send_counter;                   // Frames enviados desde o último cálculo
    double calculated_output_fps;              // Último FPS de saída calculado

    // NOVO: Campos para cálculo do FPS de ENTRADA (o que a câmera está enviando, pós-decodificação)
    long frame_input_counter;                  // Contagem de frames DECODIFICADOS (antes do skip)
    double calculated_input_fps;               // Último FPS de entrada calculado
    struct timespec last_input_fps_calc_time;  // Tempo do último cálculo de FPS de ENTRADA
    bool has_real_fps_measurement;             // Indica se já temos medição real do FPS

    // NOVO: Sincronização baseada em PTS
    double pts_time_base;                      // time_base do stream em segundos por unidade de PTS
    int64_t first_pts;                         // Primeiro PTS observado (para ancoragem)
    struct timespec playback_anchor_mono;      // Tempo monotônico correspondente ao first_pts
    double last_sent_pts_sec;                  // PTS do último frame enviado em segundos

    // NOVO: Thresholds configuráveis (segundos)
    double early_sleep_threshold_sec;          // quanto adiantado precisa estar para dormir (p.ex. 0.05)
    double lateness_catchup_threshold_sec;     // atraso acima do qual não dorme (p.ex. 0.20)
    double pts_jump_reset_threshold_sec;       // salto de PTS que gatilha realinhamento de âncora (p.ex. 1.0)
    double stall_timeout_sec;                  // tempo sem atividade para forçar reconexão (p.ex. 30.0)

    // NOVO: Marcação de última atividade para detecção de stall
    struct timespec last_activity_mono;
} camera_thread_context_t;

#endif // CAMERA_CONTEXT_H 