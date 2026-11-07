#!/bin/sh
# Build the native Moonlight client ELF (SKELETON - see main.c) for Tizen 5.0:
# cert_init + main + video_esplus + moonlight-common-c core + PlatformCrypto
# (mbedTLS) + libgamestream, linked against the TV's esplusplayer/ecore/openssl/
# curl. Produces ./mlclient-build/mlclient.
#
# Env: MLT, DEPS (no spaces), BPD (fetch-backport-deps.sh), DEPSBUILD (build-deps-lib.sh).
set -e
MLT=${MLT:?}; DEPS=${DEPS:?}; BPD=${BPD:?}; DEPSBUILD=${DEPSBUILD:?}
HERE=$(cd "$(dirname "$0")" && pwd)
GCC=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-gcc
OBJD=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-objdump
C=$MLT/moonlight-common-c; TVL=$DEPS/tv-libs/usr/lib
OUT=${OUT:-./mlclient-build}; mkdir -p "$OUT"
# esplusplayer_tv50.h lives in  (parent). Copy it to the space-free OUT so
# the -I path has no spaces (unquoted compiler flags word-split on spaces).
cp "$HERE/../esplusplayer_tv50.h" "$OUT/"
FLAGS="--sysroot=$DEPS/sysroot -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp -O2 -std=gnu11 -Wall -Wextra"
INC="-I$C/src -I$OUT -I$MLT/libgamestream -I$BPD/ossl-inc -I$BPD/curl759/include"

for src in cert_init main video_esplus audio_opus audio_pulse esplus_player gs_client input_evdev mldiag; do
  "$GCC" $FLAGS $INC -c "$HERE/$src.c" -o "$OUT/$src.o"
done

"$GCC" --sysroot=$DEPS/sysroot -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp -O2 \
  -o "$OUT/mlclient" "$OUT"/cert_init.o "$OUT"/main.o "$OUT"/video_esplus.o "$OUT"/audio_opus.o "$OUT"/audio_pulse.o "$OUT"/esplus_player.o "$OUT"/gs_client.o "$OUT"/input_evdev.o "$OUT"/mldiag.o \
  "$DEPSBUILD"/obj/*.o "$DEPSBUILD"/gs/*.o "$DEPSBUILD/libmbedcrypto_tv.a" "$DEPSBUILD/libopus_tv.a" \
  -L"$TVL" -Wl,-rpath-link,"$TVL" -Wl,--allow-shlib-undefined \
  -l:libesplusplayer.so -l:libecore_wl2.so.1 -l:libecore.so.1 \
  -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -l:libcurl.so.4 -lpthread -ldl
echo "built: $(file "$OUT/mlclient" | cut -d, -f1-3)"
bad=$("$OBJD" -T "$OUT/mlclient" | grep -oE 'GLIBC_2\.(2[5-9]|[3-9][0-9])' | sort -u || true)
[ -z "$bad" ] && echo "glibc symbols: all <= 2.24 OK" || { echo "TOO-NEW glibc: $bad"; exit 1; }
