# UI ↔ host message-port protocol

The web UI (`Moonlight5.ui`, `pkg/ui/mlbridge.js`) talks to the .NET host
(`org.moonlight.host.app`, `pkg/dotnet-host/Program.cs`, a no-display UI app) over Tizen
message ports. The host calls the engine (`libmlengine.so`, `mlclient/mlengine.h`).

| Direction | App id | Port | Trusted |
|-----------|--------|------|---------|
| UI → host (commands) | `org.moonlight.host.app` | `moonlight` | no |
| host → UI (replies, events) | `Moonlight5.ui` | `moonlightui` | no |
| UI → host (debug console relay) | `org.moonlight.host.app` | `mllog` | no |

When the `moonlight` port is missing, the UI starts the host with
`tizen.application.launchAppControl` (operation `default`, explicit app id; fallback
`tizen.application.launch`) and retries with backoff; it asks again every 8 missed
connects. Commands sent before the port exists are queued. A command's timeout starts
when it is sent, not when it is queued.

## Message shape

Every message is a list of `{key, value}` (a Bundle on the host side). All values
are strings, except the binary `data` of an `openUrl` reply (bytes).

- Command: `cmd`, `id` (decimal string, unique per UI session), plus the fields below.
- Reply: `id`, plus either the result fields or `error` (string). One reply per command.
- Event: `event` (string, no `id`). The UI passes it to `handleMessage(event)` of
  moonlight-tizen's `messages.js`, which expects the same strings as the WASM build,
  for example `streamTerminated: 0` or `Connection Established`.

### Large messages (parts)

A Tizen message port rejects a message over about 4 KB. A message whose fields
together exceed 1536 bytes is sent as ordered parts, in both directions:

- each part: `id` (and `cmd`, UI → host), `seq` (0-based), `total`, `field` (the field
  name), `chunk` (up to 512 characters of a string, or up to 1536 bytes of binary data);
- the receiver collects the parts of an `id` (duplicates are ignored), joins the
  chunks of each field in `seq` order, and then handles the joined message as usual;
- small messages stay one message, without `seq`/`total`.

This covers `openUrl` replies (serverinfo, applist, box art) and `httpInit` (cert + key).

The host runs `openUrl`, `pair`, `makeCert` and `httpInit` one at a time (libgamestream
uses one global cancel flag). `cancelRequest` does not wait for that queue.

## Commands

