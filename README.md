# mod_audio_stream

**Production-grade WebSocket audio streaming for FreeSWITCH.**

Streams real-time audio between FreeSWITCH and external systems with correct lifecycle management, thread safety and predictable memory usage.

The playback feature allows continuous forward streaming while playback runs independently, enabling full-duplex audio between the caller and the WebSocket endpoint.

Key features:

- Full-duplex audio streaming (caller ↔ WebSocket)
- Supports both base64-encoded and raw binary audio
- Playback can be tracked, paused, and resumed dynamically

## Installation

### Dependencies
It requires `libfreeswitch-dev`, `libssl-dev`, `zlib1g-dev`, `libevent-dev` and `libspeexdsp-dev` on Debian/Ubuntu which are regular packages for Freeswitch installation.
### Building
After cloning please execute: **git submodule init** and **git submodule update** to initialize the submodule.
#### Custom path
If you built FreeSWITCH from source, eq. install dir is /usr/local/freeswitch, add path to pkgconfig:
```
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig
```
To build the module, from the cloned repository:
```
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
sudo make install
```
**TLS** is `OFF` by default. To build with TLS support add `-DUSE_TLS=ON` to cmake line.

#### DEB Package
To build a DEB package after making the module:
```
cpack -G DEB
```
Debian package will be placed in root directory `_packages` folder.
The default DEB package targets distro-installed FreeSWITCH and lets Debian tooling derive shared-library dependencies automatically.
If FreeSWITCH was built from source and installed locally, add `-DMOD_AUDIO_STREAM_DEB_LOCAL_FREESWITCH=ON` to the cmake line. The local variant installs `mod_audio_stream.so` to `/usr/lib/mod-audio-stream` by default instead of `/usr/local/freeswitch/mod`, and it does not declare a Debian package dependency for FreeSWITCH. The target system must already provide `libfreeswitch.so.1` through an existing FreeSWITCH installation and runtime linker configuration.

To override the module install directory, pass `MOD_AUDIO_STREAM_MODULE_INSTALL_DIR` when configuring:
```
cmake -DCMAKE_BUILD_TYPE=Release \
    -DMOD_AUDIO_STREAM_DEB_LOCAL_FREESWITCH=ON \
    -DMOD_AUDIO_STREAM_MODULE_INSTALL_DIR=/usr/lib/mod-audio-stream \
    ..
```

To load the module from that custom location, add a `path` attribute to the module entry in `modules.conf.xml`:
```
<load module="mod_audio_stream" path="/usr/lib/mod-audio-stream"/>
```

Common `modules.conf.xml` locations are `/usr/local/freeswitch/conf/autoload_configs/modules.conf.xml` for source-built FreeSWITCH and `/etc/freeswitch/autoload_configs/modules.conf.xml` for distro packages.

## Scripted Build & Installation

```
sudo apt-get -y install git \
    && cd /usr/src/ \
    && git clone https://github.com/amigniter/mod_audio_stream.git \
    && cd mod_audio_stream \
    && sudo bash ./build-mod-audio-stream.sh
```

### Channel variables
The following channel variables can be used to fine tune websocket connection and also configure mod_audio_stream logging:

| Variable                               | Description                                             | Default |
| -------------------------------------- | ------------------------------------------------------- | ------- |
| STREAM_MESSAGE_DEFLATE                 | true or 1, disables per message deflate                 | off     |
| STREAM_HEART_BEAT                      | number of seconds, interval to send the heart beat      | off     |
| STREAM_SUPPRESS_LOG                    | true or 1, suppresses printing to log                   | off     |
| STREAM_BUFFER_SIZE                     | outbound websocket chunk duration in ms, 20-1000        | 20      |
| STREAM_EXTRA_HEADERS                   | JSON object for additional headers in string format     | none    |
| STREAM_STOP_ON_DISCONNECT              | true or 1, stop streaming when websocket closes         | true    |
| STREAM_MAX_INBOUND_QUEUE_MESSAGES      | max queued inbound ws messages before disconnect        | 100     |
| STREAM_MAX_INBOUND_QUEUE_BYTES         | max queued inbound ws bytes before disconnect           | 1048576 |
| STREAM_MAX_BINARY_FRAME_BYTES          | max single inbound binary ws frame size in bytes        | 65536   |
| STREAM_OUTBOUND_DEBUG                  | enable verbose outbound audio diagnostics               | off     |
| STREAM_LIVE_PLAYBACK_MAX_BUFFER_MS     | bounded live playback buffer for inbound binary audio   | 5000    |
| STREAM_LIVE_PLAYBACK_PREROLL_MS        | preroll before live playback starts                     | 60      |
| STREAM_LIVE_PLAYBACK_DEBUG             | enable verbose live playback diagnostics                | off     |
| ~~STREAM_NO_RECONNECT~~                    | true or 1, disables automatic websocket reconnection    | off     |
| STREAM_TLS_CA_FILE                     | CA cert or bundle, or the special values SYSTEM or NONE | SYSTEM  |
| STREAM_TLS_KEY_FILE                    | optional client key for WSS connections                 | none    |
| STREAM_TLS_CERT_FILE                   | optional client cert for WSS connections                | none    |
| STREAM_TLS_DISABLE_HOSTNAME_VALIDATION | true or 1 disable hostname check in WSS connections     | false   |

