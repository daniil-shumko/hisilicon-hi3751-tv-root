#!/usr/bin/env bash
# install-superuser.sh -- install ktvsud v2 + the KTV Superuser app, so root
#                         requests from apps show an on-screen prompt.
#
# usage: ./scripts/install-superuser.sh <tv-ip>
#
# Requires: adb root (see root.sh) and a built APK toolchain (see src/manager).
set -uo pipefail

IP="${1:-}"
[[ -z "$IP" ]] && { echo "usage: $0 <tv-ip>" >&2; exit 2; }

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
ADB="${ADB:-adb}"
SERIAL="$IP:5555"
sh_() { "$ADB" -s "$SERIAL" shell "$1" 2>&1 | tr -d '\r'; }

# ---- connect -------------------------------------------------------------
"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
sleep 1
[[ "$("$ADB" -s "$SERIAL" get-state 2>/dev/null)" == "device" ]] || {
  echo "!! cannot reach $SERIAL" >&2; exit 1; }

if [[ "$(sh_ 'id -u')" != "0" ]]; then
  echo "[*] adb root"
  "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
  sleep 2
  "$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
  sleep 1
fi
[[ "$(sh_ 'id -u')" == "0" ]] || { echo "!! need root; run ./scripts/root.sh $IP first" >&2; exit 1; }

# ---- build ---------------------------------------------------------------
echo "[*] building ktvsud + su"
make -s -C "$ROOT/src/su" >/dev/null || { echo "!! daemon build failed" >&2; exit 1; }

echo "[*] building the superuser app"
"$ROOT/src/manager/build.sh" >/dev/null 2>&1 || { echo "!! apk build failed" >&2; exit 1; }
APK="$ROOT/src/manager/out/KTVSuperuser.apk"
[[ -f "$APK" ]] || { echo "!! $APK missing" >&2; exit 1; }

# ---- install the app -----------------------------------------------------
echo "[*] installing the app"
"$ADB" -s "$SERIAL" install -r "$APK" 2>&1 | grep -E "Success|Failure" | sed 's/^/    /'
UID_APP=$(sh_ "pm list packages -U | grep lan.ktvsu.manager" | sed -n 's/.*uid:\([0-9]*\).*/\1/p')
[[ -n "$UID_APP" ]] || { echo "!! cannot determine the app uid" >&2; exit 1; }
echo "[*] superuser app uid: $UID_APP"

# ---- install the binaries ------------------------------------------------
echo "[*] installing ktvsud + su to /system/bin"
sh_ 'mount -o rw,remount /' >/dev/null 2>&1 || true
"$ADB" -s "$SERIAL" push "$ROOT/src/su/out/ktvsud" /data/local/tmp/ktvsud >/dev/null
"$ADB" -s "$SERIAL" push "$ROOT/src/su/out/su" /data/local/tmp/su >/dev/null

# rename, not overwrite: the daemon is executing the file (ETXTBSY)
sh_ 'cp /data/local/tmp/ktvsud /system/bin/ktvsud.new && \
     chown root:root /system/bin/ktvsud.new && chmod 755 /system/bin/ktvsud.new && \
     mv -f /system/bin/ktvsud.new /system/bin/ktvsud && \
     cp /data/local/tmp/su /system/bin/su.new && \
     chown root:root /system/bin/su.new && chmod 755 /system/bin/su.new && \
     mv -f /system/bin/su.new /system/bin/su' >/dev/null

# ---- state ---------------------------------------------------------------
echo "[*] preparing /data/misc/ktvsu"
sh_ 'mkdir -p /data/misc/ktvsu/pending /data/misc/ktvsu/answer && \
     chmod 700 /data/misc/ktvsu && chmod 700 /data/misc/ktvsu/pending /data/misc/ktvsu/answer'

if [[ "$(sh_ 'cat /data/misc/ktvsu/mode 2>/dev/null')" == "" ]]; then
  sh_ "echo ask > /data/misc/ktvsu/mode && chmod 600 /data/misc/ktvsu/mode"
fi

sh_ "echo $UID_APP > /data/misc/ktvsu/admin_uid && chmod 600 /data/misc/ktvsu/admin_uid"

# seed: adb shell (2000) and root (0) never prompt, otherwise every
# `adb shell su -c` from your own laptop would need a tap on the TV.
if ! sh_ 'grep -q "^2000 " /data/misc/ktvsu/policy 2>/dev/null'; then
  sh_ 'printf "0 allow\n2000 allow\n" >> /data/misc/ktvsu/policy && chmod 600 /data/misc/ktvsu/policy'
fi

# ---- restart the daemon --------------------------------------------------
echo "[*] restarting daemon"
sh_ 'for p in $(pidof ktvsud); do kill -9 $p 2>/dev/null; done; sleep 1' >/dev/null
sh_ 'nohup /system/bin/ktvsud >/data/local/tmp/ktvsud.log 2>&1 & sleep 1' >/dev/null
PID=$(sh_ 'pidof ktvsud')
echo "[*] ktvsud pid: ${PID:-NOT RUNNING}"
[[ -n "$PID" ]] || { echo "!! daemon failed: $(sh_ 'cat /data/local/tmp/ktvsud.log')" >&2; exit 1; }

# ---- let the app raise the prompt over other apps -----------------------
echo "[*] granting SYSTEM_ALERT_WINDOW (optional; the daemon raises the prompt itself)"
sh_ 'appops set lan.ktvsu.manager SYSTEM_ALERT_WINDOW allow' >/dev/null 2>&1 || true
sh_ 'am force-stop lan.ktvsu.manager' >/dev/null 2>&1 || true
sh_ 'am start -n lan.ktvsu.manager/.MainActivity' >/dev/null 2>&1 || true

# ---- verify --------------------------------------------------------------
sleep 2
echo
echo "[*] verification"
echo "    daemon:        $(sh_ 'pidof ktvsud')"
echo "    sockets:       $(sh_ 'grep -c ktvsu /proc/net/unix') (expect 2)"
echo "    admin uid:     $(sh_ 'cat /data/misc/ktvsu/admin_uid')"
echo "    mode:          $(sh_ 'cat /data/misc/ktvsu/mode')"
echo "    policy:"
sh_ 'cat /data/misc/ktvsu/policy' | sed 's/^/      /'
echo
echo "    shell su:      $(sh_ 'su -c "id -u"')  (0, no prompt, seeded)"
cat <<EOF

[ok] superuser installed.

    Unknown apps now get an on-screen prompt with DENY / ALLOW ONCE / ALLOW ALWAYS.
    Rules can be revoked in the KTV Superuser app.
    Back on the prompt = deny.

    Removal:
      adb shell rm -rf /data/misc/ktvsu
      adb uninstall lan.ktvsu.manager
      adb shell "rm -f /system/etc/init/ktvsu.rc /system/bin/ktvsud /system/bin/su"
EOF
