#include "inbound_playback.h"

#include <algorithm>
#include <cinttypes>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <switch_json.h>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kDefaultMaxBufferMs = 5000;
constexpr uint32_t kDefaultPrerollMs = 60;

enum class PlaybackState {
    Idle,
    Preroll,
    Playing,
    Draining,
    Closed
};

class ByteRingBuffer {
public:
    explicit ByteRingBuffer(size_t capacity)
        : buffer_(capacity, 0), capacity_(capacity) {}

    size_t size() const {
        return size_;
    }

    size_t capacity() const {
        return capacity_;
    }

    void clear() {
        head_ = 0;
        size_ = 0;
    }

    size_t writable_size() const {
        return capacity_ - size_;
    }

    size_t write(const uint8_t *data, size_t len) {
        if (!capacity_ || !data || !len) {
            return 0;
        }

        const size_t to_write = std::min(len, writable_size());
        if (!to_write) {
            return 0;
        }

        size_t tail = (head_ + size_) % capacity_;
        const size_t first = std::min(to_write, capacity_ - tail);
        std::memcpy(buffer_.data() + tail, data, first);

        if (to_write > first) {
            std::memcpy(buffer_.data(), data + first, to_write - first);
        }

        size_ += to_write;
        return to_write;
    }

    size_t read(uint8_t *data, size_t len) {
        if (!data || !len || !size_) {
            return 0;
        }

        const size_t to_read = std::min(len, size_);
        const size_t first = std::min(to_read, capacity_ - head_);
        std::memcpy(data, buffer_.data() + head_, first);

        if (to_read > first) {
            std::memcpy(data + first, buffer_.data(), to_read - first);
        }

        head_ = (head_ + to_read) % capacity_;
        size_ -= to_read;
        return to_read;
    }

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_ = 0;
    size_t head_ = 0;
    size_t size_ = 0;
};

struct InboundPlaybackState {
    InboundPlaybackState(
        std::string session_id,
        responseHandler_t response_handler,
        uint32_t target_rate,
        uint32_t target_channels,
        uint32_t packet_ms,
        size_t packet_bytes,
        size_t packet_samples,
        size_t max_buffer_bytes,
        size_t preroll_bytes
    )
        : session_id(std::move(session_id)),
          response_handler(response_handler),
          target_rate(target_rate),
          target_channels(target_channels),
          packet_ms(packet_ms),
          packet_bytes(packet_bytes),
          packet_samples(packet_samples),
          max_buffer_bytes(max_buffer_bytes),
          preroll_bytes(preroll_bytes),
          buffer(max_buffer_bytes),
          frame_buffer(packet_bytes, 0) {}

    ~InboundPlaybackState() {
        if (resampler) {
            speex_resampler_destroy(resampler);
            resampler = nullptr;
        }

        if (codec_initialized) {
            switch_core_codec_destroy(&raw_codec);
            codec_initialized = false;
        }
    }

    std::string session_id;
    responseHandler_t response_handler = nullptr;
    uint32_t target_rate = 8000;
    uint32_t target_channels = 1;
    uint32_t packet_ms = 20;
    size_t packet_bytes = 0;
    size_t packet_samples = 0;
    size_t max_buffer_bytes = 0;
    size_t preroll_bytes = 0;
    ByteRingBuffer buffer;
    std::vector<uint8_t> pending_input;
    std::vector<uint8_t> frame_buffer;
    std::mutex mutex;
    std::condition_variable cond;
    std::thread worker;
    SpeexResamplerState *resampler = nullptr;
    switch_codec_t raw_codec = {};
    uint32_t source_rate = 0;
    uint32_t source_channels = 0;
    uint64_t generation = 0;
    uint64_t playback_sequence = 0;
    PlaybackState state = PlaybackState::Idle;
    std::string current_playback_id;
    bool closed = false;
    bool debug_enabled = false;
    bool codec_initialized = false;
    bool playback_start_emitted = false;
    uint64_t append_calls = 0;
    uint64_t write_calls = 0;
    uint64_t zero_fill_writes = 0;
    uint64_t total_input_bytes = 0;
    uint64_t total_output_bytes = 0;
    size_t max_buffer_observed = 0;
    bool first_binary_logged = false;
    bool first_tick_logged = false;
    bool first_nonzero_write_logged = false;
    bool thread_started_logged = false;
    bool thread_exit_logged = false;
    uint64_t timer_ticks = 0;
    bool first_timer_tick_logged = false;
    uint64_t overflow_events = 0;
    uint64_t dropped_output_bytes = 0;
};

