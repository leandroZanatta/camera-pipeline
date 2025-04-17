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
    int frame_skip_count;        // Send 1 out of N frames (1 = send all)
    int frame_process_counter;   // Counter for processed frames

    // Fields for calculating actual output FPS
    struct timespec last_fps_calc_time; // Time of last FPS calculation
    long frame_send_counter;           // Frames sent since last calculation
    double calculated_output_fps;      // Last calculated output FPS
    int64_t last_frame_time_ns;        // Timestamp of the last sent frame for pacing

    int64_t last_sent_pts;           // PTS do último frame enviado para Python

    // Campos para timeout de inicialização
    struct timespec initialization_start_time; // Hora que a tentativa começou
    bool is_initializing;                  // Flag para indicar se estamos na fase de init

} camera_thread_context_t;

#endif // CAMERA_CONTEXT_H 