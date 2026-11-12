#!/usr/bin/env bash
# debloat.sh -- disable the vendor adware and the OTA updater.
#
# usage: ./scripts/debloat.sh <tv-ip>            disable
#        ./scripts/debloat.sh <tv-ip> --undo     re-enable everything
#
# Uses `pm disable-user`, so nothing is deleted. See docs/06-debloat.md
set -uo pipefail

IP="${1:-}"
MODE="${2:-}"
[[ -z "$IP" ]] && { echo "usage: $0 <tv-ip> [--undo]" >&2; exit 2; }

ADB="${ADB:-adb}"
SERIAL="$IP:5555"
sh_() { "$ADB" -s "$SERIAL" shell "$1" 2>&1 | tr -d '\r'; }

# package : why
PKGS=(
  "com.sraf.launcher.ext|SadApp - persistent system app, ad injection, watches foreground app"
  "com.zeasn.deviceportal.asdprovider.cetus.pub|device portal advertising provider"
  "com.tencent.qqlivei18n|WETV preinstalled streaming app"
  "com.ctv.hotelsetting|hotel / kiosk mode"
  "com.ctv.autotest.helper|factory auto-test broadcast helper"
  "com.cultraview.cusfactorytest|factory test harness"
  "com.cultraview.osupdate|OTA updater - disabled to protect root"
)

"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
sleep 1
[[ "$("$ADB" -s "$SERIAL" get-state 2>/dev/null)" == "device" ]] || {
  echo "!! cannot reach $SERIAL" >&2; exit 1; }

if [[ "$MODE" == "--undo" ]]; then
  echo "== re-enabling"
  for entry in "${PKGS[@]}"; do
    pkg="${entry%%|*}"
    out=$(sh_ "pm enable $pkg")
    case "$out" in
      *"new state: enabled"*|*"enabled"*) echo "  [ok]   $pkg" ;;
      *) echo "  [skip] $pkg (${out:-no such package})" ;;
    esac
  done
  echo
  echo "Remember: the TV works fine with everything enabled; you just lose the RAM back."
  exit 0
fi

echo "== free memory before"
sh_ 'free -m' | sed -n 2p | sed 's/^/  /'

echo "== disabling"
for entry in "${PKGS[@]}"; do
  pkg="${entry%%|*}"
  why="${entry#*|}"
  if ! sh_ "pm path $pkg" | grep -q package:; then
    echo "  [skip] $pkg -- not installed"
    continue
  fi
  out=$(sh_ "pm disable-user --user 0 $pkg")
  case "$out" in
    *disabled-user*) echo "  [ok]   $pkg  ($why)" ;;
    *) echo "  [warn] $pkg -> $out" ;;
  esac
done

echo
echo "== disabled packages now"
sh_ 'pm list packages -d' | sed 's/package:/  /'
echo
echo "note: PERSISTENT apps keep their memory until reboot. Power-cycle the TV to"
echo "      reclaim it, then re-check 'free -m'."
echo
echo "undo with: $0 $IP --undo"