uint32_t get_channel_var_ms(switch_channel_t *channel, const char *name, uint32_t fallback) {
    const char *value = switch_channel_get_variable(channel, name);
    if (zstr(value)) {
        return fallback;
    }

    const int parsed = std::atoi(value);
    if (parsed <= 0) {
        return fallback;
    }

    return static_cast<uint32_t>(parsed);
}

size_t bytes_for_ms(uint32_t rate, uint32_t channels, uint32_t ms) {
    return static_cast<size_t>(rate) * static_cast<size_t>(channels) * sizeof(int16_t) * static_cast<size_t>(ms) / 1000;
}

const char *playback_state_name(PlaybackState state) {
    switch (state) {
        case PlaybackState::Idle:
            return "idle";
        case PlaybackState::Preroll:
            return "preroll";
        case PlaybackState::Playing:
            return "playing";
        case PlaybackState::Draining:
            return "draining";
        case PlaybackState::Closed:
            return "closed";
    }

    return "unknown";
}

bool playback_state_is_active(PlaybackState state) {
    return state == PlaybackState::Preroll || state == PlaybackState::Playing || state == PlaybackState::Draining;
}

std::shared_ptr<InboundPlaybackState> get_state_shared(private_t *tech_pvt) {
    if (!tech_pvt) {
        return {};
    }

    auto *state_ptr = static_cast<std::shared_ptr<InboundPlaybackState> *>(tech_pvt->pInboundPlayback);
    if (!state_ptr || !(*state_ptr)) {
        return {};
    }

    return *state_ptr;
}

void copy_string_to_buffer(const std::string &value, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return;
    }

    std::snprintf(buffer, buffer_len, "%s", value.c_str());
}

std::string build_event_payload(
    const char *event_type, const std::string &playback_id
) {
    if (!event_type || playback_id.empty()) {
        return {};
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return {};
    }

    cJSON_AddStringToObject(root, "type", event_type);
    cJSON_AddStringToObject(root, "playbackId", playback_id.c_str());
    char *json = cJSON_PrintUnformatted(root);
    std::string payload = json ? json : "";
    cJSON_Delete(root);
    switch_safe_free(json);
    return payload;
}

void emit_playback_event(
    InboundPlaybackState *state,
    switch_core_session_t *session,
    const char *event_name,
    const char *event_type,
    const std::string &playback_id
) {
    if (
        !state || !session || !state->response_handler || !event_name
        || !event_type || playback_id.empty()
    ) {
        return;
    }

    const std::string payload = build_event_payload(event_type, playback_id);
    if (payload.empty()) {
        return;
    }

    state->response_handler(session, event_name, payload.c_str());
}

std::string resolve_playback_id(
    InboundPlaybackState *state, const char *requested_playback_id
) {
    if (!state) {
        return {};
    }

    if (requested_playback_id && requested_playback_id[0] != '\0') {
        return requested_playback_id;
    }

    state->playback_sequence += 1;
    return (
        state->session_id + ":inbound_tts:"
        + std::to_string(state->playback_sequence)
    );
}

switch_status_t configure_resampler(InboundPlaybackState *state) {
    if (!state) {
        return SWITCH_STATUS_FALSE;
    }

    if (state->resampler) {
        speex_resampler_destroy(state->resampler);
        state->resampler = nullptr;
    }

    if (state->source_channels != 1 || !state->source_rate || !state->target_rate) {
        return SWITCH_STATUS_FALSE;
    }

    if (state->source_rate == state->target_rate) {
        return SWITCH_STATUS_SUCCESS;
    }

    int err = 0;
    state->resampler = speex_resampler_init(1, state->source_rate, state->target_rate, SWITCH_RESAMPLE_QUALITY, &err);
    if (err != RESAMPLER_ERR_SUCCESS || !state->resampler) {
        state->resampler = nullptr;
        return SWITCH_STATUS_FALSE;
    }

    return SWITCH_STATUS_SUCCESS;
}