- Per message deflate compression option is enabled by default. It can lead to a very nice bandwidth savings. To disable it set the channel var to `true|1`.
- Heart beat is sent when there is no traffic to keep idle connections alive. Valid range is `1-3600` seconds.
- Suppress parameter is omitted by default(false). All the responses from websocket server will be printed to the log. Not to flood the log you can suppress it by setting the value to `true|1`. Events are fired still, it only affects printing to the log.
- `Buffer Size` actually represents a duration of audio chunk sent to websocket. If you want to send e.g. 100ms audio packets to your ws endpoint
you would set this variable to 100. It must be divisible by `20` and is bounded to `20-1000ms`.
- Extra headers should be a JSON object with key-value pairs representing additional HTTP headers. Header names must be valid HTTP tokens, values cannot contain CR/LF, and oversized header blobs are rejected.
  ```json
  {
      "Header1": "Value1",
      "Header2": "Value2",
      "Header3": "Value3"
  }
- ~~Websocket automatic reconnection is on by default. To disable it set this channel variable to true or 1.~~
  - libwsc does not support automatic reconnection.
- `STREAM_STOP_ON_DISCONNECT` defaults to `true`. Unexpected websocket disconnects stop the media bug and clean up the stream.
- `STREAM_MAX_INBOUND_QUEUE_MESSAGES`, `STREAM_MAX_INBOUND_QUEUE_BYTES`, and `STREAM_MAX_BINARY_FRAME_BYTES` bound hostile or bursty inbound websocket traffic. Limit violations emit an error event and disconnect the websocket.
- `STREAM_LIVE_PLAYBACK_MAX_BUFFER_MS` bounds the in-memory buffer used for live inbound binary PCM playback. Default is `5000ms`. When the buffer fills, the oldest audio is dropped to keep latency bounded.
- `STREAM_LIVE_PLAYBACK_PREROLL_MS` controls how much buffered audio is accumulated before live inbound playback starts.
- `STREAM_OUTBOUND_DEBUG` enables detailed outbound mic-stream diagnostics, including queueing and drop counters.
- `STREAM_LIVE_PLAYBACK_DEBUG` enables detailed logs for live inbound playback: stream start, binary append checkpoints, zero-fill underruns, overflow counters, and cleanup counters.
- TLS (for WSS) options can be fine tuned with the `STREAM_TLS_*` channel variables:
  - `STREAM_TLS_CA_FILE` the ca certificate (or certificate bundle) file. By default is `SYSTEM` which means use the system defaults.
Can be `NONE` which result in no peer verification.
  - `STREAM_TLS_CERT_FILE` optional client tls certificate file sent to the server.
  - `STREAM_TLS_KEY_FILE` optional client tls key file for the given certificate.
  - `STREAM_TLS_DISABLE_HOSTNAME_VALIDATION` if `true`, disables the check of the hostname against the peer server certificate.
Defaults to `false`, which enforces hostname match with the peer certificate.

## API

### Commands
The freeswitch module exposes the following API commands:

```
uuid_audio_stream <uuid> start <wss-url> <mix-type> <sampling-rate> <metadata>
```
Attaches a media bug and starts streaming audio (in L16 format) to the websocket server. FS default is 8k. If sampling-rate is other than 8k it will be resampled.
- `uuid` - Freeswitch channel unique id
- `wss-url` - websocket url `ws://` or `wss://`
- `mix-type` - choice of 
  - "mono" - single channel containing caller's audio
  - "mixed" - single channel containing both caller and callee audio
  - "stereo" - two channels with caller audio in one and callee audio in the other.
- `sampling-rate` - choice of
  - "8k" = 8000 Hz sample rate will be generated
  - "16k" = 16000 Hz sample rate will be generated
- `metadata` - (optional) a valid `utf-8` text to send. It will be sent the first before audio streaming starts.

```
uuid_audio_stream <uuid> send_text <metadata>
```
Sends a text to the websocket server. Requires a valid `utf-8` text.

```
uuid_audio_stream <uuid> stop <metadata>
```
Stops audio stream and closes websocket connection. If _metadata_ is provided it will be sent before the connection is closed.

