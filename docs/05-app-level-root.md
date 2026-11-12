# 05 — App-level root: why `setuid` fails and what to do instead

`adb root` gives *you* root from your laptop. It does not give root to the applications on
the TV. If you want a file manager, a terminal, Titanium Backup or anything else that calls
`su` to actually work, you need a `su` that survives the Android app sandbox.

The obvious approach does not work, and the reason is worth understanding.

## Attempt 1 — a setuid-root binary (fails)

A `su` binary on `/system/bin`, owned `root:root`, mode `4755`:

```
-rwsr-xr-x 1 root root 6096 /system/bin/su
```

Executing it from a root shell works perfectly. Executing it from a normal app fails:

```
$ /system/bin/su -c 'id -u'
su: permission denied: Operation not permitted
```

Note what did *not* happen: the process started, `main()` ran, and `setuid(0)` returned
**EPERM**. The binary was not rejected, it was executed with the setuid bit silently
discarded.

### Why

Android's zygote sets `PR_SET_NO_NEW_PRIVS` on application processes. The kernel contract
is that exec'ing a setuid file then grants no new privileges. So:

- `execve()` succeeds
- the effective UID stays the app's UID
- `setuid(0)` fails with EPERM because the process is not privileged

This is not a bug and it is not vendor-specific. It is why Magisk, KernelSU and every other
modern Android root system uses a **root daemon** instead of a setuid shim. The setuid
approach only ever worked on Android 4.x-era builds, or in `u:r:su:s0` domains that
`adb root` already provides.

## Attempt 2 — a root daemon plus a thin client (works)

This is the design implemented in [`src/su/`](../src/su).

```
          app process (uid 10077, untrusted_app, no_new_privs)
                 |
                 |  exec /system/bin/su -c "id"
                 v
          su  (client, mode 0755, NO setuid bit)
                 |
                 |  AF_UNIX connect to abstract socket "ktvsu"
                 |  4-byte LE length + command bytes
                 v
          ktvsud  (daemon, uid 0, started by init from /system/etc/init/ktvsu.rc)
                 |
                 |  fork -> /system/bin/sh -c "<command>" as root
                 v
          stdout+stderr streamed back, then a 12-byte trailer:
          "KTVEXIT\0" + uint16 exit code + padding
```

### Why this is not blocked

No privilege is granted at exec time. `su` runs entirely as the app's own UID. It only
makes a socket connection. The daemon is already root because **init started it that way**,
and init is allowed to do that. There is no boundary being crossed by the app; it is just
talking to a service that happens to be privileged.

The socket is in the **abstract namespace** (`\0ktvsu`), so it has no filesystem path and
no DAC permissions to get wrong. Access control is the daemon's business (see the warning
at the bottom).

### Protocol

```
client -> daemon : uint32 LE length
                   length bytes of UTF-8 shell command
daemon -> client : raw stdout + stderr bytes (streamed, unbounded)
                   "KTVEXIT\0" + uint16 LE exit code + 2 zero bytes
```

The client holds back the last 12 bytes of the stream so it can detect the trailer without
buffering the whole output. Everything before it goes straight to stdout.

### Autostart

`/system/etc/init/ktvsu.rc`:

```
# KTV root daemon (ktvsu) - provides su for apps
service ktvsu /system/bin/ktvsud
    class late_start
    user root
    group root
    seclabel u:r:su:s0
```

`class late_start` means init starts it after the framework is up, not during early boot.
No `critical` flag, so a crashing daemon cannot block boot — it would just respawn in a
loop, which is noisy but harmless.

## Install

```bash
./scripts/install-su.sh <tv-ip>
```

Which does, in order:

1. verifies the device is the expected one (fingerprint + SELinux mode)
2. builds `su` and `ktvsud` for `armv7a-linux-androideabi21` using the NDK
3. `adb push` to `/data/local/tmp`
4. `adb root`, remount `/` rw, install both to `/system/bin`, mode `0755`, `root:root`
5. writes `/system/etc/init/ktvsu.rc`
6. starts the daemon immediately (so no reboot is needed to use it)
7. verifies escalation from a *non-root* shell, not from root

Expected output:

```
[*] device ok: Hi3751V560_DMOH / Permissive
[*] building for armv7a-linux-androideabi21
[*] installing to /system/bin
[*] writing /system/etc/init/ktvsu.rc
[*] starting daemon
[ok] su works: app uid -> 0
```

## Verify

From a normal app or a non-root shell:

```bash
adb shell su -c 'id -u'
# 0
```

From inside an app sandbox (this is the real test, and the one that fails for setuid):

```
id -u           -> 10077
su -c 'id -u'   -> 0
```

After a reboot, confirm the daemon is up without any manual action:

```bash
adb shell getprop init.svc.ktvsu     # running
adb shell ps -A -o USER,PID,NAME | grep ktvsud
# root  1891  ktvsud
```

Validated on the reference device: full power cycle, `init.svc.ktvsu=running`, `su` from an
app uid returned 0.

## Uninstall

```bash
./scripts/uninstall-su.sh <tv-ip>
```

Removes the `.rc`, kills the daemon, deletes both binaries. `su` stops existing. Root via
`adb root` is unaffected.

## Security warning — read this

**This daemon has no authentication and no user prompt.** Any process on the TV can obtain
root by connecting to the abstract socket. That is the intended behaviour of a classic
`su` — it is how rooted Android phones worked before Magisk introduced per-app approval —
but it means:

- an app you sideloaded, or a vendor app you did not debloat, can silently take root
- anything that can run code on the TV can now read every database, including credentials

If you want an approval prompt, add one: the daemon is ~120 lines, and the change is to
verify the connecting peer's UID against an allow-list (via `SO_PEERCRED`) and pop a
confirmation through a privileged component. That is deliberately left as an exercise —
a prompt you cannot see on a TV that is across the room, driven by a remote, is worse UX
than silence.

## Why not just use `adb root` over TCP from an app?

Because an app cannot speak the ADB protocol. It would need the ADB key, the transport
handshake, and a pairing step — and if it could, that would be a far larger hole than a
local unix socket. The daemon is the smaller, more honest design.