std::vector<int16_t> normalize_samples(InboundPlaybackState *state, const int16_t *samples, size_t sample_count) {
    std::vector<int16_t> mono_output;

    if (!state || !samples || !sample_count || state->source_channels != 1) {
        return mono_output;
    }

    if (state->source_rate == state->target_rate) {
        mono_output.assign(samples, samples + sample_count);
    } else {
        size_t output_capacity = (sample_count * state->target_rate) / state->source_rate + 8;
        if (!output_capacity) {
            output_capacity = 8;
        }

        mono_output.resize(output_capacity);
        spx_uint32_t in_len = static_cast<spx_uint32_t>(sample_count);
        spx_uint32_t out_len = static_cast<spx_uint32_t>(mono_output.size());
        speex_resampler_process_int(state->resampler, 0, samples, &in_len, mono_output.data(), &out_len);
        mono_output.resize(out_len);
    }

    if (state->target_channels == 1) {
        return mono_output;
    }

    if (state->target_channels != 2) {
        return {};
    }

    std::vector<int16_t> stereo_output(mono_output.size() * 2);
    for (size_t i = 0; i < mono_output.size(); ++i) {
        stereo_output[i * 2] = mono_output[i];
        stereo_output[i * 2 + 1] = mono_output[i];
    }

    return stereo_output;
}

switch_status_t initialize_raw_codec(switch_core_session_t *session, InboundPlaybackState *state) {
    if (!session || !state) {
        return SWITCH_STATUS_FALSE;
    }

    if (state->codec_initialized) {
        switch_core_codec_destroy(&state->raw_codec);
        state->codec_initialized = false;
    }

    if (switch_core_codec_init(
            &state->raw_codec,
            "L16",
            NULL,
            NULL,
            state->target_rate,
            state->packet_ms,
            state->target_channels,
            SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
            NULL,
            switch_core_session_get_pool(session)
        ) != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }

    state->codec_initialized = true;
    return SWITCH_STATUS_SUCCESS;
}

void reset_playback_metrics(InboundPlaybackState *state) {
    if (!state) {
        return;
    }

    state->append_calls = 0;
    state->write_calls = 0;
    state->zero_fill_writes = 0;
    state->total_input_bytes = 0;
    state->total_output_bytes = 0;
    state->max_buffer_observed = 0;
    state->first_binary_logged = false;
    state->first_tick_logged = false;
    state->first_nonzero_write_logged = false;
    state->timer_ticks = 0;
    state->first_timer_tick_logged = false;
    state->overflow_events = 0;
    state->dropped_output_bytes = 0;
    state->playback_start_emitted = false;
}

void finalize_active_window(InboundPlaybackState *state) {
    if (!state) {
        return;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->closed && playback_state_is_active(state->state)) {
        state->state = PlaybackState::Idle;
    }
}