```
uuid_audio_stream <uuid> playback_stop
```
Stops active inbound live playback immediately without closing the websocket connection. This is intended for interrupting TTS or other binary playback already buffered in the module. If there is no active inbound playback, this is a no-op and still succeeds.

```
uuid_audio_stream <uuid> playback_end
```
Gracefully ends active inbound live playback without closing the websocket connection. Buffered audio is allowed to drain and playback completes naturally. If there is no active inbound playback, this is a no-op and still succeeds.

```
uuid_audio_stream <uuid> playback_status
```
Returns the current inbound live playback state for the active `uuid_audio_stream` session as a JSON object in the API response. This succeeds even when inbound playback is idle, and reports state such as `active`, `state`, `playbackId`, and buffered audio counters.

```
uuid_audio_stream <uuid> pause
```
Pauses audio stream

```
uuid_audio_stream <uuid> resume
```
Resumes audio stream

## Threading And Resource Model

- Inbound websocket queue processing runs on a shared worker pool keyed by session id, while each session keeps its own bounded inbound queue and in-order message handling.
- Each session currently still uses one inbound playback playout thread.
- Outbound audio capture happens in the media-bug callback, but websocket sending is moved onto an internal sender worker so the callback does not call websocket send directly.
- Outbound websocket audio is buffered in a bounded in-memory queue with drop-oldest behavior to keep latency bounded under congestion.
- Inbound websocket messages are queued behind explicit message/byte caps. Overflow is treated as a protocol/backpressure failure and disconnects the websocket.

## Events
Module will generate the following event types:
- `mod_audio_stream::json`
- `mod_audio_stream::connect`
- `mod_audio_stream::disconnect`
- `mod_audio_stream::error`
- `mod_audio_stream::play`
- `mod_audio_stream::stream_audio_begin`
- `mod_audio_stream::stream_audio_end`
- `mod_audio_stream::stream_audio_cancel`
- `mod_audio_stream::stream_audio_playback_start`
- `mod_audio_stream::stream_audio_playback_complete`
- `mod_audio_stream::stream_audio_playback_cancelled`

### response
Message received from websocket endpoint. Json expected, but it contains whatever the websocket server's response is.
#### Freeswitch event generated
**Name**: mod_audio_stream::json
**Body**: WebSocket server response

### connect
Successfully connected to websocket server.
#### Freeswitch event generated
**Name**: mod_audio_stream::connect
**Body**: JSON
```json
{
	"status": "connected"
}
```

### disconnect
Disconnected from websocket server.
#### Freeswitch event generated
**Name**: mod_audio_stream::disconnect
**Body**: JSON
```json
{
	"status": "disconnected",
	"message": {
		"code": 1000,
		"reason": "Normal closure"
	}
}
```
- code: `<int>`
- reason: `<string>`

### error
There is an error with the connection. Multiple fields will be available on the event to describe the error.
#### Freeswitch event generated
**Name**: mod_audio_stream::error
**Body**: JSON
```json
{
	"status": "error",
	"message": {
		"code": 1,
		"error": "String explaining the error"
	}
}
```
- code: `<int>`
- error: `<string>`

#### Possible `code` values

| Code | Enum Name             | Meaning                                              |
|:----:|:----------------------|:-----------------------------------------------------|
| 1    | `IO`                  | I/O error when reading/writing sockets               |
| 2    | `INVALID_HEADER`      | Server sent a malformed WebSocket header             |
| 3    | `SERVER_MASKED`       | Server frames were masked (not allowed by spec)      |
| 4    | `NOT_SUPPORTED`       | Requested feature (e.g. extension) not supported     |
| 5    | `PING_TIMEOUT`        | No PONG received within timeout                      |
| 6    | `CONNECT_FAILED`      | TCP connection or DNS lookup failed                  |
| 7    | `TLS_INIT_FAILED`     | Couldn't initialize SSL/TLS context                  |
| 8    | `SSL_HANDSHAKE_FAILED`| SSL/TLS handshake with server failed                 |
| 9    | `SSL_ERROR`           | Generic OpenSSL error (certificate, cipher, etc.)    |
| 10   | `TIMEOUT`             | Timeout                                              |
| 11   | `PROTOCOL`            | WebSocket protocol error                             |


### play
**Name**: mod_audio_stream::play
**Body**: JSON

Websocket server may return JSON object containing base64 encoded audio to be played by the user. To use this legacy feature, response must follow the format:
```json
{
  "type": "streamAudio",
  "data": {
    "audioDataType": "raw",
    "sampleRate": 8000,
    "audioData": "base64 encoded audio"
  }
}
```
- audioDataType: `<raw|wav|mp3|ogg>`

