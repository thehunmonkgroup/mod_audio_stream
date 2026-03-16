#include <string>
#include <cstring>
#include <cerrno>
#include "mod_audio_stream.h"
#include "inbound_playback.h"
#include "WebSocketClient.h"
#include "Utf8Validator.h"
#include <switch_json.h>
#include <switch_buffer.h>
#include <unordered_set>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <cctype>
#include <climits>
#include <cstdio>
#include <unistd.h>
#include "base64.h"

#define FRAME_SIZE_8000  320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/

namespace {

constexpr uint32_t kDefaultHeartbeatSeconds = 0;
constexpr uint32_t kMaxHeartbeatSeconds = 3600;
constexpr uint32_t kDefaultBufferSizeMs = 20;
constexpr uint32_t kMaxBufferSizeMs = 1000;
constexpr size_t kDefaultMaxInboundQueueMessages = 100;
constexpr size_t kDefaultMaxInboundQueueBytes = 1024 * 1024;
constexpr size_t kDefaultMaxBinaryFrameBytes = 64 * 1024;
constexpr size_t kMaxExtraHeadersBytes = 8192;
constexpr uint32_t kMaxInboundSampleRate = 48000;

volatile switch_atomic_t g_active_streams = 0;

bool is_sensitive_key(const char* key) {
    if (!key) {
        return false;
    }

    return !strcasecmp(key, "authorization") ||
           !strcasecmp(key, "proxy-authorization") ||
           !strcasecmp(key, "x-api-key") ||
           !strcasecmp(key, "api-key") ||
           !strcasecmp(key, "token") ||
           !strcasecmp(key, "access-token");
}

bool is_header_token(const char* value) {
    if (!value || !*value) {
        return false;
    }

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        const unsigned char c = *p;
        if (std::isalnum(c)) {
            continue;
        }

        switch (c) {
            case '!':
            case '#':
            case '$':
            case '%':
            case '&':
            case '\'':
            case '*':
            case '+':
            case '-':
            case '.':
            case '^':
            case '_':
            case '`':
            case '|':
            case '~':
                continue;
            default:
                return false;
        }
    }

    return true;
}

bool contains_ctl_or_space(const char* value) {
    if (!value) {
        return true;
    }

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f) {
            return true;
        }
    }

    return false;
}

bool contains_crlf(const char* value) {
    if (!value) {
        return true;
    }

    for (const char* p = value; *p; ++p) {
        if (*p == '\r' || *p == '\n') {
            return true;
        }
    }

    return false;
}

uint32_t parse_ms_setting(
    switch_channel_t* channel,
    const char* name,
    uint32_t fallback,
    uint32_t min_value,
    uint32_t max_value
) {
    const char* raw = switch_channel_get_variable(channel, name);
    if (zstr(raw)) {
        return fallback;
    }

    char* endptr = nullptr;
    unsigned long parsed = std::strtoul(raw, &endptr, 10);
    if (!endptr || *endptr != '\0' || parsed < min_value || parsed > max_value) {
        return fallback;
    }

    return static_cast<uint32_t>(parsed);
}

size_t parse_size_setting(
    switch_channel_t* channel,
    const char* name,
    size_t fallback,
    size_t min_value,
    size_t max_value
) {
    const char* raw = switch_channel_get_variable(channel, name);
    if (zstr(raw)) {
        return fallback;
    }

    char* endptr = nullptr;
    unsigned long long parsed = std::strtoull(raw, &endptr, 10);
    if (!endptr || *endptr != '\0' || parsed < min_value || parsed > max_value) {
        return fallback;
    }

    return static_cast<size_t>(parsed);
}

std::string build_error_json(int code, const char* error, const char* detail = nullptr) {
    cJSON* root = cJSON_CreateObject();
    cJSON* message = cJSON_CreateObject();
    if (!root || !message) {
        if (root) cJSON_Delete(root);
        if (message) cJSON_Delete(message);
        return "{\"status\":\"error\"}";
    }

    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddNumberToObject(message, "code", code);
    cJSON_AddStringToObject(message, "error", error ? error : "error");
    if (detail && *detail) {
        cJSON_AddStringToObject(message, "detail", detail);
    }
    cJSON_AddItemToObject(root, "message", message);

    char* json = cJSON_PrintUnformatted(root);
    std::string payload = json ? json : "{\"status\":\"error\"}";
    cJSON_Delete(root);
    switch_safe_free(json);
    return payload;
}

struct ScopedTempFile {
    int fd = -1;
    std::string path;
    bool keep = false;

    ~ScopedTempFile() {
        if (fd >= 0) {
            ::close(fd);
        }
        if (!keep && !path.empty()) {
            ::unlink(path.c_str());
        }
    }
};

bool create_scoped_temp_file(const std::string& session_id, const std::string& suffix, ScopedTempFile& out, std::string& error) {
    if (suffix.empty()) {
        error = "missing suffix";
        return false;
    }

    char file_template[512];
    const int written = switch_snprintf(
        file_template,
        sizeof(file_template),
        "%s%s%s_XXXXXX%s",
        SWITCH_GLOBAL_dirs.temp_dir,
        SWITCH_PATH_SEPARATOR,
        session_id.c_str(),
        suffix.c_str()
    );
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(file_template)) {
        error = "temp file path too long";
        return false;
    }

    const int fd = mkstemps(file_template, static_cast<int>(suffix.size()));
    if (fd < 0) {
        error = std::string("mkstemps failed: ") + std::strerror(errno);
        return false;
    }

    out.fd = fd;
    out.path.assign(file_template);
    return true;
}