switch_status_t write_next_frame(switch_core_session_t *session, InboundPlaybackState *state) {
    if (!session || !state) {
        return SWITCH_STATUS_SUCCESS;
    }

    bool should_write = false;
    bool wrote_audio = false;
    bool should_log_completion = false;
    bool should_emit_playback_start = false;
    std::string playback_start_id;
    std::string playback_complete_id;

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (state->state == PlaybackState::Idle || state->state == PlaybackState::Closed) {
            return SWITCH_STATUS_SUCCESS;
        }

        if (state->state == PlaybackState::Preroll && state->preroll_bytes && state->buffer.size() < std::min(state->preroll_bytes, state->packet_bytes)) {
            return SWITCH_STATUS_SUCCESS;
        }

        state->write_calls++;

        if (state->debug_enabled && !state->first_tick_logged) {
            state->first_tick_logged = true;
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback first tick generation=%" PRIu64 " packet_bytes=%" SWITCH_SIZE_T_FMT " buffer=%" SWITCH_SIZE_T_FMT " state=%s\n",
                state->session_id.c_str(),
                state->generation,
                static_cast<switch_size_t>(state->packet_bytes),
                static_cast<switch_size_t>(state->buffer.size()),
                playback_state_name(state->state)
            );
        }

        const size_t bytes_read = state->buffer.read(state->frame_buffer.data(), state->packet_bytes);

        if (bytes_read == 0) {
            if (state->state == PlaybackState::Draining) {
                playback_complete_id = state->current_playback_id;
                state->state = PlaybackState::Idle;
                state->current_playback_id.clear();
                state->playback_start_emitted = false;
                should_log_completion = true;
            }
        } else {
            if (bytes_read < state->packet_bytes) {
                std::memset(state->frame_buffer.data() + bytes_read, 0, state->packet_bytes - bytes_read);
                state->zero_fill_writes++;
            }

            if (state->state == PlaybackState::Preroll) {
                state->state = PlaybackState::Playing;
            }

            state->total_output_bytes += bytes_read;
            wrote_audio = true;
            should_write = true;
            if (!state->playback_start_emitted && !state->current_playback_id.empty()) {
                should_emit_playback_start = true;
                playback_start_id = state->current_playback_id;
                state->playback_start_emitted = true;
            }
        }

        if (!wrote_audio && (state->state == PlaybackState::Playing || state->state == PlaybackState::Preroll)) {
            std::memset(state->frame_buffer.data(), 0, state->packet_bytes);
            state->zero_fill_writes++;
            should_write = true;
        }

        if (state->debug_enabled && wrote_audio && !state->first_nonzero_write_logged) {
            state->first_nonzero_write_logged = true;
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback first nonzero write bytes=%" SWITCH_SIZE_T_FMT " zero_fill_writes=%" PRIu64 " state=%s\n",
                state->session_id.c_str(),
                static_cast<switch_size_t>(state->packet_bytes),
                state->zero_fill_writes,
                playback_state_name(state->state)
            );
        }

        if (state->debug_enabled && ((state->write_calls % 50) == 0)) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback write checkpoint generation=%" PRIu64 " writes=%" PRIu64 " buffer=%" SWITCH_SIZE_T_FMT " total_out=%" PRIu64 " zero_fill=%" PRIu64 " state=%s\n",
                state->session_id.c_str(),
                state->generation,
                state->write_calls,
                static_cast<switch_size_t>(state->buffer.size()),
                state->total_output_bytes,
                state->zero_fill_writes,
                playback_state_name(state->state)
            );
        }
    }

    if (should_log_completion && state->debug_enabled) {
        switch_log_printf(
            SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_DEBUG,
            "(%s) inbound playback complete generation=%" PRIu64 " writes=%" PRIu64 " total_in=%" PRIu64 " total_out=%" PRIu64 "\n",
            state->session_id.c_str(),
            state->generation,
            state->write_calls,
            state->total_input_bytes,
            state->total_output_bytes
        );
    }

    if (should_log_completion) {
        emit_playback_event(
            state,
            session,
            EVENT_STREAM_AUDIO_PLAYBACK_COMPLETE,
            "streamAudioPlaybackComplete",
            playback_complete_id
        );
    }

    if (!should_write) {
        return SWITCH_STATUS_SUCCESS;
    }

    switch_frame_t write_frame = {};
    write_frame.codec = &state->raw_codec;
    write_frame.data = state->frame_buffer.data();
    write_frame.buflen = static_cast<uint32_t>(state->frame_buffer.size());
    write_frame.datalen = static_cast<uint32_t>(state->packet_bytes);
    write_frame.samples = static_cast<uint32_t>(state->packet_samples);
    write_frame.rate = state->target_rate;

    if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(
            SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_WARNING,
            "(%s) inbound playback write_frame failed generation=%" PRIu64 "\n",
            state->session_id.c_str(),
            state->generation
        );
        return SWITCH_STATUS_FALSE;
    }

    if (should_emit_playback_start) {
        emit_playback_event(
            state,
            session,
            EVENT_STREAM_AUDIO_PLAYBACK_START,
            "streamAudioPlaybackStart",
            playback_start_id
        );
    }

    return SWITCH_STATUS_SUCCESS;
}

