# moonlight-tizen50-core

A **native** [Moonlight](https://moonlight-stream.org/) GameStream client for
**Samsung TVs running Tizen 5.0**, with low-latency hardware video.

It streams games from a GameStream host (Sunshine, GeForce Experience,
[Wolf](https://games-on-whales.github.io/wolf/)) and decodes video on the TV's
hardware video plane through the vendor library `libesplusplayer.so`.

> Status: **experimental / work in progress.** The streaming engine runs
> end-to-end on real hardware (pairing → launch → RTSP → hardware decode, 2–3
> frame player latency). The app package (UI + engine host) is still being
> brought up.

## Install (no build needed)

CI builds the packages on every release
(`.github/workflows/build-packages.yml`); download them from the
[Releases](../../releases) page and install with
**[Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung)**.

1. On the TV: enable **Developer Mode** (Apps → `12345` → on, enter your PC's IP,
   reboot).
2. Download the packages for your TV from Releases and install both:
   `org.moonlight.host-*.tpk` (the native engine host, Tizen 4.0–5.5) and
   `Moonlight-*.wgt` (the web UI). The UI starts the host by itself: no reboot and
   no manual start.
3. Open **Apps2Samsung**, point it at the TV. It reads the TV's **DUID**, signs
   each package for that DUID, and installs it. Apps2Samsung re-signs per-DUID,
   which is what the TV accepts. No Samsung certificate to create by hand, no
   `sdb` commands.

> **Moving back from the hybrid package:** uninstall the hybrid Moonlight package
> first (Apps2Samsung → *On the TV* → *Uninstall*). It holds the host app id
> `org.moonlight.host.app`, so the TV refuses the host `.tpk` while it is installed.
> Then install the two packages above.

> Developer alternative: `pkg/install-on-tv.sh <TV_IP> <package> [cert_password]`
> does `sdb connect` + `tizen install`, re-signing with a Samsung DUID-bound cert
> you supply. Apps2Samsung is the same step, automated.

## Debug vs release packages

Each package exists twice:

- **Normal** (`Moonlight-*.wgt` + `org.moonlight.host-*.tpk`):
  for daily use.
- **Debug** (the same names ending in `-debug`): download these to report a problem.
  They are debug builds (no optimisation, symbols kept) with verbose logs from the
  web UI, the .NET host and the native engine.

Normal and debug use the same package and app ids: install one, not both
(install over it, or uninstall the other first).

### Getting the logs (Apps2Samsung)

Use the debug console of the Apps2Samsung app (desktop: Linux/Windows/macOS, or
Android). It shows the web UI console, and on request the host and engine log too.

1. **On the TV** → find **Moonlight**. Hover over **⋯** and use the entry whose id
   is **`Moonlight5.ui`**. The host (`org.moonlight.host.app`, shown as
   *Moonlight host*, older builds show it as *Moonlight* too) has no console.
2. **⋯ → Debug**. The UI restarts on the TV in debug mode and the console opens
   (desktop: Chrome `chrome://inspect`; Android: the app's own console).
3. **Debug packages:** the host and engine log appear in the same console by
   themselves (`[MLHOST]` / `[MLENGINE]`). On start the host first sends its last
   100 lines (start-up, engine load), then new lines as they come.
   **Normal packages** (host lines only, the engine has no trace there), or to turn it
   off or on again; the choice is remembered:

   ```js
   mlDebug(true)    // host (+ engine) lines in this console
   mlDebug(false)   // turn it off
   ```
   **Engine start-up diagnostics** (debug packages log them at host start, as
   `[MLHOST] diag#1 ...` lines ending with `diag#1 summary: ...`):

   ```js
   mlDiag()           // the host runs its engine load checks again: [MLDIAG] lines
   mlDiag('launch')   // start the host in each way the UI can: [mldiag] lines
   ```

   If no `[MLHOST]` line comes at all, the host did not start: run `mlDiag('launch')`.
4. Do what fails (add a host, pair, start a stream), then save the console to a
   file (desktop Chrome: right-click in the console → *Save as…*; Android: share).

Private keys from the pairing certificate are shown as `[redacted]`. The file still
holds host addresses and ids: share it only with the people who need it.

### Which lines are ours

The console also shows the base UI's own lines (`[index.js, …]`, `[utils.js, …]`,
`[productinfo.js] …`) and TV/browser noise. Lines from this project start with:

| Prefix | Source | When |
|--------|--------|------|
| `[mlbridge]` | UI ↔ host bridge (`pkg/ui/mlbridge.js`): commands, replies, host start | always; details in debug packages |
| `[mltrace]` | debug console relay (`pkg/ui/mltrace.js`) | debug packages |
| `[MLHOST]` | .NET host | debug packages; normal ones after `mlDebug(true)` |
| `[MLENGINE]` | native engine (pairing, RTSP, video/audio setup) | debug packages |
| `[MLDIAG]` / `[mldiag]` | host engine checks / UI host-start test (`mlDiag`) | on request; `diag#` lines at host start in debug packages |

The `App Version` line names the build: `Moonlight v<ver> (tizen50-core <variant>
<commit>, UI <base UI version>)`. Check it first: it tells which package was tested.

### Filtering the logs

**In the console** (Chrome DevTools, the *Filter* box; a `/…/` value is a regular
expression, a leading `-` hides matches):

| Show | Filter |
|------|--------|
| only this project's lines | `/\[(mlbridge|mltrace|MLHOST|MLENGINE|MLDIAG|mldiag)\]|tizen50-core/` |
| only the diagnostics | `/diag#|\[(MLDIAG|mldiag)\]|host start/` |
| only host + engine | `/\[(MLHOST|MLENGINE)\]/` |
| the bridge round-trips | `/\[mlbridge\] (call|sent|reply|timeout|joined|startHost)/` |
| one command (e.g. id 28) | `/id=28\b/` |
| hide the base UI's chatter | `-/\[(index|utils|productinfo|AvInfo)\.js/` |

Set the level menu to *Errors* + *Warnings* to see only problems.

**In a saved console file** (one line per entry: `time LEVEL message (source)`):

```bash
F=tv-console-Moonlight5.ui-*.txt
# this project's lines, without the "(file:///…)" source suffix
grep -E '\[(mlbridge|mltrace|MLHOST|MLENGINE)\]|tizen50-core' $F | sed 's/   (file:[^)]*)$//'
# only errors and warnings
grep -E '^[0-9:]+ (ERROR|WARNING) ' $F
# host + engine only
grep -E '\[(MLHOST|MLENGINE)\]' $F
# what happened to one command (e.g. startStream id=28) and the lines around it
grep -n -E '\bid=28\b' $F
```

The base UI's lines carry CSS noise (`%c[index.js, …] color: green;`); to keep
them but make them readable add `| sed 's/%c//; s/ color: [a-z]*;//'`.

## Why this exists

The WebAssembly [moonlight-tizen](https://github.com/brightcraft/moonlight-tizen)
app needs **Tizen 5.5+** because it uses Samsung EMSS (elementary media stream
source) for the fast, low-latency video path. **Tizen 5.0 has no EMSS**, and the
public 5.0 SDK offers only the buffering `capi-media-player` path (high latency).

This project brings the fast path to 5.0 by reusing moonlight-tizen's portable
parts and replacing the EMSS video/audio path with a native engine built on
`libesplusplayer.so`, the TV's hardware ES player.

## What is reused vs. new

| | |
|---|---|
| **Reused unchanged** | `moonlight-common-c` (streaming core), `libgamestream` (pairing/HTTP), the web UI (host list, pairing, app grid, settings) |
| **Replaced** | EMSS video → `video_esplus.c`, EMSS/WebAudio → `audio_opus.c` + `audio_pulse.c`, the WASM transport → a message-port bridge, cert handling → native |
| **New** | `esplusplayer_tv50.h` (the TV's ES-player C API), the message-port bridge + JS shim, `esplus_player.c`, `input_evdev.c`, `gs_client.c` (+ own `xml_search`), and the build/packaging |

## Architecture

```
┌──────────────────────── Tizen app ────────────────────────┐
│  Web UI (.wgt)  — moonlight-tizen front-end                │
│      │  tizen.messageport  (mlbridge.js)                    │
│      ▼                                                      │
│  .NET host (.tpk)  — loads libmlengine.so (the engine)     │
│      makeCert / pair / startStream / input                 │
│      → moonlight-common-c + libgamestream + esplusplayer   │
└────────────────────────────────────────────────────────────┘
```

During streaming the engine owns the hardware video overlay; the web UI is the
menu/launcher.

## Latency & statistics

**Queue depth.** `depth_ms` = the PTS of the last video packet accepted by the
player minus `esplusplayer_get_playing_time`: how far ahead of the screen the
player queue is. A sampler thread reads it every `MLENGINE_DEPTH_POLL_MS` (the
counters and the sampler also run in release builds). `audio_depth_ms` is always 0: the
player has no audio stream. The sampler logs the A/V drift instead (audio written to
PulseAudio minus video playing time, relative to the first sample; over 250 ms it is traced,
the maximum is in the `aout` section of the `ML-ERROR` line).
**Audio.** The esplusplayer carries video only. Platform limitation: the TV player rejects engine-fed
audio streams (both Opus and raw PCM). The host sends Opus; the engine decodes it with libopus (fixed
point, from the submodule; `build-deps-lib.sh` makes `libopus_tv.a`) and writes the PCM S16LE to
PulseAudio with `libpulse-simple`, loaded at run time with `dlopen` (`audio_pulse.c`; no link-time
dependency). If PulseAudio is not available, the stream goes on without sound. The PulseAudio buffer
is set with `MLENGINE_AUDIO_LATENCY_MS` (default 40).

**Start retry.** If no video packet has reached the player 300 ms after prepare, the engine asks for an IDR frame again
(every 500 ms, at most 10 times): the first request can be lost before the host pipeline is up.

**Pacing modes** (`MLENGINE_LATENCY_MODE`, video only):

| Mode | Video PTS |
|---|---|
| `off` (default) | host PTS minus the first frame's PTS |
| `nudge` | the same minus a slow correction that moves `depth_ms` towards `MLENGINE_TARGET_DEPTH_MS` (at most `MLENGINE_NUDGE_MAX_MS_PER_S` ms per second; no change inside a hysteresis of max(10 ms, target/2); the correction is capped at ±1 s) |
| `arrival` (experimental) | the TV's monotonic clock at submit, from the first frame |

In all modes video PTS are strictly increasing (the player's PTS unit is 1 ms).

**Backpressure.** A `FULL` submit is retried 3 times (2 ms apart); then the frame
is dropped (`dropped_full`). With `MLENGINE_DROP_AFTER_MS` > 0, a non-IDR frame is
skipped (`dropped_late`, at most one per sampler tick) while `depth_ms` is above that
value (a skip does not move the PTS timeline, so it only helps when the decoder
falls behind; `nudge` is what lowers `depth_ms`). Both return `DR_NEED_IDR`:
moonlight-common-c then drops the frames that reference the lost one and asks the
host for an IDR.

**Recovery.** When playing time stops for `MLENGINE_STALL_MS` while frames are
still submitted, or after a player error, the engine does a flushing
`esplusplayer_seek` to `MLENGINE_RECOVER_BACK_MS` before the last video PTS and
forces an IDR. At most `MLENGINE_RECOVER_MAX` recoveries per session, at least
2 s apart; then it stops trying (`recovery_state` 2). A resource conflict (another
app took the player) is final: only `stop()` leaves it, so it never triggers a
recovery. The recovery watchdog never ends the session itself.

**Player rebuild.** When `start()` fails after prepare, or the playing time cannot be read for `MLENGINE_STALL_MS`
while frames arrive, the engine rebuilds the player once before the fatal watchdogs act: stop, close, create, open,
the cached video stream info, then (on the ecore main loop, without waiting) display and `prepare_async`. The video
sink drops frames until an IDR. At most `MLENGINE_PLAYER_REBUILD_MAX` rebuilds per session (default 1, 0 = off). The
watchdog timers restart with the rebuild, so a second failure ends the session as before. It needs the window and the
cached stream info; the count is `rebuilds=` in the `ML-ERROR` line.

**Fatal watchdogs.** These end the session with an `ML-ERROR` line (the host reads
it from `ml_engine_running()`) and record the raw player state (`state=`). The player
is prepared when the video sink starts, and is started once `prepare_async_done` arrives, also with ok=0 (#22). If `start()` never
succeeds although video packets were submitted for `MLENGINE_START_TIMEOUT_MS`, the
code is `-110` PLAYER_NOT_STARTED. If playing time stays frozen for
`MLENGINE_FATAL_STALL_MS` while frames are submitted, the code is `-111`
PLAYER_STALLED. A playing time that cannot be read counts as frozen. With recovery on, the stall timer restarts after each recovery, so
`-111` comes only after recovery gave up.

**Knobs.** Environment variables of the engine process, read once when the player
is created (for example on the CLI harness command line; the .NET host sets none):

| Variable | Default | Meaning |
|---|---|---|
| `MLENGINE_LATENCY_MODE` | `off` | `off` \| `nudge` \| `arrival` |
| `MLENGINE_TARGET_DEPTH_MS` | `50` | `nudge` target queue depth |
| `MLENGINE_NUDGE_MAX_MS_PER_S` | `10` | max `nudge` correction rate |
| `MLENGINE_DROP_AFTER_MS` | `0` (off) | late-frame skip threshold |
| `MLENGINE_DEPTH_POLL_MS` | `250` | sampler period; below 50 the sampler does not run |
| `MLENGINE_STALL_MS` | `700` | watchdog: no playing-time progress |
| `MLENGINE_RECOVER_MAX` | `0` (off) | recoveries per session (3 recommended once validated on a TV) |
| `MLENGINE_RECOVER_BACK_MS` | `250` | seek target behind the last video PTS |
| `MLENGINE_START_TIMEOUT_MS` | `5000` (0 = off) | fatal `-110`: no successful `start()` this long after the first video submit |
| `MLENGINE_AUDIO_LATENCY_MS` | `40` | PulseAudio buffer (target length) in ms; `0` = server default |
| `MLENGINE_PLAYER_REBUILD_MAX` | `1` (0 = off) | player rebuilds per session after a failed `start()` or an unreadable playing time |
| `MLENGINE_FATAL_STALL_MS` | `10000` (0 = off) | fatal `-111`: playing time frozen this long while submitting |

**Overlay.** `toggleStats` in the UI turns on a live overlay (`mlstats` events from
`ml_engine_get_stats`, every 500 ms); see `pkg/ui/PROTOCOL.md`.

## Build (headless, Docker)

Everything builds in one **self-contained image** (`pkg/Dockerfile`): Tizen Studio
CLI + `sdb` (base `vitalets/docker-tizen-webos-sdk`), .NET SDK 8 with the host's
NuGet packages, the Tizen 5.0 cross toolchain + sysroot (`/opt/tizen-deps`) and the
crypto/http deps (`/opt/backport-deps`). A build downloads nothing; CI runs it with
`--network none`. CI publishes the image to GitHub Container Registry
(`.github/workflows/build-image.yml`):

```
ghcr.io/<owner>/moonlight-tizen-build:deps-<hash>   # hash of the inputs: pkg/image-tag.sh
ghcr.io/<owner>/moonlight-tizen-build:latest
```

The one thing **not** in the image: `tv-libs/` — the `.so` libraries copied from
**your** TV (Samsung's, never redistribute them). Mount them at
`/opt/tizen-deps/tv-libs`.

```bash
# 1. one-time: get the web UI + moonlight-common-c (submodule)
git submodule update --init --depth 1 third_party/moonlight-tizen

# 2. get the image (private package: docker login ghcr.io first), or build it
docker pull ghcr.io/<owner>/moonlight-tizen-build:latest
#   docker build -f pkg/Dockerfile -t ghcr.io/<owner>/moonlight-tizen-build:latest .

# 3. build the engine .so, the .NET host .tpk, and the web .wgt
#    BUILD_VARIANT=release (default) or debug (trace build)
docker run --rm --network none -v "$PWD":/work/repo \
  -v /path/to/tv-libs:/opt/tizen-deps/tv-libs:ro \
  ghcr.io/<owner>/moonlight-tizen-build:latest bash -lc '
    export BUILD_VARIANT=debug                   # omit for a normal release build
    cd /work/repo; REPO=$PWD MLT=$PWD/third_party/moonlight-tizen
    DEPS=/opt/tizen-deps BPD=/opt/backport-deps DEPSBUILD=/work/depsbuild
    MLT=$MLT DEPS=$DEPS BPD=$BPD OUT=$DEPSBUILD ./build-deps-lib.sh
    MLT=$MLT DEPS=$DEPS BPD=$BPD DEPSBUILD=$DEPSBUILD OUT=/work/engine mlclient/build-engine-so.sh
    pkg/build-tpk.sh /work/engine/libmlengine.so | tee /tmp/tpk.log   # -> the .NET host .tpk
    REPO=$REPO MLT=$MLT SIGN_PROFILE=dev pkg/build-package.sh || true  # -> the web .wgt'
```

`.github/workflows/build-packages.yml` is the reference for the full sequence.

## Requirements

- Samsung TV, **Tizen 5.0**, Muse.M, armv7l.
- A GameStream host on the LAN (Sunshine / GeForce Experience / Wolf).
- To build: Linux with Docker/podman, the build image, and the TV's `/lib` + `/usr/lib` (copied once).

## Repository layout

```
esplusplayer_tv50.h   C API for the TV's libesplusplayer.so
mlclient/             native engine + CLI harness (video, audio, input, pairing)
pkg/                  app package: Dockerfile, .NET host (tpk), web UI (wgt), build
tests/                esppplay probe + host self-tests used during bring-up
third_party/          moonlight-tizen (web UI + moonlight-common-c), git submodule
fetch-*.sh build-*.sh reproducible build scripts
```

## Credits & license

This project stands on several others:

| Project | What we use |
|---|---|
| [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) | The streaming core — RTSP, control stream, FEC, the decoder/audio callback ABI. Used unchanged. |
| [moonlight-embedded](https://github.com/moonlight-embedded/moonlight-embedded) | `libgamestream` (pairing + GameStream HTTP), and its `client.c` as the reference for our native pairing/launch (`gs_client.c`). |
| [moonlight-tizen](https://github.com/brightcraft/moonlight-tizen) | The web UI — host list, pairing, app grid, settings — reused as-is, with the WASM transport swapped for our message-port bridge. |
| [Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung) | The end-user install tool — reads the TV DUID, signs each package for it, installs. |
| [vitalets/docker-tizen-webos-sdk](https://github.com/vitalets/docker-tizen-webos-sdk) + the [Jellyfin](https://github.com/jellyfin/jellyfin-tizen) install flow | The headless build+install toolchain our Docker image and `install-on-tv.sh` follow. |

**License:** `moonlight-common-c` and `libgamestream` are GPLv3, so this project is
**GPLv3**.

Samsung's TV libraries (`libesplusplayer.so` etc.) are **not** included and must not
be redistributed; they are used only locally, linked against copies from your own
device.

## Disclaimer

Independent, unofficial project. Not affiliated with or endorsed by Samsung,
NVIDIA, or the Moonlight project.
