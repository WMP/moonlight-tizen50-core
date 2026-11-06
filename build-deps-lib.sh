#!/bin/sh
# Milestone 1 step 2: build mbedTLS + the full Moonlight core/crypto/gamestream
# object set for Tizen 5.0, and link-test it against the TV's real OpenSSL 1.0.2,
# libcrypto, libcurl. Proves the whole dependency chain links.
#
# Result (2026-11-01): mbedTLS 96/96, PlatformCrypto + libgamestream compile,
# link test produces an ARM ELF with NEEDED libssl.so.1.0.0/libcrypto.so.1.0.0/
# libcurl.so.4 and no glibc symbol above 2.24.
#
# Env:
#   MLT   = moonlight-tizen checkout
#   DEPS  = tizen-deps dir (no spaces; symlink if needed)
#   BPD   = backport-deps dir from fetch-backport-deps.sh
set -e
MLT=${MLT:?set MLT}; DEPS=${DEPS:?set DEPS (no spaces)}; BPD=${BPD:?set BPD}
GCC=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-gcc
AR=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-ar
OBJD=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-objdump
C=$MLT/moonlight-common-c
TVL=$DEPS/tv-libs/usr/lib

# -core patches to the vendored moonlight-common-c (see common-c-patches/README.md; none at the moment).
# Applied to the submodule checkout before compiling; idempotent (reverse-check -> skip).
# A1 (fork brightcraft/moonlight-tizen) makes these unnecessary and they become no-ops.
HERE=$(cd "$(dirname "$0")" && pwd)
for p in "$HERE"/common-c-patches/*.patch; do
  [ -e "$p" ] || continue
  if git -C "$MLT" apply --check "$p" 2>/dev/null; then
    git -C "$MLT" apply "$p"; echo "patched: $(basename "$p")"
  elif git -C "$MLT" apply --reverse --check "$p" 2>/dev/null; then
    echo "patched (already applied): $(basename "$p")"
  else
    echo "ERROR: cannot apply $(basename "$p") to $MLT" >&2; exit 1
  fi
done

OUT=${OUT:-./deps-build}; mkdir -p "$OUT/mbedtls" "$OUT/obj" "$OUT/gs" "$OUT/shim"
cat > "$OUT/shim/emscripten.h" <<'EOF'
#include <stdio.h>
#define EM_LOG_CONSOLE 1
#define emscripten_log(flags, ...) fprintf(stderr, __VA_ARGS__)
EOF
ARCH="-mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp"
BASE="--sysroot=$DEPS/sysroot $ARCH -O2 -fPIC"

echo "== mbedTLS =="
for f in "$BPD"/mbedtls/library/*.c; do
  "$GCC" $BASE -I"$BPD/mbedtls/include" -c "$f" -o "$OUT/mbedtls/$(basename "$f" .c).o"
done
"$AR" rcs "$OUT/libmbedcrypto_tv.a" "$OUT"/mbedtls/*.o

echo "== libopus (decoder, fixed point) =="
# Same source set and defines as the fork's CMake target; the static archive lets the linker pull in only the decoder.
mkdir -p "$OUT/opus"
O=$MLT/opus
. "$HERE/mlclient/opus-sources.sh"
OPUS_SRC=$(opus_sources "$O")
for f in $OPUS_SRC; do
  "$GCC" $BASE -std=gnu99 -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF -DFIXED_POINT -DOPUS_BUILD \
    -I"$O/include" -I"$O/src" -I"$O/silk" -I"$O/celt" -I"$O/silk/fixed" -c "$O/$f" -o "$OUT/opus/$(echo "$f" | tr / _ | sed 's/\.c$//').o"
done
"$AR" rcs "$OUT/libopus_tv.a" "$OUT"/opus/*.o

echo "== moonlight-common-c (core + PlatformCrypto) =="
# LC_LOG: Limelog goes to the logMessage callback (the engine counts it in the ML-ERROR line),
# not only to the host's stderr
CFLAGS="$BASE -std=gnu11 -DHAS_SOCKLEN_T -DNDEBUG -DUSE_MBEDTLS -DLC_LOG"
CINC="-I$C/src -I$C/enet/include -I$C/reedsolomon -I$OUT/shim -I$BPD/mbedtls/include"
for f in "$C"/src/*.c "$C"/enet/*.c "$C"/reedsolomon/rs.c; do
  b=$(basename "$f" .c); case "$b" in win32|ConnectionTester) continue;; esac
  "$GCC" $CFLAGS $CINC -c "$f" -o "$OUT/obj/$b.o"
done

echo "== libgamestream =="
GINC="-I$MLT/libgamestream -I$C/src -I$BPD/ossl-inc -I$BPD/curl759/include"
for f in "$MLT"/libgamestream/http.c "$MLT"/libgamestream/pairing.c "$MLT"/libgamestream/mkcert.c; do
  "$GCC" $BASE -std=gnu11 -DHAS_SOCKLEN_T -DNDEBUG $GINC -c "$f" -o "$OUT/gs/$(basename "$f" .c).o"
done

echo "== link test against TV libs =="
cat > "$OUT/linktest.c" <<'EOF'
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <mbedtls/entropy.h>
X509 *g_Cert; EVP_PKEY *g_PrivateKey; char *g_UniqueId; char *g_CertHex;
extern int LiStartConnection(void*,void*,void*,void*,void*,void*,int,void*,int);
extern int gs_pair(void*,char*);
int main(void){ curl_global_init(CURL_GLOBAL_DEFAULT); SSL_library_init();
  mbedtls_entropy_context e; mbedtls_entropy_init(&e);
  return (EVP_sha256()!=0)&&((void*)LiStartConnection!=0)&&((void*)gs_pair!=0); }
EOF
"$GCC" $BASE -I"$BPD/ossl-inc" -I"$BPD/curl759/include" -I"$BPD/mbedtls/include" -c "$OUT/linktest.c" -o "$OUT/linktest.o"
"$GCC" $BASE -o "$OUT/linktest" "$OUT/linktest.o" "$OUT"/obj/*.o "$OUT"/gs/*.o "$OUT/libmbedcrypto_tv.a" \
  -L"$TVL" -Wl,-rpath-link,"$TVL" -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -l:libcurl.so.4 -lpthread
echo "linktest built:"; file "$OUT/linktest" | cut -d, -f1-3
bad=$("$OBJD" -T "$OUT/linktest" | grep -oE 'GLIBC_2\.(2[5-9]|[3-9][0-9])' | sort -u || true)
[ -z "$bad" ] && echo "glibc symbols: all <= 2.24 OK" || { echo "TOO-NEW glibc: $bad"; exit 1; }