void playout_loop(std::shared_ptr<InboundPlaybackState> state) {
    if (!state) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->debug_enabled && !state->thread_started_logged) {
            state->thread_started_logged = true;
            switch_log_printf(
                SWITCH_CHANNEL_LOG,
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback thread started target_rate=%u target_channels=%u packet_bytes=%" SWITCH_SIZE_T_FMT "\n",
                state->session_id.c_str(),
                state->target_rate,
                state->target_channels,
                static_cast<switch_size_t>(state->packet_bytes)
            );
        }
    }

    while (true) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cond.wait(lock, [&state]() {
                return state->closed || playback_state_is_active(state->state);
            });

            if (state->closed) {
                break;
            }
        }

        switch_core_session_t *session = switch_core_session_locate(state->session_id.c_str());
        if (!session) {
            switch_log_printf(
                SWITCH_CHANNEL_LOG,
                SWITCH_LOG_WARNING,
                "(%s) inbound playback failed to locate session for active window\n",
                state->session_id.c_str()
            );
            finalize_active_window(state.get());
            continue;
        }

        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_timer_t timer = {};
        bool timer_initialized = false;
        switch_status_t final_status = SWITCH_STATUS_SUCCESS;

        if (switch_core_timer_init(&timer, "soft", static_cast<int>(state->packet_ms), static_cast<int>(state->packet_samples), NULL) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_WARNING,
                "(%s) inbound playback timer init failed packet_ms=%u packet_samples=%" SWITCH_SIZE_T_FMT "\n",
                state->session_id.c_str(),
                state->packet_ms,
                static_cast<switch_size_t>(state->packet_samples)
            );
            switch_core_session_rwunlock(session);
            finalize_active_window(state.get());
            continue;
        }

        timer_initialized = true;
        switch_core_timer_sync(&timer);

        if (state->debug_enabled) {
            std::lock_guard<std::mutex> lock(state->mutex);
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback active window opened generation=%" PRIu64 " state=%s\n",
                state->session_id.c_str(),
                state->generation,
                playback_state_name(state->state)
            );
        }

        while (switch_channel_ready(channel)) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->closed || !playback_state_is_active(state->state)) {
                    break;
                }
            }

            const switch_status_t status = switch_core_timer_next(&timer);
            if (status != SWITCH_STATUS_SUCCESS) {
                final_status = status;
                finalize_active_window(state.get());
                break;
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->timer_ticks++;
                if (state->debug_enabled && !state->first_timer_tick_logged) {
                    state->first_timer_tick_logged = true;
                    switch_log_printf(
                        SWITCH_CHANNEL_SESSION_LOG(session),
                        SWITCH_LOG_DEBUG,
                        "(%s) inbound playback first timer tick packet_ms=%u packet_samples=%" SWITCH_SIZE_T_FMT "\n",
                        state->session_id.c_str(),
                        state->packet_ms,
                        static_cast<switch_size_t>(state->packet_samples)
                    );
                }
                if (state->debug_enabled && ((state->timer_ticks % 50) == 0)) {
                    switch_log_printf(
                        SWITCH_CHANNEL_SESSION_LOG(session),
                        SWITCH_LOG_DEBUG,
                        "(%s) inbound playback timer checkpoint ticks=%" PRIu64 " state=%s\n",
                        state->session_id.c_str(),
                        state->timer_ticks,
                        playback_state_name(state->state)
                    );
                }
            }

            if (write_next_frame(session, state.get()) != SWITCH_STATUS_SUCCESS) {
                final_status = SWITCH_STATUS_FALSE;
                finalize_active_window(state.get());
                break;
            }
        }

        if (state->debug_enabled) {
            std::lock_guard<std::mutex> lock(state->mutex);
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback active window closed status=%d channel_ready=%d closed=%d state=%s ticks=%" PRIu64 " writes=%" PRIu64 "\n",
                state->session_id.c_str(),
                final_status,
                switch_channel_ready(channel) ? 1 : 0,
                state->closed ? 1 : 0,
                playback_state_name(state->state),
                state->timer_ticks,
                state->write_calls
            );
        }

        if (timer_initialized) {
            switch_core_timer_destroy(&timer);
        }

        switch_core_session_rwunlock(session);

        if (!switch_channel_ready(channel)) {
            finalize_active_window(state.get());
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->debug_enabled && !state->thread_exit_logged) {
            state->thread_exit_logged = true;
            switch_log_printf(
                SWITCH_CHANNEL_LOG,
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback thread exiting closed=%d state=%s ticks=%" PRIu64 " writes=%" PRIu64 "\n",
                state->session_id.c_str(),
                state->closed ? 1 : 0,
                playback_state_name(state->state),
                state->timer_ticks,
                state->write_calls
            );
        }
    }
}

}  // namespace

