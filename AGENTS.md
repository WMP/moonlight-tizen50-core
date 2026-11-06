# AGENTS.md — rules for automated contributors

This repository is **self-contained**. Everything an agent needs to build, test
and improve it is in this tree. Work only within this repository.

## Hard boundaries

- **Do not connect this repository to any other repository.** Do not add git
  remotes, fetch, pull, push, submodule, subtree or mirror to or from any other
  repo. Do not search for, reference, or link a related, upstream, downstream,
  fork, or "main" repository. If a task seems to require another repository, stop
  and ask the human instead.
- **Keep this repository clean.** Do not add content about device rooting,
  exploits, jailbreaks, security-control bypasses, or reverse-engineering
  write-ups (process, disassembly, offsets, provenance narratives) — not in code,
  comments, docs, commit messages, or issues. This is a normal open-source app.
- **Never commit Samsung's TV libraries** (`libesplusplayer.so` and the other
  files under `tv-libs/`). They are proprietary, used only locally at build time,
  and are provided via the `TV_LIBS_URL` CI secret / a local mount.

## What to work on

The point of this repo is to improve the client itself:

- **Performance** of the streaming engine (`mlclient/`): latency, memory, the
  video/audio paths (`video_esplus.c`, `audio_opus.c`, `esplus_player.c`).
- **The UI** and the UI↔engine bridge (`pkg/ui/mlbridge.js`, the message port).
- **Packaging** and the build (`pkg/`, `*.sh`).

## Build & test

- Initialize the moonlight-tizen submodule first: `git submodule update --init`.
- Build everything in the self-contained image `pkg/Dockerfile` (context = repo root:
  `docker build -f pkg/Dockerfile .`; published to ghcr.io by `build-image.yml`, tag from
  `pkg/image-tag.sh`). It must hold every build dependency: builds run with
  `--network none`. Never add the TV libraries (`tv-libs/`) to the image.
  See `README.md` for the full flow (engine `.so` → `.tpk` host → web `.wgt`).
- Host self-tests that need no TV: `mlclient/xml_selftest.c` (XML parser),
  `tests/esppplay.c -DSELFTEST` (access-unit splitter). Run these after touching the
  parser or the bitstream code.
- Player/window self-test (host gcc): `sh tests/build-esplus-ecore-test.sh` (ecore balance,
  statistics, `nudge` pacing, watchdog/recovery, sampler join; plus the video sink
  test `tests/video_esplus_test.c`: PTS modes, FULL retries, late-frame cap). Needs the submodule.
  Run it after touching `mlclient/esplus_player.c`, `mlclient/esplus_stats.h` or `mlclient/video_esplus.c`.
  The same script also runs `tests/mldiag_test.c` (the `ML-ERROR` line); run it after touching `mlclient/mldiag.c`.
  It also runs `tests/engine_pad_test.c` (gamepad calls, clamps, `pad` counters); run it after touching `mlclient/engine_pad.c`.
  It also runs `tests/audio_opus_test.c` (Opus decode to PCM); run it after touching `mlclient/audio_opus.c`.
- UI bridge self-test (needs only node): `node tests/mlbridge_test.js`. Run it after touching
  `pkg/ui/mlbridge.js`.
- `BUILD_VARIANT=debug|release` (default `release`) is honoured by
  `mlclient/build-engine-so.sh`, `pkg/build-tpk.sh` and `pkg/build-package.sh`. The
  trace macro is `MLT_TRACE` in `mlclient/mltrace.h`; it must compile to nothing in
  release.
- The engine ABI (`mlclient/mlengine.h`) is **additive only**: never change a
  signature, only append functions.

## Error reports and diagnostics

- **Every failure must produce one `ML-ERROR` line.** When a stream or a user action
  fails (`connectionTerminated` with a non-zero code, `stageFailed`, a failed command),
  the engine writes one line that starts with `ML-ERROR`. It must work in release builds
  (not only with `MLT_TRACE`). Build it in `mlclient/mldiag.c`.
- **The line must hold all data needed to find the cause without a second test run:**
  - error code and name, the failed stage, time since session start;
  - session config: resolution, fps, bitrate, packet size, video formats, color space
    and range, audio config, server codec mode, encryption flags, server versions;
  - counters of each step: packets, frames, IDR frames, player open/window/prepare/prepare-done/start,
    submits per stream type and their failures;
  - control-stream encryption check (messages, failures, IV length, self-decrypt result) and whether the
    session key and key id match the `/launch` or `/resume` request;
  - the RTSP transport scheme (`rtsp` vs `rtspenc`) the host returned, since the encryption features
    are read from that response;
  - the counted moonlight-common-c log messages and the first and last library log lines.
- **The line must reach the user.** The engine returns it with `ml_engine_last_error()`,
  the host writes it to dlog and sends it in the `diag` field of `streamTerminated`, and
  the UI writes it to the console with `console.error('[ML-ERROR] ...')`.
- **No secrets in the line.** Never put the rikey, PIN, certificates, client unique id,
  RTSP session URL or host address in it.
- **When you handle a bug report:**
  1. Find the `ML-ERROR` line in the reporter's log. Do not guess the cause from a log
     without it or from a log that starts in the middle of a session.
  2. If the line does not contain the data that you need, first add that data to the
     line (and to `tests/mldiag_test.c`), then ask the reporter for a new log.
  3. Ask the reporter for: the `ML-ERROR` line, the full log from before `startStream`,
     the stream settings (resolution, fps, bitrate, codec, HDR) and the server name and version.
- When you add a new failure path, add its data to the `ML-ERROR` line in the same change.

## Branches and pull requests

- **Every branch you push gets a pull request at once**, with a full description:
  the goal (with a link to the issue, `Refs #n` or `Closes #n`), the changes per area,
  how to check the result (for diagnostics: how to read the new data), the tests you ran
  and the checks you could not run.
- When you push more commits to the branch, update the pull request description so it
  matches the whole branch.
