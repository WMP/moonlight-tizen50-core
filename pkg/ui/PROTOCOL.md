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
| `startStream` | `host`, `w`, `h`, `fps`, `bitrate`, `rikey` (hex), `rikeyid`, `appVer`, `gfeVer`, `rtspUrl`, `codecMode`, `videoCodec` (`H264`/`HEVC`/`AV1`), `hdr` (`true`/`false`), `audioConfig` (`Stereo`/`51Surround`/`71Surround`, the audio channel layout), `fullRange` (`true`/`false`, optional, default `false`) | – | `ml_engine_start_session_v2` when the engine has it, else `ml_engine_start_session` (keeps the encoder's default color negotiation; `fullRange` is ignored) |
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

### `ML-ERROR` line (`diag`)

One line, sections separated by ` | `. It is built when a session fails (also in release builds).
An event with `diag` can be larger than one message, so the host sends it like a large reply:
as ordered parts with its own id (`ev<n>`, never a command id); the UI joins them first.

`ML-ERROR code=<n> name=<NAME> stage=<stage|-> t=<ms> | cfg ... | video ... | player ... | ctl ... | pad ... | cc ... | cc-first: ... | cc-last: ...`

- `code`, `name`: the termination code (`-100` NO_VIDEO_TRAFFIC, `-101` NO_VIDEO_FRAME, `-102` UNEXPECTED_EARLY_TERMINATION, `-103` PROTECTED_CONTENT, `-104` FRAME_CONVERSION, `-110` PLAYER_NOT_STARTED, `-111` PLAYER_STALLED, other: OTHER). `stage`: the failed connection stage, or `-`. `t`: ms since session start.
- `cfg`: the requested stream (size, fps, bitrate, packet size, formats, color, audio, server codec support, requested encryption (`enc`), the negotiated encryption features `encf=<host supported>/<host requested>/<enabled>` (bits: 0x1 control stream v2, 0x2 video, 0x4 audio), `rtsp=<rtsp|rtspenc|->` (the RTSP transport the host gave: `rtspenc` = encrypted RTSP; the encryption features are read from this response), `launch=<launch|resume|->`, `rikey=<ok|DIFF|bad|->`, `rikeyid=<ok|DIFF|->` (the key and id of the session compared with the ones in the `/launch` or `/resume` URL that started it: `DIFF` = the server got another key than the client uses, `bad` = the URL key was not 32 hex chars, `-` = unknown), server versions, host feature flags, RTT/variance). No address, key or PIN.
- `video`: sink setup result, decode units (`du`), IDR frames (`idr`), ms to the first decode unit, ms to the first video packet, `idr_retry` = IDR requests repeated at the start because no video packet had reached the player yet (the first request can be lost before the host pipeline is up; at most 10).
- `player`: results of `open`, window (`win`), `prepare_async` (`prep`), the prepare-done callback (`pdone`) and `start` (`-99` = not reached), `state` (raw player state at a `-110`/`-111` stop, else `-99`), packets submitted (`vsub`; `asub` stays 0, audio does not go through the player), failed submits (`subfail=<count>/<last status>`, status `0` = NOT_PREPARED; `vfail`, `afail` = the same for video and audio alone, last status `0` when none failed), decoded audio packets (`adec`), libopus failures (`adec_fail=<count>/<last opus error>`), player errors (`err=<count>/<last>`).
- `prep-t`: times in ms after the `prepare_async` call (`-` = not reached): `pdone` = prepare-done callback, `err` = first player error, `v1`/`a1` = first accepted video/audio packet, `vpts1`/`apts1` = their PTS in ms. How to read it: `adec_fail` > 0 = corrupt audio packets (or a decoder that could not be created); `afail` high and `a1` after `v1` = the player refused audio until the first video packet; `err` right at `pdone` = the prepare failed.
- `aout`: the audio output. The player has only a video stream (platform limitation: the TV player rejects engine-fed audio streams, both Opus and raw PCM); audio is decoded with libopus and written to PulseAudio. `aout pulse server=<default|native> lat=<usec> err=<n> written_ms=<n> drift_max_ms=<n>`: `server` = which PulseAudio server answered (`native` = the `unix:/run/pulse/native` retry), `lat` = the last PulseAudio stream latency in microseconds, `err` = the last PulseAudio error code, `written_ms` = audio written so far, `drift_max_ms` = the largest A/V drift seen (audio written minus video playing time, relative to the first sample; log only). `aout none reason=<dlopen|dlsym|new|-> err=<n>`: no audio output (`dlopen`/`dlsym` = libpulse-simple missing or incomplete, `new` = `pa_simple_new` failed with PulseAudio error `err`, `-` = audio never started); the stream goes on without sound. How to read it: `lat` far above `MLENGINE_AUDIO_LATENCY_MS` = a large PulseAudio buffer; `drift_max_ms` > 250 = audio and video drift apart.
- `ctl`: control-stream encryption: `enc` = AES-GCM messages encrypted, `fail` = encrypt calls that failed, `iv` = IV length of the last one (12 = V2, 16 = V1), `verify=<ok>/<fail>/<not verified>`: each message is decrypted again with OpenSSL and compared with the plaintext. How to read it: `verify=N/0/0` while the server logs decrypt errors means the key or the IV convention differs (check `rikey`, `rikeyid`, `iv`); `verify` with fails means the client crypto is wrong on this device.
- `pad`: gamepad calls of this session: `arr` = arrival calls, `st` = state calls, `fail=<count>/<last rc>` = calls that returned non-zero (`-1` = bad index or no running stream). `arr=0 st=0` with a pad connected: the UI sent no pad data (check the port).
- `cc`: counters of moonlight-common-c log messages (lost frames, IDR requests, FEC recoveries, and so on).
- `cc-first`, `cc-last`: the first 4 and the last 8 moonlight-common-c log lines.

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
