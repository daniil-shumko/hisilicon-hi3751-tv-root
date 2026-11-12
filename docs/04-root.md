# 04 — Root

At this point you have `adb shell` as `uid=2000(shell)`. That is already useful — `logcat`,
`dumpsys`, `pm`, `settings`, real `ps` — but it cannot read `/vendor/bin`, cannot read
other processes' `/proc` entries, and cannot start init services.

Root is one command away.

## `adb root`

```bash
$ adb root
restarting adbd as root

$ adb shell id
uid=0(root) gid=0(root) groups=0(root),1004(input),1007(log),1011(adb),1015(sdcard_rw),
    1028(sdcard_r),1078(ext_data_rw),1079(ext_obb_rw),3001(net_bt_admin),3002(net_bt),
    3003(inet),3006(net_bw_stats),3009(readproc),3011(uhid),3012(readtracefs)
    context=u:r:su:s0
```

`adbd` is now running as:

```
root  3521  adbd --root_seclabel=u:r:su:s0
```

## Why this succeeds when it should not

On stock Android:

- `ro.secure=1` → adbd drops to the `shell` user at startup
- `ro.debuggable=0` → `adb root` is refused with *"adbd cannot run as root in production
  builds"*

This box has `ro.secure=1` and `ro.debuggable=0`, so both conditions are met — and yet
`adb root` works normally. Two contributing factors, stated with the confidence the
evidence supports:

1. **SELinux is `Permissive`.** The domain transition into `u:r:su:s0` that adbd performs
   when escalating is logged (`permissive=1`) rather than denied. On an enforcing build,
   even a permissive adbd would fail to `setcon()` into the su domain.
2. **The vendor's adbd accepts it.** The expected refusal string is absent from
   `/apex/com.android.adbd/bin/adbd`, which suggests the build was not compiled with the
   stock `ALLOW_ADBD_ROOT` gating. This part is inference, not proof.

`service.adb.root` becomes `1` while root adbd is running, and is reset on reboot — which
is why you re-run `adb root` after every power cycle.

## Making `/system` writable

By default `/` is read-only:

```
/dev/block/mmcblk0p20 on / type ext4 (ro,seclabel,nodev,relatime)
```

`adb remount` refuses, because it checks for an unlocked bootloader:

```
$ adb remount
Device must be bootloader unlocked
```

Ignore it. As root, a manual remount works because there is no dm-verity and no
super-partition abstraction on this device:

```bash
adb shell su -c 'mount -o rw,remount /'
```

Verify:

```bash
adb shell su -c 'mount | grep " / "'
# /dev/block/mmcblk0p20 on / type ext4 (rw,seclabel,nodev,relatime)
```

Sanity check that writes actually land — this was the test used, and it is reversible:

```bash
adb shell su -c 'touch /system/.write_test && rm /system/.write_test && echo WRITE_OK'
```

`/system` lives on this same partition, so it is now writable. `/vendor` is a separate
partition (`mmcblk0p21`) still mounted `ro`; remounting it is possible but nothing in this
repository needs it.

**Note:** the remount does not survive a reboot. If you are installing something permanent,
remount and write in the same session.

## If `adb root` is refused

Then you have a normal-ish build and need a real escalation. Things worth checking, in
order of effort:

1. **`ro.debuggable`** — if it is `1`, `adb root` will work regardless of `ro.secure`.
2. **`/system` writability without root** — check `mount | grep ' / '`. Some vendor builds
   ship `/` as `rw`, in which case a shell-uid `adb push` into `/system/xbin` may be
   possible directly.
3. **Exported vendor components** — factory/test apps (`CtvFactoryMenu`, `CHFactoryTest`,
   `NewAutoTestHelper`) are platform-signed and often hold `WRITE_SECURE_SETTINGS` or
   `INSTALL_PACKAGES`. `pm` and `am` from a shell UID can drive their exported activities
   and receivers.
4. **The OTA path** — `com.cultraview.osupdate` wraps Android's A/B `UpdateEngine`. If your
   build actually has slots (`ro.boot.slot_suffix` non-empty) and the payload signing keys
   are extractable (`/system/etc/security/otacerts.zip`), a signed payload gives root
   trivially. This box has no slot suffix, so that route was never applicable — and it is
   the route most likely to brick a working TV. Treat it as a last resort.
5. **Kernel exploit** — out of scope here. Security patch level 2023-11-01 on a 5.10.66
   vendor kernel means plenty of unpatched CVE surface, and permissive SELinux makes
   exploitation dramatically easier, but that is a research project, not a recipe.

## What root buys you

```bash
adb shell su -c "ss -tnlp"                 # which process owns which port
adb shell su -c "ps -A -o USER,PID,ARGS"   # full process list with args
adb shell su -c "ls -l /vendor/bin"        # vendor binaries are root:shell 0700
adb shell su -c "cat /proc/1707/cmdline"   # any process's command line
adb shell su -c "logcat -b all"            # every log buffer
adb shell su -c "dumpsys window"           # full window manager state
```

That list is how port 4321 was identified in
[02-recon.md](02-recon.md#port-4321).

Root also lets you keep it: see [05-app-level-root.md](05-app-level-root.md) for making
`su` available to ordinary applications, which is what makes root *useful* rather than
merely available.
