#!/bin/sh
# Build + run tests/esplus_ecore_test.c and tests/video_esplus_test.c on the host (no TV, no toolchain).
# esplusplayer_tv50.h asserts the 32-bit ARM struct layout; a 64-bit host cannot meet
# it, and the test does not depend on it, so those asserts are compiled out here.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); R="$HERE/.."
OUT=${OUT:-${TMPDIR:-/tmp}/esplus_ecore_test}
LC="$R/third_party/moonlight-tizen/moonlight-common-c/src"
[ -f "$LC/Limelight.h" ] || { echo "Limelight.h missing: run 'git submodule update --init'" >&2; exit 1; }
# libopus for the host (the engine decodes Opus to PCM; the test also uses the encoder). Third-party code: -w.
OPUS="$R/third_party/moonlight-tizen/opus"
OPUSD=$(dirname "$OUT")/esplus_opus_host
. "$R/mlclient/opus-sources.sh"
if [ ! -f "$OPUSD/libopus_host.a" ]; then
  mkdir -p "$OPUSD"
  for f in $(opus_sources "$OPUS"); do
    gcc -std=gnu99 -w -O1 -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF -DFIXED_POINT -DOPUS_BUILD \
      -I"$OPUS/include" -I"$OPUS/src" -I"$OPUS/silk" -I"$OPUS/celt" -I"$OPUS/silk/fixed" \
      -c "$OPUS/$f" -o "$OPUSD/$(echo "$f" | tr / _ | sed 's/\.c$//').o"
  done
  rm -f "$OPUSD/libopus_host.a.tmp"; ar rcs "$OPUSD/libopus_host.a.tmp" "$OPUSD"/*.o; mv "$OPUSD/libopus_host.a.tmp" "$OPUSD/libopus_host.a"
fi
for D in "" "-DMLT_TRACE"; do
  gcc -std=gnu11 -Wall -Wextra -Werror $D '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" \
    -o "$OUT" "$HERE/esplus_ecore_test.c" "$R/mlclient/esplus_player.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" -I"$LC" \
    -o "$OUT-video" "$HERE/video_esplus_test.c" "$R/mlclient/video_esplus.c" "$R/mlclient/esplus_player.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-video" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D -I"$R/mlclient" -I"$LC" \
    -o "$OUT-pad" "$HERE/engine_pad_test.c" "$R/mlclient/engine_pad.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-pad" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" -I"$LC" -I"$OPUS/include" \
    -o "$OUT-audio" "$HERE/audio_opus_test.c" "$R/mlclient/audio_opus.c" "$R/mlclient/mldiag.c" "$OPUSD/libopus_host.a" -lpthread -ldl -lm
  "$OUT-audio" 2>/dev/null
  # PulseAudio writer: dlopen/dlsym wrapped, no libpulse
  gcc -std=gnu11 -Wall -Wextra -Werror $D -I"$R/mlclient" -Wl,--wrap=dlopen,--wrap=dlsym \
    -o "$OUT-pulse" "$HERE/audio_pulse_test.c" "$R/mlclient/audio_pulse.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-pulse" 2>/dev/null
done
# mldiag self-test
gcc -std=gnu11 -Wall -Wextra -Werror -I"$R/mlclient" -o "$OUT-diag" "$HERE/mldiag_test.c" "$R/mlclient/mldiag.c" -lpthread
"$OUT-diag"
# control-stream encrypt check (needs OpenSSL headers)
if printf '#include <openssl/evp.h>\n' | gcc -E - >/dev/null 2>&1; then
  gcc -std=gnu11 -Wall -Wextra -Werror -I"$R/mlclient" -o "$OUT-ctl" "$HERE/ctlcrypto_test.c" "$R/mlclient/ctlcrypto.c" "$R/mlclient/mldiag.c" -lpthread -lcrypto
  "$OUT-ctl"
else
  echo "ctlcrypto_test SKIPPED: no OpenSSL headers"
fi
