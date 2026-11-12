#!/usr/bin/env bash
# root.sh -- escalate adbd to uid 0, make /system writable, prove it.
#
# usage: ./scripts/root.sh <tv-ip>
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

before=$(sh_ 'id -u')
echo "[*] shell uid: $before"

echo "[*] adb root"
"$ADB" -s "$SERIAL" root 2>&1 | sed 's/^/    /'
sleep 2
"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
sleep 1

after=$(sh_ 'id -u')
ctx=$(sh_ 'id' | grep -o 'context=[^ ]*' || true)
echo "[*] uid now: $after  $ctx"

if [[ "$after" != "0" ]]; then
  cat >&2 <<'EOS'
!! adb root did not take effect.

   Your build probably refuses it. See docs/04-root.md "If adb root is refused".
   In short: check ro.debuggable, check whether / is already mounted rw, and look
   at exported vendor components (factory/test apps) for a privilege pivot.
EOS
  exit 1
fi

echo "[*] testing /system writability"
sh_ 'mount -o rw,remount /' >/dev/null 2>&1 || true
if [[ "$(sh_ "touch /system/.wtest && rm /system/.wtest && echo yes")" == "yes" ]]; then
  echo "[ok] /system is writable"
else
  echo "[warn] /system is NOT writable; check 'mount | grep \" / \"'" >&2
fi

echo "[ok] root"
echo
echo "next: ./scripts/install-su.sh $IP"