Event generated by the module (subclass: _mod_audio_stream::play_) will be the same as the `data` element with the **file** added to it representing filePath:
```json
{
  "audioDataType": "raw",
  "sampleRate": 8000,
  "file": "/path/to/the/file"
}
```
If printing to the log is not suppressed, `response` printed to the console will look the same as the event. The original response containing base64 encoded audio is replaced because it can be quite huge.

All the files generated by this feature will reside at the temp directory and will be deleted when the session is closed.

### stream_audio_begin
**Name**: mod_audio_stream::stream_audio_begin
**Body**: JSON

Emitted when the websocket side starts a live binary playback stream.
```json
{
  "type": "streamAudioBegin",
  "playbackId": "call-123:inbound_tts:1",
  "format": "pcm16",
  "rate": 24000,
  "channels": 1
}
```

### stream_audio_end
**Name**: mod_audio_stream::stream_audio_end
**Body**: JSON

Emitted when the websocket side requests that live playback drain. If no inbound playback is active, the control message is still accepted as a no-op and `playbackId` may be omitted.
```json
{
  "type": "streamAudioEnd",
  "playbackId": "call-123:inbound_tts:1"
}
```

### stream_audio_cancel
**Name**: mod_audio_stream::stream_audio_cancel
**Body**: JSON

Emitted when the websocket side requests immediate cancellation of live playback. If no inbound playback is active, the control message is still accepted as a no-op and `playbackId` may be omitted.
```json
{
  "type": "streamAudioCancel",
  "playbackId": "call-123:inbound_tts:1"
}
```

### stream_audio_playback_start
**Name**: mod_audio_stream::stream_audio_playback_start
**Body**: JSON

Emitted when the module begins writing the live inbound playback stream to the channel after preroll.
```json
{
  "type": "streamAudioPlaybackStart",
  "playbackId": "call-123:inbound_tts:1"
}
```

### stream_audio_playback_complete
**Name**: mod_audio_stream::stream_audio_playback_complete
**Body**: JSON

Emitted after a live inbound playback stream has been ended and all buffered audio has drained.
```json
{
  "type": "streamAudioPlaybackComplete",
  "playbackId": "call-123:inbound_tts:1"
}
```

### stream_audio_playback_cancelled
**Name**: mod_audio_stream::stream_audio_playback_cancelled
**Body**: JSON

Emitted when active live inbound playback is cancelled either by websocket `streamAudioCancel` or FreeSWITCH `uuid_audio_stream <uuid> playback_stop`. A no-op cancel outside active playback does not emit this lifecycle event.
```json
{
  "type": "streamAudioPlaybackCancelled",
  "playbackId": "call-123:inbound_tts:1"
}
```

### live binary playback

For low-latency inbound playback, the websocket server can stream raw PCM16 audio directly to the module. `mod_audio_stream` also exposes local control commands for this path: `playback_stop`, `playback_end`, and `playback_status`.

Start a live playback stream:
```json
{
  "type": "streamAudioBegin",
  "format": "pcm16",
  "rate": 24000,
  "channels": 1
}
```

Then send websocket binary frames containing raw `pcm_s16le` audio. End the live stream with:
```json
{
  "type": "streamAudioEnd"
}
```

To interrupt active live playback immediately, send:
```json
{
  "type": "streamAudioCancel"
}
```
This is a websocket control message from the remote service to `mod_audio_stream`, not a FreeSWITCH API command.

Live binary playback is normalized into the channel's native playback format, buffered in memory, and written back to the channel on the module's internal playout clock. `streamAudioEnd` marks the current playback stream as draining; any binary frames that arrive before the next `streamAudioBegin` are still accepted and drained. Both `streamAudioCancel`/`playback_stop` and `streamAudioEnd`/`playback_end` are idempotent: if no inbound playback is active, they are accepted as no-ops. Remote services can also interrupt playback by sending the websocket `streamAudioCancel` control message shown above.

Example `playback_status` response:
```json
{
  "active": true,
  "state": "playing",
  "playbackId": "call-123:inbound_tts:1",
  "bufferedBytes": 3840,
  "bufferedMs": 120,
  "maxBufferBytes": 160000,
  "prerollBytes": 1920,
  "targetRate": 8000,
  "targetChannels": 1,
  "sourceRate": 24000,
  "sourceChannels": 1,
  "generation": 3,
  "totalInputBytes": 28800,
  "totalOutputBytes": 24960,
  "overflowEvents": 0,
  "droppedOutputBytes": 0
}
```

## History

This module is an extension of the open source work at [mod_audio_stream](https://github.com/amigniter/mod_audio_stream), which was originally inspired by [mod_audio_fork](https://github.com/dochong/drachtio-freeswitch-modules/tree/master/modules/mod_audio_fork).
