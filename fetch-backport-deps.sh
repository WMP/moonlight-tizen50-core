#!/bin/sh
# Milestone 1 step 2: fetch the crypto/http deps the native Moonlight client
# needs and that are NOT in the Tizen 5.0 sysroot. Picks versions matching the
# TV's own libraries so we can link against them:
#   - mbedTLS 2.28.8 LTS  -> for moonlight-common-c PlatformCrypto.c (built from source)
#   - OpenSSL 1.0.2o headers -> TV has libcrypto.so.1.0.0 == OpenSSL 1.0.2o
#   - libcurl 7.59.0 headers -> TV has libcurl/7.59.0
# OpenSSL/curl are linked from the TV libs (tv-libs), only their headers are fetched.
#
# Output dir layout (default ./backport-deps):
#   mbedtls/            mbedTLS 2.28.8 source
#   ossl-inc/openssl/   flat OpenSSL 1.0.2 headers + hand-written opensslconf.h (32-bit ARM)
#   curl759/include/    curl 7.59 headers
set -e
OUT=${OUT:-./backport-deps}
mkdir -p "$OUT"
cd "$OUT"

[ -d mbedtls ]  || git clone --depth 1 -b mbedtls-2.28.8   https://github.com/Mbed-TLS/mbedtls.git mbedtls
[ -d curl759 ]  || git clone --depth 1 -b curl-7_59_0      https://github.com/curl/curl.git        curl759
[ -d openssl102 ] || git clone --depth 1 -b OpenSSL_1_0_2o https://github.com/openssl/openssl.git   openssl102

# OpenSSL 1.0.2 keeps headers in source dirs; include/openssl is populated at
# configure time. Build a flat include dir by symlink, plus an ARM opensslconf.h.
mkdir -p ossl-inc/openssl
find openssl102/crypto openssl102/ssl -name '*.h' | while read -r h; do
  ln -sf "$(realpath "$h")" "ossl-inc/openssl/$(basename "$h")"
done
ln -sf "$(realpath openssl102/e_os2.h)" ossl-inc/openssl/e_os2.h
cat > ossl-inc/openssl/opensslconf.h <<'EOF'
#ifndef OPENSSL_CONF_TV
#define OPENSSL_CONF_TV
#define OPENSSL_THREADS
#define OPENSSL_NO_STATIC_ENGINE
#define OPENSSL_NO_KRB5            /* TV libcrypto built without kerberos */
#define THIRTY_TWO_BIT
#undef  SIXTY_FOUR_BIT
#undef  SIXTY_FOUR_BIT_LONG
#define BN_LLONG
#define RC4_INT unsigned int
#define DES_LONG unsigned long
#define RC4_CHUNK unsigned long
#define DES_INT
#define OPENSSL_UNISTD <unistd.h>
#define OPENSSL_EXPORT_VAR_AS_FUNCTION
#endif
EOF
echo "backport deps ready in $OUT"

# Build only the AC-3 encoder and its utility library for the TV target.
if [ ! -f ffmpeg-arm/lib/libavcodec.a ]; then
  tizen_deps=$(realpath "${DEPS:-/opt/tizen-deps}")
  out=$(pwd)
  cross="$tizen_deps/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-"
  curl -fsSL --retry 3 https://ffmpeg.org/releases/ffmpeg-6.1.3.tar.xz -o ffmpeg-6.1.3.tar.xz
  printf '%s  %s\n' bc5f1e4a4d283a6492354684ee1124129c52293bcfc6a9169193539fbece3487 ffmpeg-6.1.3.tar.xz | sha256sum -c -
  tar -xf ffmpeg-6.1.3.tar.xz
  (
    cd ffmpeg-6.1.3
    ./configure --prefix="$out/ffmpeg-arm" --enable-cross-compile --cross-prefix="$cross" \
      --arch=arm --target-os=linux --sysroot="$tizen_deps/sysroot" \
      --extra-cflags="-mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp" \
      --disable-autodetect --disable-everything --disable-programs --disable-doc --disable-asm \
      --disable-avdevice --disable-avfilter --disable-avformat --disable-swscale --disable-swresample \
      --enable-encoder=ac3_fixed --enable-pic
    make -j"$(getconf _NPROCESSORS_ONLN)"
    make install
  )
fi