extern "C" {

switch_status_t inbound_playback_session_init(switch_core_session_t *session, private_t *tech_pvt) {
    if (!session || !tech_pvt) {
        return SWITCH_STATUS_FALSE;
    }

    switch_codec_implementation_t read_impl;
    std::memset(&read_impl, 0, sizeof(read_impl));
    switch_core_session_get_read_impl(session, &read_impl);

    const uint32_t target_rate = read_impl.actual_samples_per_second ? read_impl.actual_samples_per_second : 8000;
    const uint32_t target_channels = read_impl.number_of_channels ? read_impl.number_of_channels : 1;
    const uint32_t packet_ms = read_impl.microseconds_per_packet ? (read_impl.microseconds_per_packet / 1000) : 20;
    const size_t packet_bytes =
        read_impl.decoded_bytes_per_packet ?
            static_cast<size_t>(read_impl.decoded_bytes_per_packet) :
            bytes_for_ms(target_rate, target_channels, packet_ms);
    const size_t packet_samples =
        packet_bytes / (sizeof(int16_t) * static_cast<size_t>(target_channels ? target_channels : 1));

    switch_channel_t *channel = switch_core_session_get_channel(session);
    const uint32_t max_buffer_ms = get_channel_var_ms(channel, "STREAM_LIVE_PLAYBACK_MAX_BUFFER_MS", kDefaultMaxBufferMs);
    const uint32_t preroll_ms = get_channel_var_ms(channel, "STREAM_LIVE_PLAYBACK_PREROLL_MS", kDefaultPrerollMs);

    auto state = std::make_shared<InboundPlaybackState>(
        tech_pvt->sessionId,
        tech_pvt->responseHandler,
        target_rate,
        target_channels,
        packet_ms,
        packet_bytes,
        packet_samples,
        bytes_for_ms(target_rate, target_channels, max_buffer_ms),
        bytes_for_ms(target_rate, target_channels, preroll_ms)
    );

    state->debug_enabled = switch_channel_var_true(channel, "STREAM_LIVE_PLAYBACK_DEBUG");

    if (initialize_raw_codec(session, state.get()) != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }

    state->worker = std::thread(playout_loop, state);

    tech_pvt->pInboundPlayback = new std::shared_ptr<InboundPlaybackState>(state);

    if (state->debug_enabled) {
        switch_log_printf(
            SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_DEBUG,
            "(%s) inbound playback session_init target_rate=%u target_channels=%u packet_ms=%u packet_bytes=%" SWITCH_SIZE_T_FMT " packet_samples=%" SWITCH_SIZE_T_FMT " max_buffer_ms=%u preroll_ms=%u max_buffer_bytes=%" SWITCH_SIZE_T_FMT " preroll_bytes=%" SWITCH_SIZE_T_FMT "\n",
            tech_pvt->sessionId,
            target_rate,
            target_channels,
            packet_ms,
            static_cast<switch_size_t>(packet_bytes),
            static_cast<switch_size_t>(packet_samples),
            max_buffer_ms,
            preroll_ms,
            static_cast<switch_size_t>(state->max_buffer_bytes),
            static_cast<switch_size_t>(state->preroll_bytes)
        );
    }

    return SWITCH_STATUS_SUCCESS;
}

void inbound_playback_session_cleanup(switch_core_session_t *session, private_t *tech_pvt) {
    auto state = get_state_shared(tech_pvt);
    if (!state) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->debug_enabled) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback cleanup generation=%" PRIu64 " state=%s writes=%" PRIu64 " zero_fill=%" PRIu64 " total_in=%" PRIu64 " total_out=%" PRIu64 "\n",
                state->session_id.c_str(),
                state->generation,
                playback_state_name(state->state),
                state->write_calls,
                state->zero_fill_writes,
                state->total_input_bytes,
                state->total_output_bytes
            );
        }

        state->closed = true;
        state->state = PlaybackState::Closed;
        state->buffer.clear();
        state->pending_input.clear();
    }

    state->cond.notify_all();

    if (state->worker.joinable()) {
        state->worker.join();
    }

    auto *state_ptr = static_cast<std::shared_ptr<InboundPlaybackState> *>(tech_pvt->pInboundPlayback);
    tech_pvt->pInboundPlayback = nullptr;
    delete state_ptr;
}

