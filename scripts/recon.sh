#!/usr/bin/env bash
# recon.sh -- collect the device profile described in docs/01-device-profile.md
#
# usage: ./scripts/recon.sh <tv-ip> [--root]
#
# Writes ./recon-<ip>.txt and prints a summary. --root runs adb root first,
# which unlocks the process list, socket owners and /vendor/bin.
set -uo pipefail

IP="${1:-}"
ROOT="${2:-}"
if [[ -z "$IP" ]]; then
  echo "usage: $0 <tv-ip> [--root]" >&2
  exit 2
fi

ADB="${ADB:-adb}"
SERIAL="$IP:5555"
OUT="recon-${IP}.txt"

"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
sleep 1
[[ "$("$ADB" -s "$SERIAL" get-state 2>/dev/null)" == "device" ]] || {
  echo "!! cannot reach $SERIAL" >&2
  exit 1
}

if [[ "$ROOT" == "--root" ]]; then
  "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
  sleep 1
fi

sh_() { "$ADB" -s "$SERIAL" shell "$1" 2>&1 | tr -d '\r'; }

{
  echo "### collected $(date -u +%FT%TZ) from $SERIAL"
  for p in ro.build.fingerprint ro.build.description ro.build.type ro.build.version.sdk \
           ro.build.version.security_patch ro.product.model ro.product.device \
           ro.board.platform ro.hardware ro.product.cpu.abilist ro.zygote \
           ro.secure ro.debuggable ro.adb.secure persist.adb.tcp.port \
           service.adb.tcp.port ro.boot.slot_suffix; do
    echo "$p=$(sh_ "getprop $p")"
  done
  echo "getenforce=$(sh_ getenforce)"
  echo "wm=$(sh_ 'wm size' | tr '\n' ' ')"
  echo "mem=$(sh_ 'free -m' | sed -n 2p)"
  echo "df=$(sh_ 'df -h /data' | tail -1)"
  echo
  echo "### /proc/cpuinfo"
  sh_ "grep -E 'model name|Hardware|Revision' /proc/cpuinfo" | head -8
  echo
  echo "### mounts"
  sh_ "mount | grep -E ' / | /vendor | /data | /cache '"
  echo
  echo "### partitions"
  sh_ "ls /dev/block/by-name" | tr '\n' ' '
  echo
  echo
  echo "### vendor packages"
  sh_ "pm list packages -f -s | sed 's/package://' | grep -viE 'com.android|androidx'"
  echo
  echo "### user packages"
  sh_ "pm list packages -3 | sed 's/package://'"
  if [[ "$ROOT" == "--root" ]]; then
    echo
    echo "### listeners"
    sh_ "su -c 'ss -tnlp'"
    echo
    echo "### root processes"
    sh_ "su -c \"ps -A -o USER,PID,PPID,ARGS | grep '^root' | grep -v '\\['\""
  fi
} > "$OUT" 2>&1

echo "[ok] wrote $OUT ($(wc -l < "$OUT" | tr -d ' ') lines)"
grep -E '^(ro.build.fingerprint|ro.product.device|ro.secure|ro.debuggable|ro.adb.secure|getenforce)=' "$OUT"
