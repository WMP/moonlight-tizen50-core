#!/bin/sh
# Build libmlengine.so (the engine, PIC) + a thin mlclient exe that links it.
# Same inputs as build-client.sh. Produces $OUT/libmlengine.so and $OUT/mlclient.
#
# Env: MLT, DEPS (no spaces), BPD, DEPSBUILD.  (Paths must be space-free: the
# compiler flags are unquoted. Use a space-free symlink to the repo for MLT.)
set -e
MLT=${MLT:?}; DEPS=${DEPS:?}; BPD=${BPD:?}; DEPSBUILD=${DEPSBUILD:?}
HERE=$(cd "$(dirname "$0")" && pwd)
GCC=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-gcc
OBJD=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-objdump
C=$MLT/moonlight-common-c; TVL=$DEPS/tv-libs/usr/lib
# BUILD_VARIANT: release (default, -O2) or debug (-O0 -g, MLT_TRACE trace on).
BUILD_VARIANT=${BUILD_VARIANT:-release}
case "$BUILD_VARIANT" in
  release) OPT="-O2"; LOPT="-O2" ;;
  debug)   OPT="-O0 -g -DMLT_TRACE"; LOPT="-O0 -g" ;;
  *) echo "BUILD_VARIANT must be debug or release, got: $BUILD_VARIANT" >&2; exit 1 ;;
esac
echo "engine variant: $BUILD_VARIANT"
OUT=${OUT:-./engine-build}; mkdir -p "$OUT"
cp "$HERE/../esplusplayer_tv50.h" "$OUT/"
ARCH="-mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp"
FLAGS="--sysroot=$DEPS/sysroot $ARCH $OPT -std=gnu11 -Wall -Wextra -fPIC"
INC="-I$C/src -I$MLT/opus/include -I$OUT -I$HERE -I$MLT/libgamestream -I$BPD/ossl-inc -I$BPD/curl759/include"

# engine objects (everything except the thin main.c)
for src in cert_init engine_api engine_ui engine_pad video_esplus audio_opus audio_pulse esplus_player gs_client input_evdev mldiag ctlcrypto; do
  "$GCC" $FLAGS $INC -c "$HERE/$src.c" -o "$OUT/$src.o"
done

# libmlengine.so: engine objects + the static deps (moonlight-common-c core,
# libgamestream, mbedcrypto) + the TV's shared libs, resolved into the .so.
# --hash-style=both: the .NET host loads the engine from memory (so downloaded
# engine updates apply without rewriting the app folder) and its fallback ELF
# loader resolves symbols via a SYSV DT_HASH. The engine is also built TLS-free
# (that loader can't do R_ARM_TLS_*); verified: no _Thread_local in the
# engine/moonlight-common-c.
"$GCC" --sysroot=$DEPS/sysroot $ARCH $LOPT -shared -Wl,-soname,libmlengine.so \
  -Wl,--hash-style=both -Wl,--wrap=PltEncryptMessage \
  -o "$OUT/libmlengine.so" \
  "$OUT"/cert_init.o "$OUT"/engine_api.o "$OUT"/engine_ui.o "$OUT"/engine_pad.o "$OUT"/video_esplus.o "$OUT"/audio_opus.o "$OUT"/audio_pulse.o \
  "$OUT"/esplus_player.o "$OUT"/gs_client.o "$OUT"/input_evdev.o "$OUT"/mldiag.o "$OUT"/ctlcrypto.o \
  "$DEPSBUILD"/obj/*.o "$DEPSBUILD"/gs/*.o "$DEPSBUILD/libmbedcrypto_tv.a" "$DEPSBUILD/libopus_tv.a" \
  -L"$TVL" -Wl,-rpath-link,"$TVL" -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-in-shared-libs \
  -l:libesplusplayer.so -l:libecore_wl2.so.1 -l:libecore.so.1 \
  -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -l:libcurl.so.4 -lpthread -ldl

# thin exe: main.o + the .so
"$GCC" $FLAGS $INC -c "$HERE/main.c" -o "$OUT/main.o"
"$GCC" --sysroot=$DEPS/sysroot $ARCH $LOPT -o "$OUT/mlclient" "$OUT/main.o" \
  -L"$OUT" -Wl,-rpath-link,"$TVL" -Wl,--unresolved-symbols=ignore-in-shared-libs -lmlengine -Wl,-rpath,'$ORIGIN/../lib'

echo "built: $(file "$OUT/libmlengine.so" | cut -d, -f1-3)"
echo "       $(file "$OUT/mlclient" | cut -d, -f1-3)"

# self-check 1: the ABI symbols are exported by the .so
miss=""
for s in ml_engine_start ml_engine_running ml_engine_wait ml_engine_stop \
         ml_engine_make_cert ml_engine_http_init ml_engine_open_url ml_engine_pair \
         ml_engine_cancel_request ml_engine_start_session ml_engine_send_key ml_engine_free \
         ml_engine_set_log_cb ml_engine_get_stats ml_engine_start_session_v2 ml_engine_last_error \
         ml_engine_gamepad_arrival ml_engine_gamepad_state; do
  "$OBJD" -T "$OUT/libmlengine.so" | grep -q " $s\$" || miss="$miss $s"
done
[ -z "$miss" ] && echo "ABI exported: ml_engine_* OK" || { echo "MISSING ABI:$miss"; exit 1; }

# self-check 2: no glibc symbol newer than the TV's 2.24
bad=$("$OBJD" -T "$OUT/libmlengine.so" "$OUT/mlclient" | grep -oE 'GLIBC_2\.(2[5-9]|[3-9][0-9])' | sort -u || true)
[ -z "$bad" ] && echo "glibc symbols: all <= 2.24 OK" || { echo "TOO-NEW glibc: $bad"; exit 1; }
