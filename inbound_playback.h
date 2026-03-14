#ifndef INBOUND_PLAYBACK_H
#define INBOUND_PLAYBACK_H

#include "mod_audio_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

switch_status_t inbound_playback_session_init(switch_core_session_t *session, private_t *tech_pvt);
void inbound_playback_session_cleanup(switch_core_session_t *session, private_t *tech_pvt);

switch_status_t inbound_playback_start(
    switch_core_session_t *session,
    private_t *tech_pvt,
    uint32_t source_rate,
    uint32_t source_channels
);
switch_status_t inbound_playback_end(private_t *tech_pvt);
switch_status_t inbound_playback_cancel(switch_core_session_t *session, private_t *tech_pvt);
switch_status_t inbound_playback_append(private_t *tech_pvt, const void *data, size_t len);
switch_status_t inbound_playback_tick(switch_core_session_t *session, private_t *tech_pvt);

#ifdef __cplusplus
}
#endif

#endif
