# 02 — Recon: what the box looked like from the LAN

Before any settings were touched, this is what the TV exposed on `192.168.100.36`.

## Full TCP sweep

A connect scan of all 65536 ports returned:

```
OPEN 4321
OPEN 14035   (transient, refused on re-test — almost certainly an ephemeral source port)
```

Nothing else. No 5555, no 8008/8009/8443 (Google Cast), no 1900 SSDP, no 5353 mDNS, no
HTTP anywhere.

| Probe | Result |
| --- | --- |
| ICMP | alive, TTL 64 |
| 5555 (ADB) | refused |
| 8008 / 8009 / 8443 (Cast) | refused |
| 80 / 443 / 8080 / 9090 | refused |
| 1900 SSDP discovery | silence — no UPnP/DLNA advertisement |
| 5353 mDNS browse | silence |
| `adb connect :4321` | socket accepted, ADB handshake never answered → `offline` |

## Port 4321

This was the only real signal, and it behaved oddly enough to be interesting:

```
connect, send nothing   ->  5.00 s of silence, then a single byte 0xC0, then close
send any byte           ->  connection held open, server waits indefinitely
send 192 zero bytes     ->  RST
HTTP GET / CNXN / JSON  ->  ignored
```

Deterministic to ±10 ms. `0xC0` is an idle-timeout error byte, not a protocol version.

It could not be identified without a shell, because `/proc/<pid>/fd` is `0500` owned by
each process and even `shell` cannot read the sockets of root processes. Once root was
available:

```
$ adb shell su -c "ss -tnlp"
LISTEN 0 20 0.0.0.0:4321 users:(("hwtvmw@1.0-serv",pid=1707,fd=20))
LISTEN 0 4     *:5555  users:(("adbd",pid=10433,fd=12))
```

**Port 4321 is `vendor.hisilicon.hardware.hwtvmw@1.0-service`** — the HiSilicon TV
middleware HAL. It is the proprietary channel the TV's own applications and vendor remote
apps use to drive TV middleware (source switching, PQ, panel state). It is not a debug
port, it is not an ADB variant, and there is nothing to exploit there.

Worth writing down so nobody else spends an evening on it.

## What could be learned without a shell

An installed app (see the side-channel approach in
[03-enable-adb.md](03-enable-adb.md#if-you-cannot-reach-developer-options)) could read
properties and world-readable files, which produced most of
[01-device-profile.md](01-device-profile.md). Two things it could not do:

- read `/proc/<pid>/fd` of other processes (DAC, not SELinux — permissive does not help)
- enumerate other processes at all (`hidepid` hides them from non-shell UIDs)

Everything else came after ADB.

## Vendor app inventory

From `/system/app` and `pm list packages -f -s`, the vendor stack is:

```
com.cultraview.setting        CtvSetting       the blue TV settings UI
com.cultraview.tvboot         CtvBoot          boot-time helper
com.cultraview.livetv         CtvLiveTv
com.cultraview.guide          CtvGuide
com.cultraview.filemanager    CtvFileManager
com.cultraview.screensaver    ScreenSaver
com.cultraview.osupdate       OSupdate         OTA updater (A/B update_engine wrapper)
com.cultraview.cusfactorytest CHFactoryTest    factory test harness
com.cultraview.ctvfactorymenu CtvFactoryMenu   factory menu
com.ctv.autotest.helper       NewAutoTestHelper  broadcasts android.intent.action.AUTO_TEST_CMD
com.ctv.hotelsetting          HotelSetting     hotel/kiosk mode
com.hisilicon.rmservice       HiRMService      RAM/suspend manager (NOT a socket listener)
com.hisilicon.higallery       HiGalleryL
com.hisilicon.miracast        Miracast
com.zeasn.launcher            TvLauncher_Gray  the actual launcher
zeasn.open.technical.tv.store TvAppStore_Gray  vendor app store
com.sraf.launcher.ext         SadApp           adware, see below
com.zeasn.deviceportal.asdprovider.cetus.pub   ad provider
```

### Two findings worth flagging

**`HiRMService` is a red herring.** The name suggests "remote management" and therefore a
daemon listener. Decompiled, it is a memory/suspend manager (`HiSuspend`, `HiRamManager`,
`RamController`) with no `ServerSocket` anywhere in the APK. It does not own port 4321.

**`com.sraf.launcher.ext` ("SadApp") does not belong.** Evidence:

```
/system/app/SadApp/SadApp.apk        directory mtime 2026-05-04
firmware build date                  2024-12-30
pkgFlags: SYSTEM HAS_CODE PERSISTENT ALLOW_CLEAR_USER_DATA
lastUpdateTime = firstInstallTime = 2026-05-04 19:48:18
```

A **persistent** system app, installed five months *after* the firmware was built, running
a `FloatVideoWindowService` and logging the foreground activity on a timer:

```
D [SAD-APK][FloatVideoWindowService]: TIMER_TAG_SCAN_FOREGROUND
D [SAD-APK][FloatVideoWindowService]: allowed:[com.zeasn.launcher] topActivity: com.android.settings
```

That is an ad-injection framework watching what you are using. It was pushed post-sale,
presumably by OTA. It is the first thing the debloat script removes.

## A note on the vendor's own scripts

`init.ctv.rc` starts several root services from `/system/bin`:

```
preinstallapp   /system/bin/preinstallapp.sh   (root, u:r:shell:s0, on boot_completed)
preinit         /system/bin/pre-init.sh        (root, oneshot, start of boot)
myautolog       /system/bin/myautolog          (root, started by persist.sys.autolog=1)
presetGoogle    /system/bin/presetGoogle.sh    (root, copies gservices.db)
ctv_usb_debug   /system/bin/udiskcmd.sh        (root -- file does not exist on this build)
```

`preinstallapp.sh` installs every APK in `/system/preinstall/app` as root once, gated by
`persist.sys.preinstallapp`. That directory is on the read-only partition, so it is not a
writable injection point — but it explains the `WETV.apk` that appears preinstalled.

`ctv_usb_debug` referencing a script that does not exist means the whole USB-debug feature
is dead on this build.

Shelld apks note: `shell` can set `persist.*` properties but not `ctl.*`, so init service
start/stop triggers are not reachable without root. Worth knowing before you try
`setprop ctl.start <anything>`.