bool write_all_fd(int fd, const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        const ssize_t written = ::write(fd, data + offset, len - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

}  // namespace

class AudioStreamer {
public:
    // Factory
    static std::shared_ptr<AudioStreamer> create(
        const char* uuid, const char* wsUri, responseHandler_t callback, int deflate, int heart_beat,
        bool suppressLog, bool outboundDebug, const char* extra_headers, const char* tls_cafile, const char* tls_keyfile, 
        const char* tls_certfile, bool tls_disable_hostname_validation,
        bool stop_on_disconnect, size_t max_inbound_queue_messages,
        size_t max_inbound_queue_bytes, size_t max_binary_frame_bytes) {

        std::shared_ptr<AudioStreamer> sp(new AudioStreamer(
            uuid, wsUri, callback, deflate, heart_beat,
            suppressLog, outboundDebug, extra_headers, tls_cafile, tls_keyfile, 
            tls_certfile, tls_disable_hostname_validation,
            stop_on_disconnect, max_inbound_queue_messages,
            max_inbound_queue_bytes, max_binary_frame_bytes
        ));

        sp->bindCallbacks(std::weak_ptr<AudioStreamer>(sp));

        sp->client.connect();

        return sp;
    }

    ~AudioStreamer() {
        stopWorkers();
    }

    void disconnect() {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting...\n");
        client.disconnect();
    }

    bool isConnected() {
        return client.isConnected();
    }

    void writeBinary(uint8_t* buffer, size_t len) {
        const bool connected = this->isConnected();
        if (m_outbound_debug) {
            switch_log_printf(
                SWITCH_CHANNEL_LOG,
                SWITCH_LOG_DEBUG,
                "(%s) websocket_write_binary bytes=%" SWITCH_SIZE_T_FMT " connected=%s\n",
                m_sessionId.c_str(),
                static_cast<switch_size_t>(len),
                connected ? "true" : "false"
            );
        }
        if(!connected) return;
        client.sendBinary(buffer, len);
    }

    void writeText(const char* text) {
        if(!this->isConnected()) return;
        client.sendMessage(text, strlen(text));
    }

    void deleteFiles() {
        std::vector<std::string> files;

        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            if (m_Files.empty())
                return;

            files.assign(m_Files.begin(), m_Files.end());
            m_Files.clear();
            m_playFile = 0;
        }

        for (const auto& fn : files) {
            ::remove(fn.c_str());
        }
    }

    void markCleanedUp() {
        m_cleanedUp.store(true, std::memory_order_release);
        stopWorkers();
        client.setMessageCallback({});
        client.setBinaryCallback({});
        client.setOpenCallback({});
        client.setErrorCallback({});
        client.setCloseCallback({});
    }

    void notifyOutboundData() {
        {
            std::lock_guard<std::mutex> lock(m_outboundMutex);
            m_outboundPending = true;
        }
        m_outboundCond.notify_one();
    }

    bool isCleanedUp() const {
        return m_cleanedUp.load(std::memory_order_acquire);
    }

private:
    // Ctor
    AudioStreamer(
        const char* uuid, const char* wsUri, responseHandler_t callback, int deflate, int heart_beat,
        bool suppressLog, bool outboundDebug, const char* extra_headers, const char* tls_cafile, const char* tls_keyfile, 
        const char* tls_certfile, bool tls_disable_hostname_validation,
        bool stop_on_disconnect, size_t max_inbound_queue_messages,
        size_t max_inbound_queue_bytes, size_t max_binary_frame_bytes
    ) : m_sessionId(uuid), m_notify(callback), m_suppress_log(suppressLog), 
        m_outbound_debug(outboundDebug), m_extra_headers(extra_headers), m_playFile(0),
        m_stop_on_disconnect(stop_on_disconnect),
        m_maxInboundQueueMessages(max_inbound_queue_messages),
        m_maxInboundQueueBytes(max_inbound_queue_bytes),
        m_maxBinaryFrameBytes(max_binary_frame_bytes) {

        WebSocketHeaders hdrs;
        WebSocketTLSOptions tls;

        if (m_extra_headers && std::strlen(m_extra_headers) <= kMaxExtraHeadersBytes) {
            cJSON *headers_json = cJSON_Parse(m_extra_headers);
            if (headers_json) {
                cJSON *iterator = headers_json->child;
                while (iterator) {
                    if (iterator->type == cJSON_String && iterator->valuestring != nullptr &&
                        iterator->string && is_header_token(iterator->string) &&
                        !contains_crlf(iterator->valuestring)) {
                        hdrs.set(iterator->string, iterator->valuestring);
                    } else if (iterator->string && iterator->type == cJSON_String && iterator->valuestring != nullptr) {
                        switch_log_printf(
                            SWITCH_CHANNEL_LOG,
                            SWITCH_LOG_WARNING,
                            "(%s) ignoring unsafe extra header %s\n",
                            m_sessionId.c_str(),
                            is_sensitive_key(iterator->string) ? "[redacted]" : iterator->string
                        );
                    }
                    iterator = iterator->next;
                }
                cJSON_Delete(headers_json);
            }
        }

        client.setUrl(wsUri);

        // Setup TLS options
        // NONE - disables validation
        // SYSTEM - uses the system CAs bundle
        if (tls_cafile) {
            tls.caFile = tls_cafile;
        }

        if (tls_keyfile) {
            tls.keyFile = tls_keyfile;
        }

        if (tls_certfile) {
            tls.certFile = tls_certfile;
        }

        tls.disableHostnameValidation = tls_disable_hostname_validation;
        client.setTLSOptions(tls);

        // Optional heart beat, sent every xx seconds when there is not any traffic
        // to make sure that load balancers do not kill an idle connection.
        if(heart_beat)
            client.setPingInterval(heart_beat);

        // Per message deflate connection is enabled by default. You can tweak its parameters or disable it
        if(deflate)
            client.enableCompression(false);

        // Set extra headers if any
        if(!hdrs.empty())
            client.setHeaders(hdrs);

        m_inboundWorker = std::thread(&AudioStreamer::processInboundQueue, this);
        m_outboundWorker = std::thread(&AudioStreamer::processOutboundQueue, this);
    }

    struct ProcessResult {
        switch_bool_t ok = SWITCH_FALSE;
        std::string rewrittenJsonData;
        std::vector<std::string> errors;
    };

    enum class InboundMessageType {
        Text,
        Binary
    };

    struct InboundMessage {
        InboundMessageType type;
        std::string text;
        std::vector<uint8_t> binary;
    };

    static inline void push_err(ProcessResult& out, const std::string& sid, const std::string& s) {
        out.errors.push_back("(" + sid + ") " + s);
    }

    void emitErrorAndDisconnect(int code, const char* error, const char* detail = nullptr) {
        if (isCleanedUp()) {
            return;
        }

        const std::string payload = build_error_json(code, error, detail);
        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
        if (psession) {
            m_notify(psession, EVENT_ERROR, payload.c_str());
            media_bug_close(psession);
            switch_core_session_rwunlock(psession);
        }

        client.disconnect();
    }

    void bindCallbacks(std::weak_ptr<AudioStreamer> wp) {
        client.setMessageCallback([wp](const std::string& message) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;
            try {
                self->enqueueText(message);
            } catch (const std::exception& e) {
                self->emitErrorAndDisconnect(4500, "inbound_text_exception", e.what());
            } catch (...) {
                self->emitErrorAndDisconnect(4500, "inbound_text_exception");
            }
        });

        client.setBinaryCallback([wp](const void* data, size_t len) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;
            try {
                self->enqueueBinary(data, len);
            } catch (const std::exception& e) {
                self->emitErrorAndDisconnect(4501, "inbound_binary_exception", e.what());
            } catch (...) {
                self->emitErrorAndDisconnect(4501, "inbound_binary_exception");
            }
        });

        client.setOpenCallback([wp]() {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            try {
                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "connected");
                char* json_str = cJSON_PrintUnformatted(root);
                self->notifySessionEvent(CONNECT_SUCCESS, json_str);
                cJSON_Delete(root);
                switch_safe_free(json_str);
            } catch (const std::exception& e) {
                self->emitErrorAndDisconnect(4502, "open_callback_exception", e.what());
            } catch (...) {
                self->emitErrorAndDisconnect(4502, "open_callback_exception");
            }
        });

        client.setErrorCallback([wp](int code, const std::string& msg) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            try {
                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "error");
                cJSON* message = cJSON_CreateObject();
                cJSON_AddNumberToObject(message, "code", code);
                cJSON_AddStringToObject(message, "error", msg.c_str());
                cJSON_AddItemToObject(root, "message", message);

                char* json_str = cJSON_PrintUnformatted(root);

                self->notifySessionEvent(CONNECT_ERROR, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);
            } catch (...) {
                self->emitErrorAndDisconnect(4503, "error_callback_exception");
            }
        });

        client.setCloseCallback([wp](int code, const std::string& reason) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            try {
                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "disconnected");
                cJSON* message = cJSON_CreateObject();
                cJSON_AddNumberToObject(message, "code", code);
                cJSON_AddStringToObject(message, "reason", reason.c_str());
                cJSON_AddItemToObject(root, "message", message);

                char* json_str = cJSON_PrintUnformatted(root);

                self->notifySessionEvent(CONNECTION_DROPPED, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);
            } catch (...) {
                self->emitErrorAndDisconnect(4504, "close_callback_exception");
            }
        });
    }

    switch_media_bug_t *get_media_bug(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if(!channel) {
            return nullptr;
        }
        auto *bug = (switch_media_bug_t *) switch_channel_get_private(channel, MY_BUG_NAME);
        return bug;
    }

    inline void media_bug_close(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if(bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            switch_atomic_set(&tech_pvt->close_requested, 1);
            switch_core_media_bug_close(&bug, SWITCH_FALSE);
        }
    }

    inline void send_initial_metadata(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if(bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            if(tech_pvt && strlen(tech_pvt->initialMetadata) > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                  "sending initial metadata len=%" SWITCH_SIZE_T_FMT "\n",
                                  (switch_size_t)strlen(tech_pvt->initialMetadata));
                writeText(tech_pvt->initialMetadata);
            }
        }
    }

    void enqueueText(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(m_inboundMutex);
            if (m_inboundQueue.size() >= m_maxInboundQueueMessages || (m_inboundQueueBytes + message.size()) > m_maxInboundQueueBytes) {
                m_inboundOverflowed = true;
            } else {
                m_inboundQueueBytes += message.size();
                m_inboundQueue.push_back(InboundMessage{InboundMessageType::Text, message, {}});
            }
        }

        if (m_inboundOverflowed) {
            emitErrorAndDisconnect(4408, "inbound_queue_overflow", "text queue limit exceeded");
            return;
        }

        m_inboundCond.notify_one();
    }

    void enqueueBinary(const void* data, size_t len) {
        if (!data || !len) {
            return;
        }

        if (len > m_maxBinaryFrameBytes) {
            emitErrorAndDisconnect(4409, "binary_frame_too_large", "inbound binary frame exceeded configured limit");
            return;
        }

        InboundMessage message;
        message.type = InboundMessageType::Binary;
        message.binary.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);

        {
            std::lock_guard<std::mutex> lock(m_inboundMutex);
            if (m_inboundQueue.size() >= m_maxInboundQueueMessages || (m_inboundQueueBytes + message.binary.size()) > m_maxInboundQueueBytes) {
                m_inboundOverflowed = true;
            } else {
                m_inboundQueueBytes += message.binary.size();
                m_inboundQueue.push_back(std::move(message));
            }
        }

        if (m_inboundOverflowed) {
            emitErrorAndDisconnect(4408, "inbound_queue_overflow", "binary queue limit exceeded");
            return;
        }

        m_inboundCond.notify_one();
    }

    void stopWorkers() {
        {
            std::lock_guard<std::mutex> lock(m_inboundMutex);
            m_stopInboundWorker = true;
            m_inboundQueueBytes = 0;
            m_inboundQueue.clear();
        }
        m_inboundCond.notify_all();

        if (m_inboundWorker.joinable()) {
            if (m_inboundWorker.get_id() == std::this_thread::get_id()) {
                m_inboundWorker.detach();
            } else {
                m_inboundWorker.join();
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_outboundMutex);
            m_stopOutboundWorker = true;
            m_outboundPending = true;
        }
        m_outboundCond.notify_all();

        if (m_outboundWorker.joinable()) {
            if (m_outboundWorker.get_id() == std::this_thread::get_id()) {
                m_outboundWorker.detach();
            } else {
                m_outboundWorker.join();
            }
        }
    }

    private_t *get_private_data(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if (!bug) {
            return nullptr;
        }

        return static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    }

    void processInboundQueue() {
        try {
            while (true) {
                InboundMessage message;

                {
                    std::unique_lock<std::mutex> lock(m_inboundMutex);
                    m_inboundCond.wait(lock, [this]() {
                        return m_stopInboundWorker || !m_inboundQueue.empty();
                    });

                    if (m_stopInboundWorker && m_inboundQueue.empty()) {
                        return;
                    }

                    message = std::move(m_inboundQueue.front());
                    if (message.type == InboundMessageType::Text) {
                        m_inboundQueueBytes -= message.text.size();
                    } else {
                        m_inboundQueueBytes -= message.binary.size();
                    }
                    m_inboundQueue.pop_front();
                }

                switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
                if (!psession) {
                    continue;
                }

                auto *tech_pvt = get_private_data(psession);
                if (!tech_pvt) {
                    switch_core_session_rwunlock(psession);
                    continue;
                }

                if (message.type == InboundMessageType::Text) {
                    processQueuedText(psession, tech_pvt, message.text);
                } else {
                    processQueuedBinary(psession, tech_pvt, message.binary);
                }

                switch_core_session_rwunlock(psession);
            }
        } catch (const std::exception& e) {
            emitErrorAndDisconnect(4505, "inbound_worker_exception", e.what());
        } catch (...) {
            emitErrorAndDisconnect(4505, "inbound_worker_exception");
        }
    }

    void processOutboundQueue() {
        try {
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(m_outboundMutex);
                    m_outboundCond.wait(lock, [this]() {
                        return m_stopOutboundWorker || m_outboundPending;
                    });

                    if (m_stopOutboundWorker) {
                        return;
                    }

                    m_outboundPending = false;
                }

                switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
                if (!psession) {
                    continue;
                }

                auto* tech_pvt = get_private_data(psession);
                if (!tech_pvt) {
                    switch_core_session_rwunlock(psession);
                    continue;
                }

                while (client.isConnected()) {
                    switch_size_t bytes_to_read = 0;
                    std::vector<uint8_t> chunk;

                    switch_mutex_lock(tech_pvt->mutex);
                    if (tech_pvt->sbuffer) {
                        const switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                        if (tech_pvt->outbound_chunk_bytes > 0 && inuse >= tech_pvt->outbound_chunk_bytes) {
                            bytes_to_read = tech_pvt->outbound_chunk_bytes;
                            chunk.resize(bytes_to_read);
                            switch_buffer_read(tech_pvt->sbuffer, chunk.data(), bytes_to_read);
                        }
                    }
                    switch_mutex_unlock(tech_pvt->mutex);

                    if (!bytes_to_read) {
                        break;
                    }

                    writeBinary(chunk.data(), chunk.size());
                }

                switch_core_session_rwunlock(psession);
            }
        } catch (const std::exception& e) {
            emitErrorAndDisconnect(4506, "outbound_worker_exception", e.what());
        } catch (...) {
            emitErrorAndDisconnect(4506, "outbound_worker_exception");
        }
    }

    void notifySessionEvent(notifyEvent_t event, const char* message) {
        std::string msg = message ? message : "";

        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
        if (!psession) {
            return;
        }

        switch (event) {
            case CONNECT_SUCCESS:
                send_initial_metadata(psession);
                m_notify(psession, EVENT_CONNECT, msg.c_str());
                break;

            case CONNECTION_DROPPED:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection closed\n");
                m_notify(psession, EVENT_DISCONNECT, msg.c_str());
                if (m_stop_on_disconnect) {
                    media_bug_close(psession);
                }
                break;

            case CONNECT_ERROR:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection error\n");
                m_notify(psession, EVENT_ERROR, msg.c_str());
                media_bug_close(psession);
                break;

            case MESSAGE:
                break;
        }

        switch_core_session_rwunlock(psession);
    }

    uint32_t get_uint_field(cJSON* root, cJSON* data, const char* field_name, uint32_t fallback = 0) {
        cJSON* item = cJSON_GetObjectItem(root, field_name);
        if (!cJSON_IsNumber(item) && data) {
            item = cJSON_GetObjectItem(data, field_name);
        }

        if (!cJSON_IsNumber(item) || item->valueint <= 0) {
            return fallback;
        }

        return static_cast<uint32_t>(item->valueint);
    }

    const char* get_string_field(cJSON* root, cJSON* data, const char* field_name) {
        const char* value = cJSON_GetObjectCstr(root, field_name);
        if (!value && data) {
            value = cJSON_GetObjectCstr(data, field_name);
        }
        return value;
    }

    void emitControlEvent(
        switch_core_session_t *session,
        private_t *tech_pvt,
        const char* event_name,
        const std::string& message
    ) {
        if (!tech_pvt || !tech_pvt->responseHandler || !event_name) {
            return;
        }
        tech_pvt->responseHandler(
            session,
            event_name,
            message.empty() ? nullptr : message.c_str()
        );
    }

    void emitProtocolError(switch_core_session_t* session, private_t* tech_pvt, int code, const char* detail) {
        if (!session || !tech_pvt || !tech_pvt->responseHandler) {
            return;
        }

        const std::string payload = build_error_json(code, "protocol_error", detail);
        tech_pvt->responseHandler(session, EVENT_ERROR, payload.c_str());
    }

    std::string buildControlEventPayload(
        const char* event_type,
        const char* playback_id,
        const char* format = nullptr,
        uint32_t sample_rate = 0,
        uint32_t channels = 0,
        const char* audio_data_type = nullptr,
        const char* encoding = nullptr
    ) {
        if (!event_type) {
            return {};
        }

        cJSON* root = cJSON_CreateObject();
        if (!root) {
            return {};
        }

        cJSON_AddStringToObject(root, "type", event_type);
        if (playback_id && playback_id[0] != '\0') {
            cJSON_AddStringToObject(root, "playbackId", playback_id);
        }
        if (format && format[0] != '\0') {
            cJSON_AddStringToObject(root, "format", format);
        }
        if (sample_rate > 0) {
            cJSON_AddNumberToObject(root, "rate", sample_rate);
        }
        if (channels > 0) {
            cJSON_AddNumberToObject(root, "channels", channels);
        }
        if (audio_data_type && audio_data_type[0] != '\0') {
            cJSON_AddStringToObject(root, "audioDataType", audio_data_type);
        }
        if (encoding && encoding[0] != '\0') {
            cJSON_AddStringToObject(root, "encoding", encoding);
        }

        char* json = cJSON_PrintUnformatted(root);
        std::string payload = json ? json : "";
        cJSON_Delete(root);
        switch_safe_free(json);
        return payload;
    }

    bool handleControlMessage(switch_core_session_t *session, private_t *tech_pvt, const std::string& message) {
        using jsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
        jsonPtr root(cJSON_Parse(message.c_str()), &cJSON_Delete);
        if (!root) {
            return false;
        }

        const char* json_type = cJSON_GetObjectCstr(root.get(), "type");
        if (!json_type) {
            return false;
        }

        cJSON* json_data = cJSON_GetObjectItem(root.get(), "data");

        if (std::strcmp(json_type, "streamAudioBegin") == 0) {
            const char* format = get_string_field(root.get(), json_data, "format");
            const char* audio_data_type = get_string_field(root.get(), json_data, "audioDataType");
            const char* encoding = get_string_field(root.get(), json_data, "encoding");
            const char* requested_playback_id = get_string_field(root.get(), json_data, "playbackId");
            const uint32_t sample_rate = get_uint_field(root.get(), json_data, "rate", get_uint_field(root.get(), json_data, "sampleRate"));
            const uint32_t channels = get_uint_field(root.get(), json_data, "channels", 1);
            char resolved_playback_id[512] = "";
            const bool any_format_field = format || audio_data_type || encoding;
            const bool format_ok = !format || std::strcmp(format, "pcm16") == 0;
            const bool audio_type_ok = !audio_data_type || std::strcmp(audio_data_type, "raw") == 0;
            const bool encoding_ok = !encoding || std::strcmp(encoding, "pcm_s16le") == 0;

            if (!any_format_field || !format_ok || !audio_type_ok || !encoding_ok ||
                !sample_rate || sample_rate > kMaxInboundSampleRate || channels != 1) {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "(%s) unsupported inbound playback format rate=%u channels=%u format=%s audioDataType=%s encoding=%s\n",
                    tech_pvt->sessionId,
                    sample_rate,
                    channels,
                    format ? format : "",
                    audio_data_type ? audio_data_type : "",
                    encoding ? encoding : ""
                );
                emitProtocolError(session, tech_pvt, 4410, "invalid streamAudioBegin payload");
                return true;
            }

            if (
                inbound_playback_start(
                    session,
                    tech_pvt,
                    sample_rate,
                    channels,
                    requested_playback_id,
                    resolved_playback_id,
                    sizeof(resolved_playback_id)
                ) != SWITCH_STATUS_SUCCESS
            ) {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "(%s) failed to start inbound playback stream\n",
                    tech_pvt->sessionId
                );
                emitProtocolError(session, tech_pvt, 4410, "failed to start inbound playback");
            }
            else {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_DEBUG,
                    "(%s) accepted streamAudioBegin rate=%u channels=%u format=%s audioDataType=%s encoding=%s\n",
                    tech_pvt->sessionId,
                    sample_rate,
                    channels,
                    format ? format : "",
                    audio_data_type ? audio_data_type : "",
                    encoding ? encoding : ""
                );
            }

            emitControlEvent(
                session,
                tech_pvt,
                EVENT_STREAM_AUDIO_BEGIN,
                buildControlEventPayload(
                    "streamAudioBegin",
                    resolved_playback_id,
                    format,
                    sample_rate,
                    channels,
                    audio_data_type,
                    encoding
                )
            );
            return true;
        }

        if (std::strcmp(json_type, "streamAudioEnd") == 0) {
            char playback_id[512] = "";
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) received streamAudioEnd\n",
                tech_pvt->sessionId
            );
            if (inbound_playback_end(tech_pvt, playback_id, sizeof(playback_id)) != SWITCH_STATUS_SUCCESS) {
                emitProtocolError(session, tech_pvt, 4411, "streamAudioEnd without active playback");
            }
            emitControlEvent(
                session,
                tech_pvt,
                EVENT_STREAM_AUDIO_END,
                buildControlEventPayload("streamAudioEnd", playback_id)
            );
            return true;
        }

        if (std::strcmp(json_type, "streamAudioCancel") == 0) {
            char playback_id[512] = "";
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) received streamAudioCancel\n",
                tech_pvt->sessionId
            );
            if (inbound_playback_cancel(session, tech_pvt, playback_id, sizeof(playback_id)) != SWITCH_STATUS_SUCCESS) {
                emitProtocolError(session, tech_pvt, 4412, "streamAudioCancel without active playback");
            }
            emitControlEvent(
                session,
                tech_pvt,
                EVENT_STREAM_AUDIO_CANCEL,
                buildControlEventPayload("streamAudioCancel", playback_id)
            );
            return true;
        }

        if (!std::strncmp(json_type, "streamAudio", std::strlen("streamAudio"))) {
            emitProtocolError(session, tech_pvt, 4413, "unsupported streamAudio control message");
            return true;
        }

        return false;
    }

    void processQueuedText(switch_core_session_t *session, private_t *tech_pvt, const std::string& message) {
        if (handleControlMessage(session, tech_pvt, message)) {
            if (!m_suppress_log) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "response: %s\n", message.c_str());
            }
            return;
        }

        ProcessResult pr = processMessage(message);
        for (const auto& e : pr.errors) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s\n", e.c_str());
        }

        const std::string& payload = pr.ok == SWITCH_TRUE ? pr.rewrittenJsonData : message;
        m_notify(session, pr.ok == SWITCH_TRUE ? EVENT_PLAY : EVENT_JSON, payload.c_str());

        if (!m_suppress_log) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "response: %s\n", payload.c_str());
        }
    }

    void processQueuedBinary(switch_core_session_t *session, private_t *tech_pvt, const std::vector<uint8_t>& payload) {
        if (payload.empty()) {
            return;
        }

        if (switch_channel_var_true(switch_core_session_get_channel(session), "STREAM_LIVE_PLAYBACK_DEBUG")) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "(%s) received binary playback payload bytes=%" SWITCH_SIZE_T_FMT "\n",
                tech_pvt->sessionId,
                static_cast<switch_size_t>(payload.size())
            );
        }

        if (inbound_playback_append(tech_pvt, payload.data(), payload.size()) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_WARNING,
                "(%s) dropping inbound binary playback payload of %" SWITCH_SIZE_T_FMT " bytes\n",
                tech_pvt->sessionId,
                static_cast<switch_size_t>(payload.size())
            );
        }
    }


    ProcessResult processMessage(const std::string& message) {
        ProcessResult out;

        // RAII
        using jsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
        jsonPtr root(cJSON_Parse(message.c_str()), &cJSON_Delete);
        if (!root) return out;

        const char* jsonType = cJSON_GetObjectCstr(root.get(), "type");
        if (!jsonType || std::strcmp(jsonType, "streamAudio") != 0) {
            return out; // not ours
        }

        cJSON* jsonData = cJSON_GetObjectItem(root.get(), "data");
        if (!jsonData) {
            push_err(out, m_sessionId, "processMessage - no data in streamAudio");
            return out;
        }

        const char* jsAudioDataType = cJSON_GetObjectCstr(jsonData, "audioDataType");
        if (!jsAudioDataType) jsAudioDataType = "";

        jsonPtr jsonAudio(cJSON_DetachItemFromObject(jsonData, "audioData"), &cJSON_Delete);

        if (!jsonAudio) {
            push_err(out, m_sessionId, "processMessage - streamAudio missing 'audioData' field");
            return out;
        }

        if (!cJSON_IsString(jsonAudio.get()) || !jsonAudio->valuestring) {
            push_err(out, m_sessionId, "processMessage - 'audioData' is not a string (expected base64 string)");
            return out;
        }

        // sampleRate (only meaningful for raw)
        int sampleRate = 0;
        if (cJSON* jsonSampleRate = cJSON_GetObjectItem(jsonData, "sampleRate")) {
            sampleRate = jsonSampleRate->valueint;
        }

        // map file type
        std::string fileType;
        if (std::strcmp(jsAudioDataType, "raw") == 0) {
            switch (sampleRate) {
                case 8000:  fileType = ".r8";  break;
                case 16000: fileType = ".r16"; break;
                case 24000: fileType = ".r24"; break;
                case 32000: fileType = ".r32"; break;
                case 48000: fileType = ".r48"; break;
                case 64000: fileType = ".r64"; break;
                default:
                    push_err(out, m_sessionId, "processMessage - unsupported sample rate: " + std::to_string(sampleRate));
                    return out;
            }
        } else if (std::strcmp(jsAudioDataType, "wav") == 0)  fileType = ".wav";
        else if (std::strcmp(jsAudioDataType, "mp3") == 0)   fileType = ".mp3";
        else if (std::strcmp(jsAudioDataType, "ogg") == 0)   fileType = ".ogg";
        else if (std::strcmp(jsAudioDataType, "pcmu") == 0)  fileType = ".pcmu";
        else if (std::strcmp(jsAudioDataType, "pcma") == 0)  fileType = ".pcma";
        else {
            push_err(out, m_sessionId, "processMessage - unsupported audio type: " + std::string(jsAudioDataType));
            return out;
        }

        // base64 decode
        std::string decoded;
        try {
            decoded = base64_decode(jsonAudio->valuestring);
        } catch (const std::exception& e) {
            push_err(out, m_sessionId, "processMessage - base64 decode error: " + std::string(e.what()));
            return out;
        }

        ScopedTempFile temp_file;
        std::string temp_file_error;
        if (!create_scoped_temp_file(m_sessionId, fileType, temp_file, temp_file_error)) {
            push_err(out, m_sessionId, "processMessage - failed to create temp file: " + temp_file_error);
            return out;
        }

        if (!write_all_fd(
                temp_file.fd,
                reinterpret_cast<const uint8_t*>(decoded.data()),
                decoded.size())) {
            push_err(out, m_sessionId, std::string("processMessage - failed writing file: ") + temp_file.path);
            return out;
        }

        const int fd = temp_file.fd;
        temp_file.fd = -1;
        if (::close(fd) != 0) {
            push_err(out, m_sessionId, std::string("processMessage - failed closing file: ") + temp_file.path);
            return out;
        }

        cJSON* file_item = cJSON_CreateString(temp_file.path.c_str());
        if (!file_item) {
            push_err(out, m_sessionId, "processMessage - failed to create file json field");
            return out;
        }
        cJSON_AddItemToObject(jsonData, "file", file_item);

        // return rewritten jsonData as string
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        if (!jsonString) {
            push_err(out, m_sessionId, "processMessage - cJSON_PrintUnformatted failed");
            return out;
        }

        out.rewrittenJsonData.assign(jsonString);
        switch_safe_free(jsonString);
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            m_Files.insert(temp_file.path);
        }
        temp_file.keep = true;
        out.ok = SWITCH_TRUE;
        return out;
    }