| `cmd` | Fields | Reply on success | Engine call |
|-------|--------|------------------|-------------|
| `makeCert` | – | `cert`, `key` (PEM) | `ml_engine_make_cert` |
| `httpInit` | `cert`, `key`, `uniqueId` | – | `ml_engine_http_init` |
| `openUrl` | `url`, `ppk` (server cert PEM, may be absent), `binary` (`true`/`false`) | `data` (string, or bytes when `binary=true`) | `ml_engine_open_url` |
| `pair` | `major`, `addr`, `port`, `pin` | `serverCert` (PEM) | `ml_engine_pair` |
| `cancelRequest` | – | – | `ml_engine_cancel_request` |
| `startStream` | `host`, `w`, `h`, `fps`, `bitrate`, `rikey` (hex), `rikeyid`, `appVer`, `gfeVer`, `rtspUrl`, `codecMode`, `videoCodec` (`H264`/`HEVC`/`AV1`), `hdr` (`true`/`false`), `audioConfig` (`Stereo`/`51Surround`/`71Surround`, the audio channel layout), `fullRange` (`true`/`false`, optional, default `false`), `audioBatchMs` (0..100, optional: ms of audio per `mlaudio` event for this stream, default from `ML_AUDIO_BATCH_MS`; see `mlaudio`) | – | `ml_engine_start_session_v2` when the engine has it, else `ml_engine_start_session` (keeps the encoder's default color negotiation; `fullRange` is ignored) |
| `stopStream` | – | – | `ml_engine_stop` |
| `sendKeyboardEvent` | `key`, `action`, `mods` | – | `ml_engine_send_key` |
| `gamepadArrival` | `index` (0..15), `mask` (active pad bits), `type` (0 unknown, 1 Xbox, 2 PS, 3 Nintendo), `buttons` (supported button flags), `caps` (1 = analog triggers); all decimal | none: no reply, handled in order on the port thread | `ml_engine_gamepad_arrival` |
| `gamepadState` | `index`, `mask`, `buttons`, `lt`, `rt` (0..255), `lsx`, `lsy`, `rsx`, `rsy` (-32768..32767); all decimal. All zero values with a mask without the bit of `index` = pad removed | none: no reply, handled in order on the port thread (not in a task, so states keep their order) | `ml_engine_gamepad_state` |
| `toggleStats` | `on` (`true`/`false`: the wanted state; absent = flip) | – (`error` = `not supported` with an engine that lacks it) | `ml_engine_get_stats`; while on and a stream runs, the host polls it every 500 ms and sends `mlstats` events |
| `stun` | – | `result` = `""` | none (not supported yet) |
| `wakeOnLan` | `mac` | `error` = `not supported` | none (not supported yet) |
| `setLog` | `on` (`true`/`false`) | – | `ml_engine_set_log_cb` (works also when the engine did not load) |
| `diag` | `rerun` (`true`: run the checks again now) | `report` (the lines of the last run, `\n`-separated) | none; works also when the engine did not load (see below) |

An engine error becomes `error` = the engine return code as a decimal string (the
same values the WASM build rejects with, for example `-1` = GS_FAILED).

## Log lines (on demand)

After `setLog on=true` the host sends log lines to the UI port: `log` (the line, at
most 1000 characters) and `tag` (`MLHOST` or `MLENGINE`), no `id`. It first sends its
last 100 `MLHOST` lines (debug builds: 1000, so the start-up diagnostics fit), then new
ones as they are written. Engine lines exist only in
debug builds. `mlbridge.js` prints them with `console.log('[<tag>] <line>')`. The UI
sends `setLog` from `mlDebug(true|false)` and at start-up when it is on: remembered
on, or not set and this is a debug package (`ML_TRACE`). The dlog output (`MLHOST`, `MLENGINE`, `MLUI`) does not change.

## Events

| `event` | When |
|---------|------|
| `Connection Established` | `startStream` succeeded (`LiStartConnection` returned 0) |
| `streamTerminated: <code>` | a started session ended (`ml_engine_running()` went 1 → 0). `<code>` is the engine's termination code from `ml_engine_last_error()`: `0` after `stopStream` or a graceful end, for example `-101` (no video frame) when the engine failed; `-1` when `startStream` failed. Optional field `diag`: the `ML-ERROR` line (see below). The UI writes it to the console with `console.error('[ML-ERROR] ...')`; the host writes it to dlog. |
| `mlstats` | live statistics, every 500 ms while `toggleStats` is on (see below) |
| `mlaudio` | a batch of decoded audio frames for the UI to play (see below); about 50 per second with the default 20 ms batch |

### `ML-ERROR` line (`diag`)

One line, sections separated by ` | `. It is built when a session fails (also in release builds).
An event with `diag` can be larger than one message, so the host sends it like a large reply:
as ordered parts with its own id (`ev<n>`, never a command id); the UI joins them first.

`ML-ERROR code=<n> name=<NAME> stage=<stage|-> t=<ms> | cfg ... | video ... | player ... | ctl ... | pad ... | cc ... | cc-first: ... | cc-last: ...`

- `code`, `name`: the termination code (`-100` NO_VIDEO_TRAFFIC, `-101` NO_VIDEO_FRAME, `-102` UNEXPECTED_EARLY_TERMINATION, `-103` PROTECTED_CONTENT, `-104` FRAME_CONVERSION, `-110` PLAYER_NOT_STARTED, `-111` PLAYER_STALLED, other: OTHER). `stage`: the failed connection stage, or `-`. `t`: ms since session start.
- `cfg`: the requested stream (size, fps, bitrate, packet size, formats, color, audio, server codec support, requested encryption (`enc`), the negotiated encryption features `encf=<host supported>/<host requested>/<enabled>` (bits: 0x1 control stream v2, 0x2 video, 0x4 audio), `rtsp=<rtsp|rtspenc|->` (the RTSP transport the host gave: `rtspenc` = encrypted RTSP; the encryption features are read from this response), `launch=<launch|resume|->`, `rikey=<ok|DIFF|bad|->`, `rikeyid=<ok|DIFF|->` (the key and id of the session compared with the ones in the `/launch` or `/resume` URL that started it: `DIFF` = the server got another key than the client uses, `bad` = the URL key was not 32 hex chars, `-` = unknown), server versions, host feature flags, RTT/variance). No address, key or PIN.
- `video`: sink setup result, decode units (`du`), IDR frames (`idr`), ms to the first decode unit, ms to the first video packet, `idr_retry` = IDR requests repeated at the start because no video packet had reached the player yet (the first request can be lost before the host pipeline is up; at most 10).
- `player`: results of `open`, window (`win`), `prepare_async` (`prep`), the prepare-done callback (`pdone`) and `start` (`-99` = not reached), `state` (raw player state at a `-110`/`-111` stop, else `-99`), `rebuilds` (player rebuilds this session, see `MLENGINE_PLAYER_REBUILD_MAX`; a rebuild is also visible as a second `open` and `prep`), packets submitted (`vsub`; `asub` stays 0, audio does not go through the player), failed submits (`subfail=<count>/<last status>`, status `0` = NOT_PREPARED; `vfail`, `afail` = the same for video and audio alone, last status `0` when none failed), decoded audio packets (`adec`), libopus failures (`adec_fail=<count>/<last opus error>`), player errors (`err=<count>/<last>`).
- `prep-t`: times in ms after the `prepare_async` call (`-` = not reached): `pdone` = prepare-done callback, `err` = first player error, `v1`/`a1` = first accepted video/audio packet, `vpts1`/`apts1` = their PTS in ms. How to read it: `adec_fail` > 0 = corrupt audio packets (or a decoder that could not be created); `afail` high and `a1` after `v1` = the player refused audio until the first video packet; `err` right at `pdone` = the prepare failed.
- `aout`: the audio output. The player has only a video stream (platform limitation: the TV player rejects engine-fed audio streams); audio is decoded with libopus and sent to the UI as `mlaudio` events, which the UI plays with Web Audio. ` | aout webaudio frames=<n> bytes=<n> sent=<n> drops=<n> sendfail=<n>`: `frames`, `bytes` = PCM the engine handed to the host, `sent` = `mlaudio` events (batches) the host sent, `drops` = frames the host's queue dropped (not sent), `sendfail` = events that failed twice (dropped). The engine re-reads all of them when the host reads the line, so they are final. ` | aout none reason=<no-callback|->`: no audio output; the stream goes on without sound. How to read it: `frames` far below the stream time x 200/s = audio decode problem (see `adec`, `adec_fail`); `drops` > 0 = the host or the message port cannot keep up; `sendfail` > 0 = the message port refused messages; all counters fine but no sound = check the UI console (`[mlaudio, statistics]`).
- `ctl`: control-stream encryption: `enc` = AES-GCM messages encrypted, `fail` = encrypt calls that failed, `iv` = IV length of the last one (12 = V2, 16 = V1), `verify=<ok>/<fail>/<not verified>`: each message is decrypted again with OpenSSL and compared with the plaintext. How to read it: `verify=N/0/0` while the server logs decrypt errors means the key or the IV convention differs (check `rikey`, `rikeyid`, `iv`); `verify` with fails means the client crypto is wrong on this device.
- `pad`: gamepad calls of this session: `arr` = arrival calls, `st` = state calls, `fail=<count>/<last rc>` = calls that returned non-zero (`-1` = bad index or no running stream). `arr=0 st=0` with a pad connected: the UI sent no pad data (check the port).
- `cc`: counters of moonlight-common-c log messages (lost frames, IDR requests, FEC recoveries, and so on).
- `cc-first`, `cc-last`: the first 4 and the last 8 moonlight-common-c log lines.

### `mlaudio`

A batch of decoded audio frames. Fields: `rate` (Hz), `ch` (channels), `samples` (per channel, the
total of the batch); all decimal strings. `data` = binary PCM, signed 16-bit little endian,
interleaved (`samples * ch * 2` bytes). The engine makes 240-sample frames (5 ms at 48 kHz stereo,
960 bytes), about 200 per second. The host groups them: one event holds at most
`ML_AUDIO_BATCH_MS` ms of audio (default 20, so about 50 events per second). The host sends a batch
when it is full, when the oldest frame in it is that old (timeout), and when the rate or the channel
count changes. A batch adds up to that many ms of latency. The host reads the environment variable
`ML_AUDIO_BATCH_MS` once (0..100, bad value = 20); the UI can set `window._mlAudioBatchMs`, which
`startStream` sends as `audioBatchMs` for that stream. 0 = one event per frame.

An event of at most 1536 bytes (sum of key and value lengths) is one message. A bigger one is sent
as parts with an `au<n>` id (not `ev<n>`) and only `data` is split: each part is
`{id, seq, total, field=data, chunk}` with `event`, `rate`, `ch`, `samples` on every part, and `total` =
the number of data chunks (1536 bytes each; a 3840-byte batch is 3 messages). The UI joins them first
(it also accepts the older format, where each scalar is a part of its own, see "Large messages").
If a send fails the host sends the whole event once more with the same id (the UI ignores a repeated
part); if that fails too, the event is dropped and counted in `sendfail`. Counters: `sent` = events,
`drops` = frames, `sendfail` = events. The UI keeps at most 8 unfinished `au` events and clears them
when the stream ends. `mlbridge.js` never passes `mlaudio` to `window.handleMessage` and writes no
console line per event.

UI audio scheduler (`mlaudio.js`, a port of the audio scheduler of moonlight-tizen): a Web Audio
context (48 kHz, `interactive`) is created by `mlAudioStart()` in the `startStream` path
(a user gesture, for the autoplay policy) and closed by `mlAudioStop()` on `stopStream` and
`streamTerminated`. Frames are scheduled against `currentTime` with a jitter buffer of 100 ms
(`window._mlAudioTargetMs` changes it); after an underrun the schedule is primed to the full
buffer again, and a frame is dropped when the schedule is over twice the buffer ahead. The
stream is resampled when the context does not run at the stream rate. Statistics go to the console
every 5 s (`[mlaudio, statistics]`: received = events, `audio=<ms>` = audio received in the window (5000 ms per 5 s = no loss), scheduled, dropped, resync, underrun, headroom). `window._mlAudioTargetMs` is also set from the audio jitter setting of the UI.
With the stats overlay on, the overlay also shows the counters of the scheduler (`mlAudioStats()`);
the host does not know them. Without Web Audio the UI logs `audio output unavailable (Web Audio)`
once and drops the frames.

### `mlstats`

Fields (all decimal strings; the message is well under 4 KB, so it is never split into parts):

| Key | Meaning |
|-----|---------|
| `depth` | video queue depth in ms: last submitted video PTS minus the player playing time |
| `addepth` | the same for audio |
| `fps` | video packets accepted in the last second |
| `drop` | frames skipped by the late-frame cap |
| `full` | frames lost because the player queue stayed full |
| `uo` | `<underruns>/<overruns>` |
| `err` | player errors |
| `rec` | recoveries |
| `play` | playing time in ms |
| `ain` | audio frames the host received from the engine since the stream start |
| `asent` | `mlaudio` events (batches) the host sent |
| `adrop` | audio frames the host's queue dropped |
| `afail` | `mlaudio` events that failed to send (dropped) |

The UI shows these in an overlay and does not pass the event to `window.handleMessage`.
The host stops the polling and the UI hides the overlay on `streamTerminated`.

## Timeouts (UI side)

Each command rejects after 15 s with `timeout`. `openUrl` uses its own timeout
argument when it has one, and then also sends `cancelRequest`. `pair` waits up to
120 s, because the user types the PIN on the PC.

## Video window (known limit)

The engine creates its own Wayland window (`ecore_wl2`) for the esplusplayer overlay,
on the host's ecore main loop. The host is a UI app with no window (nodisplay); if the TV gives it
no display access, `ecore_wl2_display_connect` or `ecore_wl2_window_new` fails. The
engine then fails video setup cleanly: `startStream` replies `error` and the UI gets
`streamTerminated: -1` (MLENGINE shows `esplus_set_window -> -1 (no window ...)`).

If that happens on the TV, the alternative is a small UI app in the same `.tpk` that
owns the overlay window only while a stream runs (the stream takes the screen anyway);
the host keeps the engine and the message ports.

## Engine start-up diagnostics

`pkg/dotnet-host/Diag.cs` logs how the host was started (decoded launch arguments), its
process context (ids, capabilities, security label, cgroup, parent), where the engine
file lies (size, sha256, mount, extended attributes), and then tries each way of loading
`libmlengine.so` on its own: mmap probe + `dlopen` of the packaged file and of its
resolved path, the memfd route, a copy in the data folder (diagnostic only), and each
engine dependency. Every line is `diag#<run> <step>: ...` with the branch taken
(`branch=ok|failed|...`), errno and `dlerror` text; the last line is `summary`.

Debug builds run it at start-up, before the real load. The lines reach the UI console
with the log forward above, and `<data>/mlhost-diag.log`. In the UI console:
`mlDiag()` sends `diag` with `rerun=true` and prints the report as `[MLDIAG]` lines;
`mlDiag('launch')` starts the host in each way the UI can (`launch`, then
`launchAppControl`) while it does not run and prints `[mldiag]` lines after each.
