#ifndef MOD_AUDIO_STREAM_H
#define MOD_AUDIO_STREAM_H

#include <switch.h>
#include <speex/speex_resampler.h>

#define MY_BUG_NAME "audio_stream"
#define MAX_SESSION_ID (256)
#define MAX_WS_URI (4096)
#define MAX_METADATA_LEN (8192)

#define EVENT_CONNECT           "mod_audio_stream::connect"
#define EVENT_DISCONNECT        "mod_audio_stream::disconnect"
#define EVENT_ERROR             "mod_audio_stream::error"
#define EVENT_JSON              "mod_audio_stream::json"
#define EVENT_PLAY              "mod_audio_stream::play"
#define EVENT_STREAM_AUDIO_BEGIN "mod_audio_stream::stream_audio_begin"
#define EVENT_STREAM_AUDIO_END   "mod_audio_stream::stream_audio_end"
#define EVENT_STREAM_AUDIO_CANCEL "mod_audio_stream::stream_audio_cancel"
#define EVENT_STREAM_AUDIO_PLAYBACK_START "mod_audio_stream::stream_audio_playback_start"
#define EVENT_STREAM_AUDIO_PLAYBACK_COMPLETE "mod_audio_stream::stream_audio_playback_complete"
#define EVENT_STREAM_AUDIO_PLAYBACK_CANCELLED "mod_audio_stream::stream_audio_playback_cancelled"

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, const char* json);

struct private_data {
    switch_mutex_t *mutex;
    char sessionId[MAX_SESSION_ID];
    SpeexResamplerState *resampler;
    responseHandler_t responseHandler;
    void *pAudioStreamer;
    void *pInboundPlayback;
    char ws_uri[MAX_WS_URI];
    int sampling;
    int channels;
    volatile switch_atomic_t audio_paused;
    volatile switch_atomic_t close_requested;
    volatile switch_atomic_t cleanup_started;
    volatile switch_atomic_t active_stream_counted;
    switch_size_t outbound_chunk_bytes;
    switch_size_t outbound_buffer_bytes;
    int outbound_debug;
    char initialMetadata[8192];
    switch_buffer_t *sbuffer;
    int rtp_packets;
    spx_int16_t *resample_buffer;
    switch_size_t resample_buffer_samples;
};

typedef struct private_data private_t;

enum notifyEvent_t {
    CONNECT_SUCCESS,
    CONNECT_ERROR,
    CONNECTION_DROPPED,
    MESSAGE
};

#endif //MOD_AUDIO_STREAM_H