private:
    std::string m_sessionId;
    responseHandler_t m_notify;
    WebSocketClient client;
    bool m_suppress_log;
    bool m_outbound_debug;
    const char* m_extra_headers;
    int m_playFile;
    bool m_stop_on_disconnect;
    size_t m_maxInboundQueueMessages;
    size_t m_maxInboundQueueBytes;
    size_t m_maxBinaryFrameBytes;
    std::unordered_set<std::string> m_Files;
    std::atomic<bool> m_cleanedUp{false};
    std::mutex m_stateMutex;
    std::mutex m_inboundMutex;
    std::condition_variable m_inboundCond;
    std::deque<InboundMessage> m_inboundQueue;
    std::thread m_inboundWorker;
    bool m_stopInboundWorker = false;
    bool m_inboundOverflowed = false;
    size_t m_inboundQueueBytes = 0;
    std::mutex m_outboundMutex;
    std::condition_variable m_outboundCond;
    std::thread m_outboundWorker;
    bool m_stopOutboundWorker = false;
    bool m_outboundPending = false;
};


namespace {

    constexpr size_t kOutboundQueueDepthChunks = 8;

    switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, char *wsUri,
                                     uint32_t sampling, int desiredSampling, int channels, char *metadata, responseHandler_t responseHandler,
                                     int deflate, int heart_beat, bool suppressLog, int rtp_packets, const char* extra_headers,
                                     const char *tls_cafile, const char *tls_keyfile, const char *tls_certfile, 
                                     bool tls_disable_hostname_validation, bool stop_on_disconnect,
                                     size_t max_inbound_queue_messages, size_t max_inbound_queue_bytes,
                                     size_t max_binary_frame_bytes)
    {
        int err; //speex

        switch_memory_pool_t *pool = switch_core_session_get_pool(session);

        memset(tech_pvt, 0, sizeof(private_t));

        switch_copy_string(tech_pvt->sessionId, switch_core_session_get_uuid(session), sizeof(tech_pvt->sessionId));
        switch_copy_string(tech_pvt->ws_uri, wsUri, sizeof(tech_pvt->ws_uri));
        tech_pvt->sampling = desiredSampling;
        tech_pvt->responseHandler = responseHandler;
        tech_pvt->rtp_packets = rtp_packets;
        tech_pvt->channels = channels;
        switch_atomic_set(&tech_pvt->audio_paused, 0);
        switch_atomic_set(&tech_pvt->close_requested, 0);
        switch_atomic_set(&tech_pvt->cleanup_started, 0);
        switch_atomic_set(&tech_pvt->active_stream_counted, 0);
        tech_pvt->outbound_debug = switch_channel_var_true(
            switch_core_session_get_channel(session),
            "STREAM_OUTBOUND_DEBUG"
        );

        if (metadata) {
            switch_copy_string(tech_pvt->initialMetadata, metadata, sizeof(tech_pvt->initialMetadata));
        }

        switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);

        tech_pvt->outbound_chunk_bytes = static_cast<switch_size_t>(FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets);
        tech_pvt->outbound_buffer_bytes = tech_pvt->outbound_chunk_bytes * kOutboundQueueDepthChunks;
        tech_pvt->resample_buffer_samples = static_cast<switch_size_t>(FRAME_SIZE_8000 * desiredSampling / 8000 * channels / sizeof(spx_int16_t));

        if (switch_buffer_create_dynamic(&tech_pvt->sbuffer, tech_pvt->outbound_chunk_bytes, tech_pvt->outbound_buffer_bytes, tech_pvt->outbound_buffer_bytes) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                "%s: Error creating switch buffer.\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        tech_pvt->resample_buffer = static_cast<spx_int16_t*>(
            switch_core_session_alloc(session, tech_pvt->resample_buffer_samples * sizeof(spx_int16_t))
        );
        if (!tech_pvt->resample_buffer) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error allocating resample buffer.\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        if (desiredSampling != sampling) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) resampling from %u to %u\n", tech_pvt->sessionId, sampling, desiredSampling);
            tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
            if (0 != err) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
                return SWITCH_STATUS_FALSE;
            }
        }
        else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
        }

        if (inbound_playback_session_init(session, tech_pvt) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) failed to initialize inbound playback state\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        auto sp = AudioStreamer::create(tech_pvt->sessionId, wsUri, responseHandler, deflate, heart_beat,
                                        suppressLog, tech_pvt->outbound_debug, extra_headers, tls_cafile, tls_keyfile,
                                        tls_certfile, tls_disable_hostname_validation, stop_on_disconnect,
                                        max_inbound_queue_messages, max_inbound_queue_bytes, max_binary_frame_bytes);

        tech_pvt->pAudioStreamer = new std::shared_ptr<AudioStreamer>(sp);

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_data_init\n", tech_pvt->sessionId);

        return SWITCH_STATUS_SUCCESS;
    }

    void destroy_tech_pvt(private_t* tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s destroy_tech_pvt\n", tech_pvt->sessionId);
        if (tech_pvt->resampler) {
            speex_resampler_destroy(tech_pvt->resampler);
            tech_pvt->resampler = nullptr;
        }
        if (tech_pvt->sbuffer) {
            switch_buffer_destroy(&tech_pvt->sbuffer);
        }
        tech_pvt->resample_buffer = nullptr;
        tech_pvt->pInboundPlayback = nullptr;
        if (tech_pvt->mutex) {
            switch_mutex_destroy(tech_pvt->mutex);
            tech_pvt->mutex = nullptr;
        }
    }

}

