#!/bin/sh
# Build + run tests/esplus_ecore_test.c, tests/video_esplus_test.c and tests/esplus_hdr_test.c on the host (no TV, no toolchain).
# esplusplayer_tv50.h asserts the 32-bit ARM struct layout; a 64-bit host cannot meet
# it, and the test does not depend on it, so those asserts are compiled out here.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); R="$HERE/.."
cd "$R"   # the opus cflags are passed unquoted: keep them free of spaces (a checkout path may have them)
OUT=${OUT:-${TMPDIR:-/tmp}/esplus_ecore_test}
LC="$R/third_party/moonlight-tizen/moonlight-common-c/src"
[ -f "$LC/Limelight.h" ] || { echo "Limelight.h missing: run 'git submodule update --init'" >&2; exit 1; }
# libopus for the host (the engine decodes Opus to PCM; the test also uses the encoder). Third-party code: -w.
OPUS=third_party/moonlight-tizen/opus
. "$R/mlclient/opus-sources.sh"
OUT=$(mkdir -p "$(dirname "$OUT")" && cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")   # absolute: never write into the working tree
OPUSF=$(opus_cflags "$OPUS")
# the cache dir is named by a hash of the source list, the flags and the file contents: a changed submodule or list gives a new dir
OPUSH=$( { opus_sources "$OPUS"; echo "$OPUSF"; for f in $(opus_sources "$OPUS"); do cksum < "$OPUS/$f"; done; } | cksum | tr ' ' _)
OPUSD=$(dirname "$OUT")/esplus_opus_host_$OPUSH
if [ ! -f "$OPUSD/libopus_host.a" ]; then
  rm -rf "$OPUSD"; mkdir -p "$OPUSD/o"
  objs=""
  for f in $(opus_sources "$OPUS"); do
    o="$OPUSD/o/$(echo "$f" | tr / _ | sed 's/\.c$//').o"; objs="$objs $o"
    gcc -w -O1 $OPUSF -c "$OPUS/$f" -o "$o"
  done
  ar rcs "$OPUSD/libopus_host.a.tmp" $objs; mv "$OPUSD/libopus_host.a.tmp" "$OPUSD/libopus_host.a"
fi
# the decoder-only list (what the TV build archives) must link and decode: a missing decoder file fails here
DECD="$OUT-opusdec"; rm -rf "$DECD"; mkdir -p "$DECD"; objs=""
for f in $(opus_sources "$OPUS" dec); do
  o="$DECD/$(echo "$f" | tr / _ | sed 's/\.c$//').o"; objs="$objs $o"; gcc -w -O1 $OPUSF -c "$OPUS/$f" -o "$o"
done
ar rcs "$DECD/libopus_dec.a" $objs
gcc -std=gnu11 -Wall -Wextra -Werror -I"$OPUS/include" -o "$OUT-opusdec-test" "$HERE/opus_dec_link_test.c" "$DECD/libopus_dec.a" -lm
"$OUT-opusdec-test"
for D in "" "-DMLT_TRACE"; do
  gcc -std=gnu11 -Wall -Wextra -Werror $D '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" \
    -o "$OUT" "$HERE/esplus_ecore_test.c" "$R/mlclient/esplus_player.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D -DNO_OSD_SYMS '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" \
    -o "$OUT-nosym" "$HERE/esplus_ecore_test.c" "$R/mlclient/esplus_player.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-nosym" nosym 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" -I"$LC" \
    -Wl,--wrap=esplus_set_color_info -o "$OUT-video" "$HERE/video_esplus_test.c" "$R/mlclient/video_esplus.c" "$R/mlclient/h264_sps.c" "$R/mlclient/esplus_hdr.c" "$R/mlclient/esplus_player.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-video" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D -Ddlopen=fake_dlopen -Ddlsym=fake_dlsym -Ddlclose=fake_dlclose '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" \
    -o "$OUT-hdr" "$HERE/esplus_hdr_test.c" "$R/mlclient/esplus_hdr.c"
  "$OUT-hdr" 2>/dev/null
  "$OUT-hdr" nolib 2>/dev/null
  "$OUT-hdr" nostate 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D -I"$R/mlclient" -I"$LC" \
    -o "$OUT-pad" "$HERE/engine_pad_test.c" "$R/mlclient/engine_pad.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-pad" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D -Ddlopen=fake_dlopen -Ddlsym=fake_dlsym -Ddlclose=fake_dlclose -I"$R/mlclient" \
    -o "$OUT-avoc" "$HERE/engine_avoc_test.c" "$R/mlclient/engine_avoc.c"
  "$OUT-avoc" 2>/dev/null
  "$OUT-avoc" nolib 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D -I"$R/mlclient" -I"$LC" -I"$R/third_party/moonlight-tizen/libgamestream" \
    -o "$OUT-api" "$HERE/engine_api_test.c" "$R/mlclient/engine_api.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-api" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D -DML_AUDIO_IO_FAKE '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" -I"$LC" -I"$R/third_party/moonlight-tizen/libgamestream" -I"$OPUS/include" \
    -o "$OUT-audio" "$HERE/audio_opus_test.c" "$R/mlclient/audio_opus.c" "$R/mlclient/mldiag.c" "$OPUSD/libopus_host.a" \
    -lpthread -ldl -lm
  "$OUT-audio" 2>/dev/null
done
# SPS patch test: the h264bitstream reader/writer (third-party, -w) is the oracle
H264=third_party/moonlight-tizen/h264bitstream
HBD="$OUT-h264bs"; rm -rf "$HBD"; mkdir -p "$HBD"
for f in h264_stream h264_nal h264_sei; do gcc -w -O1 -c "$H264/$f.c" -o "$HBD/$f.o"; done
gcc -std=gnu11 -Wall -Wextra -Werror -I"$R/mlclient" -I"$H264" -c "$HERE/h264_sps_test.c" -o "$HBD/test.o"
gcc -std=gnu11 -Wall -Wextra -Werror -I"$R/mlclient" -c "$R/mlclient/h264_sps.c" -o "$HBD/sps.o"
gcc -o "$OUT-sps" "$HBD/test.o" "$HBD/sps.o" "$HBD"/h264_*.o
"$OUT-sps"
# the same under ASan + UBSan, when the compiler has them
if echo 'int main(void){return 0;}' | gcc -fsanitize=address,undefined -x c - -o "$HBD/probe" >/dev/null 2>&1; then
  SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -g"
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -I"$R/mlclient" -I"$H264" -c "$HERE/h264_sps_test.c" -o "$HBD/test_san.o"
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -I"$R/mlclient" -c "$R/mlclient/h264_sps.c" -o "$HBD/sps_san.o"
  gcc $SAN -o "$OUT-sps-san" "$HBD/test_san.o" "$HBD/sps_san.o" "$HBD"/h264_*.o
  "$OUT-sps-san"
else
  echo "h264_sps_test sanitizer run SKIPPED: no ASan/UBSan"
fi
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
