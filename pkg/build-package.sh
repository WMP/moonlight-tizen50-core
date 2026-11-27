#!/bin/sh
# Build the Moonlight Tizen 5.0 web UI package (.wgt): the moonlight-tizen
# front-end with its transport swapped to the Tizen message port (mlbridge.js).
# The native streaming engine ships separately as the .NET host .tpk (build-tpk.sh).
# Run inside the Docker image (pkg/Dockerfile) for a reproducible toolchain.
#
#   REPO           repo root (this checkout)
#   MLT            a moonlight-tizen checkout (web UI source)
#   TIZEN_PROFILE  signing profiles.xml path (optional; else a dev cert is generated)
#   SIGN_PROFILE   profile name in profiles.xml (default: Moonlight)
set -e
: "${REPO:?}"; : "${MLT:?}"
HERE="$REPO/pkg"
BUILD_VARIANT=${BUILD_VARIANT:-release}
case "$BUILD_VARIANT" in debug|release) ;; *) echo "BUILD_VARIANT must be debug or release" >&2; exit 1 ;; esac
OUT=${OUT:-$HERE/out}; rm -rf "$OUT"; mkdir -p "$OUT/ui"

echo "== phase 1: web UI (moonlight-tizen front-end + message-port transport) =="
# Ship the web front-end, NOT the WASM module.
cp "$MLT/wasm/index.html" "$OUT/ui/index.html"
cp -r "$MLT/wasm/platform" "$OUT/ui/platform"
cp -r "$MLT/wasm/static" "$OUT/ui/static" 2>/dev/null || true
[ -d "$MLT/wasm/res" ] && cp -r "$MLT/wasm/res" "$OUT/ui/res" || true
# platform.js = TV remote-key handler (platformOnLoad/tvKey, used by index.js); not WASM.
cp "$MLT/res/platform.js" "$OUT/ui/platform.js"
cp "$HERE/ui/mlbridge.js" "$OUT/ui/platform/mlbridge.js"
cp "$HERE/ui/mlaudio.js" "$OUT/ui/platform/mlaudio.js"
cp "$HERE/ui/config.xml" "$OUT/ui/config.xml"
cp "$HERE/ui/icon.png" "$OUT/ui/icon.png" 2>/dev/null || true
# Inject the message-port shim after messages.js (it replaces the WASM transport).
sed -i 's#<script type="text/javascript" src="platform/messages.js"></script>#&\n  <script type="text/javascript" src="platform/mlaudio.js"></script>\n  <script type="text/javascript" src="platform/mlbridge.js"></script>#' "$OUT/ui/index.html"
grep -q platform/mlaudio.js "$OUT/ui/index.html" || { echo "mlaudio.js not injected into index.html" >&2; exit 1; }
# The TV encodes the negotiated six-channel Opus stream as Dolby Digital.
sed -i '/data-value="51Surround"/a\                <li class="mdl-menu__item" data-value="51Ac3">5.1 Surround (Dolby Digital / AC-3)</li>' "$OUT/ui/index.html"
grep -q 'data-value="51Ac3"' "$OUT/ui/index.html" || { echo "AC-3 audio menu not found" >&2; exit 1; }
# Version + build id shown in Settings > About ("Moonlight v<ver> (<ML_BUILD>)").
#   APP_VERSION   x.y.z for config.xml (CI: the release tag); unset keeps config.xml's
#   BUILD_COMMIT  this repo's commit (CI passes it; git may refuse the mount in a container)
BUILD_COMMIT=${BUILD_COMMIT:-$(git -C "$REPO" -c safe.directory='*' rev-parse --short HEAD 2>/dev/null || echo unknown)}
UI_BASE=$(sed -n 's/.*<widget[^>]* version="\([^"]*\)".*/\1/p' "$MLT/res/config.xml" | head -1)
ML_BUILD="tizen50-core $BUILD_VARIANT $BUILD_COMMIT${UI_BASE:+, UI $UI_BASE}"
if [ -n "$APP_VERSION" ]; then
  echo "$APP_VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "APP_VERSION must be x.y.z, got: $APP_VERSION" >&2; exit 1; }
  sed -i "/<widget/,/>/ s/ version=\"[^\"]*\"/ version=\"$APP_VERSION\"/" "$OUT/ui/config.xml"   # <widget> spans lines