switch_status_t inbound_playback_start(
    switch_core_session_t *session,
    private_t *tech_pvt,
    uint32_t source_rate,
    uint32_t source_channels,
    const char *requested_playback_id,
    char *resolved_playback_id,
    size_t resolved_playback_id_len
) {
    auto state = get_state_shared(tech_pvt);
    if (!session || !state || source_channels != 1) {
        return SWITCH_STATUS_FALSE;
    }

    std::string playback_id;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->generation++;
        playback_id = resolve_playback_id(state.get(), requested_playback_id);
        state->source_rate = source_rate ? source_rate : state->target_rate;
        state->source_channels = source_channels;
        state->buffer.clear();
        state->pending_input.clear();
        state->state = PlaybackState::Preroll;
        state->closed = false;
        state->current_playback_id = playback_id;
        reset_playback_metrics(state.get());

        if (configure_resampler(state.get()) != SWITCH_STATUS_SUCCESS) {
            state->state = PlaybackState::Idle;
            state->current_playback_id.clear();
            return SWITCH_STATUS_FALSE;
        }
    }

    copy_string_to_buffer(playback_id, resolved_playback_id, resolved_playback_id_len);
    state->cond.notify_one();

    if (state->debug_enabled) {
        switch_log_printf(
            SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_DEBUG,
            "(%s) inbound playback begin generation=%" PRIu64 " source_rate=%u source_channels=%u target_rate=%u target_channels=%u packet_bytes=%" SWITCH_SIZE_T_FMT "\n",
            tech_pvt->sessionId,
            state->generation,
            state->source_rate,
            state->source_channels,
            state->target_rate,
            state->target_channels,
            static_cast<switch_size_t>(state->packet_bytes)
        );
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t inbound_playback_end(
    private_t *tech_pvt,
    char *playback_id,
    size_t playback_id_len
) {
    auto state = get_state_shared(tech_pvt);
    if (!state) {
        return SWITCH_STATUS_FALSE;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->state == PlaybackState::Idle || state->state == PlaybackState::Closed) {
        return SWITCH_STATUS_FALSE;
    }

    copy_string_to_buffer(state->current_playback_id, playback_id, playback_id_len);
    state->state = PlaybackState::Draining;
    if (state->debug_enabled) {
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_DEBUG,
            "(%s) inbound playback end generation=%" PRIu64 " buffer=%" SWITCH_SIZE_T_FMT " append_calls=%" PRIu64 " total_in=%" PRIu64 "\n",
            state->session_id.c_str(),
            state->generation,
            static_cast<switch_size_t>(state->buffer.size()),
            state->append_calls,
            state->total_input_bytes
        );
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t inbound_playback_cancel(
    switch_core_session_t *session,
    private_t *tech_pvt,
    char *playback_id,
    size_t playback_id_len
) {
    auto state = get_state_shared(tech_pvt);
    if (!state) {
        return SWITCH_STATUS_FALSE;
    }

    std::string cancelled_playback_id;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        cancelled_playback_id = state->current_playback_id;
        copy_string_to_buffer(
            cancelled_playback_id, playback_id, playback_id_len
        );
        state->generation++;
        state->buffer.clear();
        state->pending_input.clear();
        state->state = PlaybackState::Idle;
        state->current_playback_id.clear();
        state->playback_start_emitted = false;

        if (state->debug_enabled) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) inbound playback cancel generation=%" PRIu64 "\n",
                state->session_id.c_str(),
                state->generation
            );
        }
    }

    state->cond.notify_one();
    emit_playback_event(
        state.get(),
        session,
        EVENT_STREAM_AUDIO_PLAYBACK_CANCELLED,
        "streamAudioPlaybackCancelled",
        cancelled_playback_id
    );

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t inbound_playback_append(private_t *tech_pvt, const void *data, size_t len) {
    auto state = get_state_shared(tech_pvt);
    if (!state || !data || !len) {
        return SWITCH_STATUS_FALSE;
    }

    std::lock_guard<std::mutex> lock(state->mutex);

    if (!playback_state_is_active(state->state)) {
        return SWITCH_STATUS_FALSE;
    }

    state->append_calls++;
    state->total_input_bytes += len;

    if (state->debug_enabled && state->state == PlaybackState::Draining) {
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_DEBUG,
            "(%s) inbound playback received binary while draining generation=%" PRIu64 " payload_bytes=%" SWITCH_SIZE_T_FMT " buffer=%" SWITCH_SIZE_T_FMT "\n",
            state->session_id.c_str(),
            state->generation,
            static_cast<switch_size_t>(len),
            static_cast<switch_size_t>(state->buffer.size())
        );
    }

    if (state->debug_enabled && !state->first_binary_logged) {
        state->first_binary_logged = true;
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_DEBUG,
            "(%s) inbound playback first binary append generation=%" PRIu64 " payload_bytes=%" SWITCH_SIZE_T_FMT " state=%s\n",
            state->session_id.c_str(),
            state->generation,
            static_cast<switch_size_t>(len),
            playback_state_name(state->state)
        );
    }

    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    state->pending_input.insert(state->pending_input.end(), bytes, bytes + len);

    const size_t complete_bytes = state->pending_input.size() - (state->pending_input.size() % sizeof(int16_t));
    if (!complete_bytes) {
        return SWITCH_STATUS_SUCCESS;
    }

    std::vector<uint8_t> complete_input(state->pending_input.begin(), state->pending_input.begin() + complete_bytes);
    state->pending_input.erase(state->pending_input.begin(), state->pending_input.begin() + complete_bytes);

    const size_t sample_count = complete_input.size() / sizeof(int16_t);
    std::vector<int16_t> input_samples(sample_count);
    std::memcpy(input_samples.data(), complete_input.data(), complete_input.size());

    std::vector<int16_t> normalized = normalize_samples(state.get(), input_samples.data(), input_samples.size());
    if (normalized.empty()) {
        return SWITCH_STATUS_SUCCESS;
    }

    const size_t normalized_size = normalized.size() * sizeof(int16_t);
    const auto *normalized_bytes = reinterpret_cast<const uint8_t *>(normalized.data());
    size_t dropped_bytes = 0;

    if (state->buffer.size() == 0 && normalized_size > state->buffer.capacity()) {
        const size_t written = state->buffer.write(normalized_bytes, state->buffer.capacity());
        dropped_bytes = normalized_size - written;
    } else {
        const size_t written = state->buffer.write(normalized_bytes, normalized_size);
        dropped_bytes = normalized_size - written;
    }

    if (dropped_bytes > 0) {
        state->overflow_events++;
        state->dropped_output_bytes += dropped_bytes;
        if (state->debug_enabled) {
            switch_log_printf(
                SWITCH_CHANNEL_LOG,
                SWITCH_LOG_WARNING,
                "(%s) inbound playback overflow generation=%" PRIu64 " dropped_bytes=%" SWITCH_SIZE_T_FMT " buffer=%" SWITCH_SIZE_T_FMT " capacity=%" SWITCH_SIZE_T_FMT "\n",
                state->session_id.c_str(),
                state->generation,
                static_cast<switch_size_t>(dropped_bytes),
                static_cast<switch_size_t>(state->buffer.size()),
                static_cast<switch_size_t>(state->buffer.capacity())
            );
        }
    }

    state->max_buffer_observed = std::max(state->max_buffer_observed, state->buffer.size());

    if (state->state == PlaybackState::Preroll && (!state->preroll_bytes || state->buffer.size() >= state->preroll_bytes)) {
        state->state = PlaybackState::Playing;
    }

    if (state->debug_enabled && ((state->append_calls % 25) == 0 || state->append_calls == 1)) {
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_DEBUG,
            "(%s) inbound playback append checkpoint generation=%" PRIu64 " append_calls=%" PRIu64 " payload_bytes=%" SWITCH_SIZE_T_FMT " normalized_bytes=%" SWITCH_SIZE_T_FMT " dropped_bytes=%" PRIu64 " buffer=%" SWITCH_SIZE_T_FMT " max_buffer=%" SWITCH_SIZE_T_FMT " state=%s\n",
            state->session_id.c_str(),
            state->generation,
            state->append_calls,
            static_cast<switch_size_t>(len),
            static_cast<switch_size_t>(normalized_size),
            state->dropped_output_bytes,
            static_cast<switch_size_t>(state->buffer.size()),
            static_cast<switch_size_t>(state->max_buffer_observed),
            playback_state_name(state->state)
        );
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t inbound_playback_tick(switch_core_session_t *session, private_t *tech_pvt) {
    auto state = get_state_shared(tech_pvt);
    if (!session || !state) {
        return SWITCH_STATUS_SUCCESS;
    }

    return write_next_frame(session, state.get());
}

}  // extern "C"
