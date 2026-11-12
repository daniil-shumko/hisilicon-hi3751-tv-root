#!/usr/bin/env bash
# Build the KTV Superuser apk (no gradle).
#
# Requires an Android SDK with build-tools and platform android-34.
#   ./src/manager/build.sh          -> src/manager/out/KTVSuperuser.apk
set -euo pipefail

SDK="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
BT="$(ls -d "$SDK"/build-tools/*/ | sort -V | tail -1)"; BT="${BT%/}"
AJ="$SDK/platforms/android-34/android.jar"
[ -f "$AJ" ] || AJ="$(ls -d "$SDK"/platforms/android-*/ | sort -V | tail -1)android.jar"

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$OUT"

echo "[*] aapt: base apk"
"$BT/aapt" package -f -M "$HERE/AndroidManifest.xml" -I "$AJ" -F "$WORK/base.apk"

echo "[*] javac"
javac -nowarn -source 11 -target 11 -classpath "$AJ" -d "$WORK/classes" \
      $(find "$HERE/src" -name '*.java') 2>&1 | grep -vE 'bootstrap|obsolete|deprecat' || true
[ -d "$WORK/classes/lan" ] || { echo "!! javac produced nothing" >&2; exit 1; }

echo "[*] d8"
mkdir -p "$WORK/dex"
"$BT/d8" --lib "$AJ" --min-api 21 --output "$WORK/dex" $(find "$WORK/classes" -name '*.class')

cp "$WORK/base.apk" "$WORK/u.apk"
(cd "$WORK/dex" && zip -q -j "$WORK/u.apk" classes.dex)
"$BT/zipalign" -f 4 "$WORK/u.apk" "$WORK/a.apk"

KEYSTORE="$HERE/ktvsu.keystore"
if [ ! -f "$KEYSTORE" ]; then
  echo "[*] generating keystore"
  keytool -genkeypair -keystore "$KEYSTORE" -alias ktvsu -keyalg RSA -keysize 2048 \
    -validity 10000 -storepass ktvsu123 -keypass ktvsu123 \
    -dname "CN=KTVSuperuser, O=local, C=XX" >/dev/null 2>&1
fi

"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:ktvsu123 --key-pass pass:ktvsu123 \
  --out "$OUT/KTVSuperuser.apk" "$WORK/a.apk" 2>/dev/null
"$BT/apksigner" verify --print-certs "$OUT/KTVSuperuser.apk" 2>/dev/null | head -2
ls -la "$OUT/KTVSuperuser.apk"
echo "[ok] $OUT/KTVSuperuser.apk"
