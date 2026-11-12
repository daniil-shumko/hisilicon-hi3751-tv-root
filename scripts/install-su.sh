#!/usr/bin/env bash
# install-su.sh -- build ktvsud + su, install them to /system, autostart at boot,
#                 and verify root from a NON-root context.
#
# usage: ./scripts/install-su.sh <tv-ip> [--force]
#
# Why a daemon instead of a setuid binary: see docs/05-app-level-root.md
set -uo pipefail

IP="${1:-}"
FORCE="${2:-}"
[[ -z "$IP" ]] && { echo "usage: $0 <tv-ip> [--force]" >&2; exit 2; }

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src/su"
ADB="${ADB:-adb}"
SERIAL="$IP:5555"
sh_() { "$ADB" -s "$SERIAL" shell "$1" 2>&1 | tr -d '\r'; }

# ---- connect -------------------------------------------------------------
"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
sleep 1
[[ "$("$ADB" -s "$SERIAL" get-state 2>/dev/null)" == "device" ]] || {
  echo "!! cannot reach $SERIAL -- enable USB debugging first (docs/03-enable-adb.md)" >&2
  exit 1; }

dev=$(sh_ 'getprop ro.product.device')
abi=$(sh_ 'getprop ro.product.cpu.abilist')
enforce=$(sh_ 'getenforce')

if [[ "$dev" != "Hi3751V560" && "$FORCE" != "--force" ]]; then
  echo "!! device is '$dev', not Hi3751V560. Re-run with --force if you know what you are doing." >&2
  exit 1
fi
if [[ "$abi" != *armeabi-v7a* ]]; then
  echo "!! device has no armeabi-v7a; rebuild src/su for '$abi'" >&2
  exit 1
fi
if [[ "$enforce" != "Permissive" && "$FORCE" != "--force" ]]; then
  echo "!! SELinux is enforcing ($enforce). The daemon needs a su domain transition." >&2
  echo "   This is documented as unsupported. Re-run with --force to try anyway." >&2
  exit 1
fi
echo "[*] device ok: $dev / $enforce"

# ---- build ---------------------------------------------------------------
echo "[*] building for armv7a-linux-androideabi21"
make -s -C "$SRC" >/dev/null || { echo "!! build failed; run 'make -C $SRC' to see why" >&2; exit 1; }
SU="$SRC/out/su"
DAEMON="$SRC/out/ktvsud"
RC="$SRC/ktvsu.rc"
[[ -f "$SU" && -f "$DAEMON" ]] || { echo "!! build produced nothing" >&2; exit 1; }

# ---- root ----------------------------------------------------------------
uid=$(sh_ 'id -u')
if [[ "$uid" != "0" ]]; then
  echo "[*] escalating adbd"
  "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
  sleep 2
  "$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
  sleep 1
  uid=$(sh_ 'id -u')
fi
if [[ "$uid" != "0" ]]; then
  echo "!! adb root failed. See docs/04-root.md" >&2
  exit 1
fi
echo "[*] root uid 0"

sh_ 'mount -o rw,remount /' >/dev/null 2>&1 || true
if [[ "$(sh_ "touch /system/.wtest && rm /system/.wtest && echo yes")" != "yes" ]]; then
  echo "!! /system is not writable" >&2
  exit 1
fi

# ---- install -------------------------------------------------------------
echo "[*] pushing"
"$ADB" -s "$SERIAL" push "$SU" /data/local/tmp/ktv-su >/dev/null
"$ADB" -s "$SERIAL" push "$DAEMON" /data/local/tmp/ktvsud >/dev/null
"$ADB" -s "$SERIAL" push "$RC" /data/local/tmp/ktvsu.rc >/dev/null

echo "[*] installing to /system/bin"
sh_ 'mount -o rw,remount /' >/dev/null 2>&1 || true
# rename rather than overwrite: a running daemon makes the target ETXTBSY
sh_ 'cp /data/local/tmp/ktv-su /system/bin/su.new && \
     cp /data/local/tmp/ktvsud /system/bin/ktvsud.new && \
     chown root:root /system/bin/su.new /system/bin/ktvsud.new && \
     chmod 755 /system/bin/su.new /system/bin/ktvsud.new && \
     chmod u-s /system/bin/su.new && \
     mv -f /system/bin/su.new /system/bin/su && \
     mv -f /system/bin/ktvsud.new /system/bin/ktvsud' >/dev/null

echo "[*] writing /system/etc/init/ktvsu.rc"
sh_ 'cp /data/local/tmp/ktvsu.rc /system/etc/init/ktvsu.rc && \
     chown root:root /system/etc/init/ktvsu.rc && \
     chmod 644 /system/etc/init/ktvsu.rc' >/dev/null
sh_ 'sync'

echo "[*] restarting daemon"
old=$(sh_ "pidof ktvsud" | tr -d '\r')
if [[ -n "$old" ]]; then
  sh_ "kill $old" >/dev/null 2>&1 || true
  sleep 1
fi
sh_ 'rm -f /dev/socket/ktvsu' >/dev/null 2>&1 || true
sh_ 'nohup /system/bin/ktvsud >/dev/null 2>&1 & sleep 1' >/dev/null

# ---- verify from a NON-root context -------------------------------------
pid=$(sh_ "ps -A -o NAME | grep -c ktvsud")
echo "[*] ktvsud processes: $pid"

# drop adbd back to shell so the verification is honest
"$ADB" -s "$SERIAL" unroot >/dev/null 2>&1 || true
sleep 2
"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
sleep 1
who=$(sh_ 'id -u')
got=$(sh_ 'su -c "id -u"')
echo "[*] unprivileged uid $who -> su -c 'id -u' = $got"

if [[ "$got" == "0" ]]; then
  cat <<EOF
[ok] su works from an unprivileged context.

    /system/bin/su        client
    /system/bin/ktvsud    root daemon
    /system/etc/init/ktvsu.rc   autostart

    After a reboot confirm with:
      adb shell getprop init.svc.ktvsu    # running
      adb shell su -c 'id -u'             # 0
EOF
  exit 0
fi

echo "!! su did not escalate. Check:" >&2
echo "     adb shell ps -A -o USER,PID,NAME | grep ktvsud" >&2
echo "     adb shell su -c id" >&2
echo "   See docs/07-troubleshooting.md" >&2
exit 1
