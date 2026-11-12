# 07 — Troubleshooting, including every wrong turn taken

This chapter exists because most of the time spent on this device went into things that
looked broken and were not. If you are stuck, read the matching entry.

## `su` works, then fails, then works again

**Symptom.** From an app sandbox, `su -c id` succeeds, then fails with
`/system/bin/sh: /system/bin/su: inaccessible or not found`, then succeeds again. Perfectly
alternating, 50% failure rate.

**Cause.** Not the daemon, not SELinux, not the build. There were **two devices polling the
same command channel** — the TV and a phone used for testing, both running the same helper
app. Commands alternated between them. The phone had no `/system/bin/su`, so every other
command died there.

**Fix.** Remove the helper from the second device, or namespace your command channel per
device. Confirm by printing the executing UID on every call:

```
id -u           -> 10077    (TV)
id -u           -> 10462    (phone -- this is the bug)
```

**Lesson.** An alternating failure is almost never a race. It is two of something.

## An app says it cannot obtain root

Start with the call log. It answers "did the app even ask, and with what?":

```bash
adb root
adb shell cat /data/misc/ktvsu/calls.log
```

Each line is `time uid=<uid> <command|(interactive shell)> :: <verdict>`.

Three causes, in the order they are worth checking:

1. **No entry for that uid at all.** The app never invoked `su` — it is looking for a root
   solution by other means (a Magisk daemon, a `su` in a path you do not have), or it caches a
   previous failure. See [09-superuser.md](09-superuser.md#2-magisk-flag-compatibility) for
   the flags apps send.

2. **`:: deny (rejected/timeout)`.** You missed the prompt, or the request took longer than
   the 120 s window. Approve it in the KTV Superuser app, or pre-seed the rule.

3. **`:: allow (...)` and the app still fails.** The shell is not behaving like a terminal.
   Browsers, file managers and terminals want a **pty** and a prompt; a pipe that only
   collects output looks like failure. The reference fix was `forkpty()` in the daemon
   ([09-superuser.md](09-superuser.md#1-a-pty-not-a-pipe)).

If you want to see exactly what an app is doing, watch the log while it tries:

```bash
adb shell "tail -f /data/misc/ktvsu/calls.log"
```

## `setuid` binary says "Operation not permitted"

Expected, and not fixable. See [05-app-level-root.md](05-app-level-root.md). The binary
executes; the setuid bit is discarded by `no_new_privs`. Use the daemon.

## `adb root` says "restarting adbd as root" but `id` still shows shell

Give it a second, and check the transport:

```bash
adb disconnect
adb connect <tv-ip>:5555
adb root
adb shell id
```

If it still shows uid 2000, your build refuses root escalation. Check `ro.debuggable` and
read [04-root.md](04-root.md#if-adb-root-is-refused).

## `adb remount` → "Device must be bootloader unlocked"

Harmless. Use the manual remount instead:

```bash
adb root
adb shell su -c 'mount -o rw,remount /'
adb shell su -c "mount | grep ' / '"
```

The remount is lost on reboot. Remount and write in the same session.

## Port 4321 is open and looks like a service

It is `vendor.hisilicon.hardware.hwtvmw@1.0-service`, the HiSilicon TV middleware HAL. It
is not ADB, it is not a debug port. Full write-up in
[02-recon.md](02-recon.md#port-4321). Move on.

## `adb connect` succeeds but `adb devices` says `offline`

Two known causes here:

1. You pointed adb at **4321** instead of 5555. The socket accepts, the ADB handshake never
   answers, and adb reports `offline`. That port will never work.
2. adbd is not actually running. Check `adb shell getprop init.svc.adbd` — if it is
   `stopped`, USB debugging is off in Developer options.

## The vendor settings app has no "System" or "Developer options"

Correct, and permanent. Developer options only exist in `com.android.settings` (AOSP), not
in `com.cultraview.setting`. See [03-enable-adb.md](03-enable-adb.md#the-confusing-part-up-front).

## Tapping menu labels in "System Info" does nothing

Same root cause. The vendor System Info screen is decorative — its labels are not
clickable. Tap the build number in the *AOSP* About screen instead.

## Wireless debugging is missing from Developer options

Not supported on this build. There is no pairing code flow. Plain TCP ADB on 5555, which is
already enabled by init, is the only path — and it is simpler.

## After enabling USB debugging, still nothing on 5555

```bash
adb shell getprop init.svc.adbd        # running?
adb shell getprop service.adb.tcp.port # 5555?
adb shell ip addr show wlan0 | grep 'inet '  # right IP? DHCP may have moved it
```

The TV is Wi-Fi only on the reference device. If your router hands out a new lease, the old
IP silently blackholes.

## `screencap` returns the same image twice

Known on this hardware. The compositor does not always push a fresh frame. Force a redraw
first:

```bash
adb shell input keyevent KEYCODE_DPAD_DOWN
sleep 1
adb exec-out screencap -p > tv.png
```

## The TV shows an "Advertising Choose" screen after debloating

Expected, harmless, one tap. Press **Not Agree**. Details and the exact screen contents are
in [06-debloat.md](06-debloat.md#things-to-know). It is not a boot loop: `dumpsys` will show
the launcher resumed underneath.

## `pm` / `settings` from a shell UID → "Permission Denial: ... INTERACT_ACROSS_USERS"

Some `settings` subcommands need a privileged caller. You can still read most settings from
Java via `Settings.Global.getString()`, or just use root:

```bash
adb shell su -c "settings get global adb_enabled"
```

## `/vendor/bin` → Permission denied

`/vendor/bin` is `drwxr-x--x root shell`. A shell UID outside the `shell` group cannot list
it, and an app cannot even traverse it. As root it is readable, which is how the vendor
binaries were pulled for analysis.

## `dumpsys package` from an app → "missing android.permission.DUMP"

Normal. Use a shell UID or root.

## After a reboot adbd is shell again

Expected. `service.adb.root` is not persistent. Either run `adb root` again, or rely on the
`ktvsu` daemon for app-level root — it starts automatically and does not care about adbd's
mode.

## Recovering from something worse

If the TV boots but the launcher is missing:

```bash
adb shell pm enable com.zeasn.launcher
adb shell input keyevent KEYCODE_HOME
```

If you disabled something else important and cannot remember what:

```bash
adb shell pm list packages -d          # everything currently disabled
adb shell pm enable <package>
```

If ADB is gone entirely (you disabled USB debugging or a package that owns the toggle), the
only route back is the TV UI. This is why nothing in this repository touches the
`adb_enabled` setting.

If the device does not boot at all, you flashed something you should not have. There is no
published recovery image for this board, so stop and look for a `Hi3751V560` / `Hi3751V553`
full dump matching your panel before doing anything else.