fi
sed -i "s/__ML_BUILD__/$ML_BUILD/" "$OUT/ui/platform/mlbridge.js"
# index.js placeholders (moonlight-tizen's own Dockerfile does the same).
if [ "$BUILD_VARIANT" = debug ]; then BT=development; else BT=release; fi
sed -i -e "s/__BUILD_TYPE__/$BT/g" -e "s/__BUILD_COMMIT__/$BUILD_COMMIT/g" "$OUT/ui/platform/index.js"
# Game Mode switch: upstream disables it on Tizen 5.5 because the WASM player's
# latency mode is unusable there. This engine drives the TV's own game picture
# mode instead (mlbridge.js startStream `gameMode`), so keep the switch usable.
# Both branches: the defaults reset (switch off+disabled) and the stored-state
# load (off+disabled); the ForceGM-variant check stays - that variant is signed
# for a forced mode and must not double-apply.
sed -i -e 's/} else if (checkForceGMVariant || parseFloat(platformVer) === 5.5) {/} else if (checkForceGMVariant) {/' \
       -e 's/} else if (isForceGMVariant || parseFloat(platformVer) === 5.5) {/} else if (isForceGMVariant) {/' \
       "$OUT/ui/platform/index.js"
grep -q "isForceGMVariant || parseFloat" "$OUT/ui/platform/index.js" && { echo "gameMode 5.5 unlock sed failed" >&2; exit 1; }
# Remote navigation: add the game mode off button (made by mlbridge.js) after the game mode switch.
sed -i "s/^      'gameModeBtn'\$/      'gameModeBtn',\n      'gameModeOffBtn'/" "$OUT/ui/platform/navigation.js"
grep -q "'gameModeOffBtn'" "$OUT/ui/platform/navigation.js" || { echo "gameModeOffBtn navigation sed failed" >&2; exit 1; }
echo "version: $(sed -n '/<widget/,/>/ s/.* version="\([^"]*\)".*/\1/p' "$OUT/ui/config.xml" | head -1) ($ML_BUILD)"
if [ "$BUILD_VARIANT" = debug ]; then
  # trace build: console -> host -> dlog (see ui/mltrace.js); first script in <head>
  cp "$HERE/ui/mltrace.js" "$OUT/ui/platform/mltrace.js"
  sed -i 's#<head\( [^>]*\)\{0,1\}>#&\n  <script type="text/javascript" src="platform/mltrace.js"></script>#I' "$OUT/ui/index.html"
  grep -q mltrace.js "$OUT/ui/index.html" || { echo "mltrace.js not injected into index.html" >&2; exit 1; }
fi
echo "ui assembled -> $OUT/ui"

echo "== phase 2: package + sign =="
# tizen CLI is on PATH inside the image; allow override for host runs.
TZ=$(command -v tizen || echo "${TIZEN_CLI:+$TIZEN_CLI/tizen}")
if [ -n "$TZ" ] && [ -x "$TZ" ]; then
  P=${SIGN_PROFILE:-Moonlight}
  PWD_=${CERT_PASS:-tizenpkpass}
  if [ -n "$TIZEN_PROFILE" ]; then
    "$TZ" cli-config "default.profiles.path=$TIZEN_PROFILE/profile/profiles.xml" >/dev/null 2>&1 || true
  elif ! "$TZ" security-profiles list 2>/dev/null | grep -q "^$P\b"; then
    echo "generating dev signing profile '$P' in-container"
    mkdir -p "$OUT/certs"
    "$TZ" certificate -a "$P" -p "$PWD_" -c PL -n Moonlight -- "$OUT/certs"
    "$TZ" security-profiles add -n "$P" -a "$OUT/certs/author.p12" -p "$PWD_"
  fi
  "$TZ" build-web -- "$OUT/ui" || true
  "$TZ" package -t wgt -s "$P" -- "$OUT/ui"
  echo "package(s):"; ls "$OUT/ui"/*.wgt 2>/dev/null
else
  echo "tizen CLI not found - run inside the Docker image (pkg/Dockerfile)."
fi
