#!/usr/bin/env bash
# uninstall-su.sh -- remove the ktvsud daemon, the su client and the init rc.
#                    adb root is unaffected.
#
# usage: ./scripts/uninstall-su.sh <tv-ip>
set -uo pipefail

IP="${1:-}"
[[ -z "$IP" ]] && { echo "usage: $0 <tv-ip>" >&2; exit 2; }

ADB="${ADB:-adb}"
SERIAL="$IP:5555"
sh_() { "$ADB" -s "$SERIAL" shell "$1" 2>&1 | tr -d '\r'; }

"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
sleep 1
[[ "$("$ADB" -s "$SERIAL" get-state 2>/dev/null)" == "device" ]] || {
  echo "!! cannot reach $SERIAL" >&2; exit 1; }

if [[ "$(sh_ 'id -u')" != "0" ]]; then
  "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
  sleep 2
  "$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
  sleep 1
fi

pid=$(sh_ "pidof ktvsud" | tr -d '\r')
sh_ 'mount -o rw,remount /' >/dev/null 2>&1 || true

echo "[*] removing /system/etc/init/ktvsu.rc"
sh_ 'rm -f /system/etc/init/ktvsu.rc'

echo "[*] removing /system/bin/su and /system/bin/ktvsud"
sh_ 'rm -f /system/bin/su /system/bin/ktvsud /system/xbin/su'

echo "[*] stopping daemon (if running)"
if [[ -n "$pid" ]]; then
  sh_ "kill $pid" >/dev/null 2>&1 || true
fi
sh_ 'sync'

echo "[ok] removed. adb root still works; app-level su is gone after reboot."
echo "     (already-running ktvsud instances keep the socket until the process dies)"