extern "C" {
    void stream_session_active_inc(void) {
        switch_atomic_inc(&g_active_streams);
    }

    void stream_session_active_dec(void) {
        switch_atomic_dec(&g_active_streams);
    }

    uint32_t stream_session_active_count(void) {
        return switch_atomic_read(&g_active_streams);
    }

    int validate_ws_uri(const char* url, char* wsUri) {
        if (!url || !wsUri) {
            return 0;
        }

        const size_t url_len = std::strlen(url);
        if (!url_len || url_len >= MAX_WS_URI || contains_ctl_or_space(url)) {
            return 0;
        }

        const char* hostStart = nullptr;
        if (std::strncmp(url, "ws://", 5) == 0) {
            hostStart = url + 5;
        } else if (std::strncmp(url, "wss://", 6) == 0) {
            hostStart = url + 6;
        } else {
            return 0;
        }

        const char* cursor = hostStart;
        if (*cursor == '[') {
            ++cursor;
            while (*cursor && *cursor != ']') {
                const unsigned char c = static_cast<unsigned char>(*cursor);
                if (!std::isxdigit(c) && c != ':' && c != '.') {
                    return 0;
                }
                ++cursor;
            }
            if (*cursor != ']') {
                return 0;
            }
            ++cursor;
        } else {
            while (*cursor && *cursor != ':' && *cursor != '/' && *cursor != '?') {
                const unsigned char c = static_cast<unsigned char>(*cursor);
                if (!std::isalnum(c) && c != '-' && c != '.') {
                    return 0;
                }
                ++cursor;
            }
        }

        if (hostStart == cursor || (*hostStart == '[' && cursor == hostStart + 1)) {
            return 0;
        }

        if (*cursor == ':') {
            ++cursor;
            const char* port_start = cursor;
            while (*cursor && *cursor != '/' && *cursor != '?') {
                if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
                    return 0;
                }
                ++cursor;
            }
            if (port_start == cursor) {
                return 0;
            }
        }

        switch_copy_string(wsUri, url, MAX_WS_URI);
        return 1;
    }

    switch_status_t is_valid_utf8(const char *str) {
        if (!str) {
            return SWITCH_STATUS_FALSE;
        }

        Utf8Validator validator;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(str);
        const size_t len = std::strlen(str);
        if (!validator.validateChunk(bytes, len) || !validator.validateFinal()) {
            return SWITCH_STATUS_FALSE;
        }

        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_send_text(switch_core_session_t *session, char* text) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_text failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;

        std::shared_ptr<AudioStreamer> streamer;

        switch_mutex_lock(tech_pvt->mutex);

        if (tech_pvt->pAudioStreamer) {
            auto sp_wrap = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
            if (sp_wrap && *sp_wrap) {
                streamer = *sp_wrap; // copy shared_ptr
            }
        }

        switch_mutex_unlock(tech_pvt->mutex);

        if (streamer) {
            streamer->writeText(text);
            return SWITCH_STATUS_SUCCESS;
        }

        return SWITCH_STATUS_FALSE;
    }

    switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_pauseresume failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;

        switch_core_media_bug_flush(bug);
        switch_atomic_set(&tech_pvt->audio_paused, pause ? 1 : 0);
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_init(switch_core_session_t *session,
                                        responseHandler_t responseHandler,
                                        uint32_t samples_per_second,
                                        char *wsUri,
                                        int sampling,
                                        int channels,
                                        char* metadata,
                                        void **ppUserData)
    {
        int deflate = 0;
        int heart_beat = kDefaultHeartbeatSeconds;
        bool suppressLog = false;
        const char* extra_headers = nullptr;
        int rtp_packets = 1; //20ms burst
        const char* tls_cafile = NULL;;
        const char* tls_keyfile = NULL;;
        const char* tls_certfile = NULL;;
        bool tls_disable_hostname_validation = false;
        bool stop_on_disconnect = true;
        size_t max_inbound_queue_messages = kDefaultMaxInboundQueueMessages;
        size_t max_inbound_queue_bytes = kDefaultMaxInboundQueueBytes;
        size_t max_binary_frame_bytes = kDefaultMaxBinaryFrameBytes;

        switch_channel_t *channel = switch_core_session_get_channel(session);

        if (metadata && std::strlen(metadata) >= MAX_METADATA_LEN) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "metadata exceeds max length of %d bytes\n", MAX_METADATA_LEN - 1);
            return SWITCH_STATUS_FALSE;
        }

        if (switch_channel_var_true(channel, "STREAM_MESSAGE_DEFLATE")) {
            deflate = 1;
        }

        if (switch_channel_var_true(channel, "STREAM_SUPPRESS_LOG")) {
            suppressLog = true;
        }

        tls_cafile = switch_channel_get_variable(channel, "STREAM_TLS_CA_FILE");
        tls_keyfile = switch_channel_get_variable(channel, "STREAM_TLS_KEY_FILE");
        tls_certfile = switch_channel_get_variable(channel, "STREAM_TLS_CERT_FILE");

        if (switch_channel_var_true(channel, "STREAM_TLS_DISABLE_HOSTNAME_VALIDATION")) {
            tls_disable_hostname_validation = true;
        }

        const char* heartBeat = switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
        if (heartBeat) {
            char *endptr;
            long value = strtol(heartBeat, &endptr, 10);
            if (*endptr == '\0' && value >= 1 && value <= kMaxHeartbeatSeconds) {
                heart_beat = (int) value;
            }
        }

        const uint32_t buffer_ms = parse_ms_setting(channel, "STREAM_BUFFER_SIZE", kDefaultBufferSizeMs, kDefaultBufferSizeMs, kMaxBufferSizeMs);
        if ((buffer_ms % 20) == 0) {
            rtp_packets = static_cast<int>(buffer_ms / 20);
        }

        extra_headers = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");
        if (extra_headers && std::strlen(extra_headers) > kMaxExtraHeadersBytes) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "STREAM_EXTRA_HEADERS exceeds max length of %" SWITCH_SIZE_T_FMT " bytes\n",
                              static_cast<switch_size_t>(kMaxExtraHeadersBytes));
            return SWITCH_STATUS_FALSE;
        }

        if (switch_channel_get_variable(channel, "STREAM_STOP_ON_DISCONNECT")) {
            stop_on_disconnect = switch_channel_var_true(channel, "STREAM_STOP_ON_DISCONNECT") ? true : false;
        }
        max_inbound_queue_messages = parse_size_setting(channel, "STREAM_MAX_INBOUND_QUEUE_MESSAGES", kDefaultMaxInboundQueueMessages, 1, 1000);
        max_inbound_queue_bytes = parse_size_setting(channel, "STREAM_MAX_INBOUND_QUEUE_BYTES", kDefaultMaxInboundQueueBytes, 1024, 8 * 1024 * 1024);
        max_binary_frame_bytes = parse_size_setting(channel, "STREAM_MAX_BINARY_FRAME_BYTES", kDefaultMaxBinaryFrameBytes, 512, 1024 * 1024);

        // allocate per-session tech_pvt
        auto* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));

        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
            return SWITCH_STATUS_FALSE;
        }
        if (SWITCH_STATUS_SUCCESS != stream_data_init(tech_pvt, session, wsUri, samples_per_second, sampling, channels, 
                                                        metadata, responseHandler, deflate, heart_beat, suppressLog, rtp_packets, 
                                                        extra_headers, tls_cafile, tls_keyfile, tls_certfile, tls_disable_hostname_validation,
                                                        stop_on_disconnect, max_inbound_queue_messages,
                                                        max_inbound_queue_bytes, max_binary_frame_bytes)) {
            destroy_tech_pvt(tech_pvt);
            return SWITCH_STATUS_FALSE;
        }

        *ppUserData = tech_pvt;

        return SWITCH_STATUS_SUCCESS;
    }

    switch_bool_t stream_frame(switch_media_bug_t *bug) {
        auto *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) return SWITCH_TRUE;
        if (switch_atomic_read(&tech_pvt->audio_paused) || switch_atomic_read(&tech_pvt->cleanup_started)) {
            if (tech_pvt->outbound_debug) {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
                    SWITCH_LOG_DEBUG,
                    "(%s) websocket_audio_skip paused=%d cleanup_started=%d\n",
                    tech_pvt->sessionId,
                    switch_atomic_read(&tech_pvt->audio_paused),
                    switch_atomic_read(&tech_pvt->cleanup_started)
                );
            }
            return SWITCH_TRUE;
        }

        std::shared_ptr<AudioStreamer> streamer;

        if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
            if (tech_pvt->outbound_debug) {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
                    SWITCH_LOG_DEBUG,
                    "(%s) websocket_audio_skip reason=mutex_busy\n",
                    tech_pvt->sessionId
                );
            }
            return SWITCH_TRUE;
        }

        if (!tech_pvt->pAudioStreamer) {
            if (tech_pvt->outbound_debug) {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
                    SWITCH_LOG_DEBUG,
                    "(%s) websocket_audio_skip reason=no_streamer\n",
                    tech_pvt->sessionId
                );
            }
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        auto sp_ptr = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
        if (!sp_ptr || !(*sp_ptr)) {
            if (tech_pvt->outbound_debug) {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
                    SWITCH_LOG_DEBUG,
                    "(%s) websocket_audio_skip reason=streamer_unavailable\n",
                    tech_pvt->sessionId
                );
            }
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        streamer = *sp_ptr;
        if (!streamer || !streamer->isConnected()) {
            if (tech_pvt->outbound_debug) {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
                    SWITCH_LOG_DEBUG,
                    "(%s) websocket_audio_skip reason=not_connected\n",
                    tech_pvt->sessionId
                );
            }
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        auto *resampler = tech_pvt->resampler;
        const int channels = tech_pvt->channels;
        if (tech_pvt->outbound_debug) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
                SWITCH_LOG_DEBUG,
                "(%s) stream_frame_enter connected=%s resampler=%s channels=%d rtp_packets=%d\n",
                tech_pvt->sessionId,
                "true",
                resampler ? "true" : "false",
                channels,
                tech_pvt->rtp_packets
            );
        }

        switch_size_t frames_read = 0;
        switch_size_t bytes_captured = 0;
        switch_size_t bytes_dropped = 0;
        switch_size_t bytes_queued = 0;
        uint8_t discard_buf[1024];
        auto drop_oldest = [&](switch_size_t needed) {
            while (needed > 0 && switch_buffer_inuse(tech_pvt->sbuffer) > 0) {
                const switch_size_t chunk = needed > sizeof(discard_buf) ? sizeof(discard_buf) : needed;
                const switch_size_t dropped = switch_buffer_read(tech_pvt->sbuffer, discard_buf, chunk);
                if (!dropped) {
                    break;
                }
                bytes_dropped += dropped;
                needed -= dropped;
            }
        };

        auto queue_audio = [&](const uint8_t* data, switch_size_t len) {
            if (!data || !len || !tech_pvt->sbuffer) {
                return;
            }

            if (len > tech_pvt->outbound_buffer_bytes) {
                data += (len - tech_pvt->outbound_buffer_bytes);
                bytes_dropped += (len - tech_pvt->outbound_buffer_bytes);
                len = tech_pvt->outbound_buffer_bytes;
            }

            const switch_size_t freespace = switch_buffer_freespace(tech_pvt->sbuffer);
            if (len > freespace) {
                drop_oldest(len - freespace);
            }

            switch_buffer_write(tech_pvt->sbuffer, data, len);
            bytes_queued += len;
        };

        if (nullptr == resampler) {
            uint8_t data_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t frame = {};
            frame.data = data_buf;
            frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                if (!frame.datalen) {
                    continue;
                }
                frames_read += 1;
                bytes_captured += static_cast<switch_size_t>(frame.datalen);
                queue_audio(static_cast<const uint8_t *>(frame.data), frame.datalen);
            }
        } else {
            uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t frame = {};
            frame.data = data;
            frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                if(!frame.datalen) {
                    continue;
                }
                frames_read += 1;

                spx_uint32_t in_len = frame.samples;
                spx_uint32_t out_len = static_cast<spx_uint32_t>(tech_pvt->resample_buffer_samples / static_cast<switch_size_t>(channels));

                if(channels == 1) {
                    speex_resampler_process_int(resampler,
                                    0,
                                    (const spx_int16_t *)frame.data,
                                    &in_len,
                                    tech_pvt->resample_buffer,
                                    &out_len);
                } else {
                    speex_resampler_process_interleaved_int(resampler,
                                    (const spx_int16_t *)frame.data,
                                    &in_len,
                                    tech_pvt->resample_buffer,
                                    &out_len);
                }

                if(out_len > 0) {
                    const size_t bytes_written = (size_t)out_len * (size_t)channels * sizeof(spx_int16_t);
                    bytes_captured += static_cast<switch_size_t>(bytes_written);
                    queue_audio(reinterpret_cast<const uint8_t *>(tech_pvt->resample_buffer), static_cast<switch_size_t>(bytes_written));
                }
            }
        }

        switch_mutex_unlock(tech_pvt->mutex);
        if (tech_pvt->outbound_debug) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
                SWITCH_LOG_DEBUG,
                "(%s) stream_frame_capture_result frames=%" SWITCH_SIZE_T_FMT " captured_bytes=%" SWITCH_SIZE_T_FMT " queued_bytes=%" SWITCH_SIZE_T_FMT " dropped_bytes=%" SWITCH_SIZE_T_FMT "\n",
                tech_pvt->sessionId,
                frames_read,
                bytes_captured,
                bytes_queued,
                bytes_dropped
            );
        }

        if (bytes_queued > 0 && streamer) {
            streamer->notifyOutboundData();
        }
        if (tech_pvt->outbound_debug && bytes_dropped > 0) {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
                SWITCH_LOG_WARNING,
                "(%s) outbound queue dropped %" SWITCH_SIZE_T_FMT " bytes to keep latency bounded\n",
                tech_pvt->sessionId,
                bytes_dropped
            );
        }

        return SWITCH_TRUE;
    }

    switch_status_t stream_session_cleanup_impl(
        switch_core_session_t *session,
        char* text,
        int channelIsClosing,
        private_t *tech_pvt_fallback
    ) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        auto* tech_pvt = bug ? (private_t*) switch_core_media_bug_get_user_data(bug) : tech_pvt_fallback;
        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "stream_session_cleanup: no bug - websocket connection already closed\n");
            return SWITCH_STATUS_FALSE;
        }

        char sessionId[MAX_SESSION_ID];
        switch_copy_string(sessionId, tech_pvt->sessionId, sizeof(sessionId));

        std::shared_ptr<AudioStreamer>* sp_wrap = nullptr;
        std::shared_ptr<AudioStreamer> streamer;

        switch_mutex_lock(tech_pvt->mutex);

        if (switch_atomic_read(&tech_pvt->cleanup_started)) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_STATUS_SUCCESS;
        }
        switch_atomic_set(&tech_pvt->cleanup_started, 1);

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_session_cleanup\n", sessionId);

        if (bug) {
            switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
        }

        sp_wrap = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
        tech_pvt->pAudioStreamer = nullptr;

        if (sp_wrap && *sp_wrap) {
            streamer = *sp_wrap;
        }

        switch_mutex_unlock(tech_pvt->mutex);

        if (bug && !channelIsClosing) {
            switch_core_media_bug_remove(session, &bug);
        }

        if (sp_wrap) {
            delete sp_wrap;
            sp_wrap = nullptr;
        }

        if(streamer) {
            if (text) streamer->writeText(text);
            streamer->markCleanedUp();
            streamer->deleteFiles();
            streamer->disconnect();
        }

        inbound_playback_session_cleanup(session, tech_pvt);
        if (switch_atomic_read(&tech_pvt->active_stream_counted)) {
            switch_atomic_set(&tech_pvt->active_stream_counted, 0);
            stream_session_active_dec();
        }
        destroy_tech_pvt(tech_pvt);

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%s) stream_session_cleanup: connection closed\n", sessionId);
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing) {
        return stream_session_cleanup_impl(session, text, channelIsClosing, nullptr);
    }

    switch_status_t stream_session_cleanup_with_data(switch_core_session_t *session, char* text, int channelIsClosing, private_t *tech_pvt) {
        return stream_session_cleanup_impl(session, text, channelIsClosing, tech_pvt);
    }
}
