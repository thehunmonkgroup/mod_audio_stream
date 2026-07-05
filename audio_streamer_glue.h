#ifndef AUDIO_STREAMER_GLUE_H
#define AUDIO_STREAMER_GLUE_H
#include "mod_audio_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

int validate_ws_uri(const char* url, char *wsUri);
switch_status_t is_valid_utf8(const char *str);
switch_status_t stream_session_send_text(switch_core_session_t *session, const char* text);
switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause);
switch_status_t stream_session_init(switch_core_session_t *session, responseHandler_t responseHandler,
    uint32_t samples_per_second, char *wsUri, int sampling, int channels, char* metadata, void **ppUserData);
switch_bool_t stream_frame(switch_media_bug_t *bug);
switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing);
switch_status_t stream_session_cleanup_with_data(switch_core_session_t *session, char* text, int channelIsClosing, private_t *tech_pvt);
void stream_session_active_inc(void);
void stream_session_active_dec(void);
uint32_t stream_session_active_count(void);

#ifdef __cplusplus
}
#endif

#endif //AUDIO_STREAMER_GLUE_H
