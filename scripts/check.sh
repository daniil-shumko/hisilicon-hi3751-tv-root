#!/usr/bin/env bash
# check.sh -- tell me whether this TV is the expected device and whether
#             the documented conditions for rooting hold.
#
# usage: ./scripts/check.sh <tv-ip>
set -uo pipefail

IP="${1:-}"
if [[ -z "$IP" ]]; then
  echo "usage: $0 <tv-ip>" >&2
  exit 2
fi

ADB="${ADB:-adb}"
command -v "$ADB" >/dev/null 2>&1 || { echo "adb not found in PATH" >&2; exit 1; }

SERIAL="$IP:5555"
"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
sleep 1

state=$("$ADB" -s "$SERIAL" get-state 2>/dev/null || true)
if [[ "$state" != "device" ]]; then
  echo "!! cannot reach $SERIAL (state: ${state:-none})"
  echo "   is 'USB debugging' enabled in Developer options?"
  exit 1
fi

g() { "$ADB" -s "$SERIAL" shell getprop "$1" 2>/dev/null | tr -d '\r'; }

fp=$(g ro.build.fingerprint)
dev=$(g ro.product.device)
plat=$(g ro.board.platform)
btype=$(g ro.build.type)
secure=$(g ro.secure)
debuggable=$(g ro.debuggable)
adbsecure=$(g ro.adb.secure)
tcp=$(g persist.adb.tcp.port)
abi=$(g ro.product.cpu.abilist)
enforce=$("$ADB" -s "$SERIAL" shell getenforce 2>/dev/null | tr -d '\r')

echo "-----------------------------------------------------------------"
printf '%-26s %s\n' "build.fingerprint" "$fp"
printf '%-26s %s\n' "product.device" "$dev"
printf '%-26s %s\n' "board.platform" "$plat"
printf '%-26s %s\n' "build.type" "$btype"
printf '%-26s %s\n' "ro.secure" "$secure"
printf '%-26s %s\n' "ro.debuggable" "$debuggable"
printf '%-26s %s\n' "ro.adb.secure" "${adbsecure:-(unset)}"
printf '%-26s %s\n' "persist.adb.tcp.port" "$tcp"
printf '%-26s %s\n' "abilist" "$abi"
printf '%-26s %s\n' "getenforce" "$enforce"
echo "-----------------------------------------------------------------"

verdict=0

if [[ "$dev" == "Hi3751V560" && "$fp" == *"HiDPT/Hi3751V560_DMOH"* ]]; then
  echo "[ok]   device matches the reference Hi3751V560"
else
  echo "[warn] device does not match the reference unit"
  echo "       vendor package names in docs/06-debloat.md will not apply"
  verdict=1
fi

if [[ -z "$adbsecure" ]]; then
  echo "[ok]   ro.adb.secure is unset -> ADB has NO key authentication"
else
  echo "[warn] ro.adb.secure=$adbsecure -> you will see an RSA 'Allow debugging' prompt"
  verdict=1
fi

if [[ "$enforce" == "Permissive" ]]; then
  echo "[ok]   SELinux is Permissive"
else
  echo "[warn] SELinux is enforcing -> the su daemon needs policy work this repo does not cover"
  verdict=1
fi

if [[ "$abi" == *"armeabi-v7a"* ]]; then
  echo "[ok]   32-bit ARM present (binaries here target armeabi-v7a)"
else
  echo "[warn] no armeabi-v7a in abilist; rebuild src/su for your ABI"
  verdict=1
fi

echo
echo "next: ./scripts/root.sh $IP      (escalate adbd and check /system writability)"
exit $verdict
