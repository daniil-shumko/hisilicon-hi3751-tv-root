# 03 — Enabling ADB

Nothing in this repository works until adbd is listening. On this box that takes two taps,
but the menus are not where you expect them.

## The confusing part, up front

This TV has **two separate settings apps**:

| App | Package | Looks like |
| --- | --- | --- |
| Vendor TV settings | `com.cultraview.setting` | the blue UI: My / Home / Movie / Music / Sports / Apps, gear icon top-right |
| AOSP Settings | `com.android.settings` | plain grey/blue Android phone settings, "About phone", "Build number" |

The vendor UI has **no System section and no Developer options, ever**. Developer options
only exist in the AOSP app, which is installed but never launched by the launcher. If you
tap the build number in the vendor "System Info" screen, nothing happens — its labels are
not clickable.

This is the single thing that makes people give up on these boxes.

## Step 1 — open AOSP Settings

If you can only use the remote, you need a way to launch an activity that has no launcher
icon. Two approaches:

### Option A: via ADB from another device (chicken-and-egg, skip)

Only if you already have a foothold.

### Option B: an installed app that fires an Intent (what was used here)

Any APK you can sideload that calls `startActivity()` works. The one used during this work
was a ~20 KB WebView app that exposed an `intent(action, uri)` method over a local HTTP
channel, so the exact action could be fired from a laptop.

The actions that work, in order of usefulness:

```
android.settings.APPLICATION_DEVELOPMENT_SETTINGS   -> straight to Developer options
android.settings.DEVICE_INFO_SETTINGS               -> About phone
android.settings.SETTINGS                           -> main AOSP Settings list
```

Implicit component launches of the form
`com.android.settings/.Settings$DevelopmentSettingsDashboardActivity` **fail** with
`ActivityNotFoundException` — those activities are not exported on this build. Use the
action strings, not component names.

### Option C: a remote with a pointer

The stock remote emulates a mouse pointer over the TV UI. If you have a way to launch
arbitrary activities from an app you already trust, use it. Otherwise side-loading a tiny
launcher app is the pragmatic route.

## Step 2 — enable developer mode

Once `com.android.settings` is open:

```
Settings -> About phone -> tap "Build number" seven times
```

You will get "You are now a developer!" The About screen on this build shows:

```
Android version            14
Android security update    1 November 2023
Google Play system update  14
Baseband version           Unknown
Kernel version             5.10.66_s5
                           #12 Mon Dec 30 16:07:15 CST 2024
Build number               Hi3751V560_DMOH-userdebug 14 UP1A.231105.003.A1
                           eng.yankeq.20241230.160120 dev-keys
```

Then:

```
Back out to Settings -> System -> Developer options -> USB debugging = ON
```

## Step 3 — connect

```bash
adb connect <tv-ip>:5555
adb devices
# 192.168.100.36:5555    device
adb shell id
# uid=2000(shell) gid=2000(shell) ... context=u:r:shell:s0
```

**Expect no authorization prompt.** On a correctly configured device you would see
"Allow USB debugging?" with an RSA fingerprint. Here you get straight through, because
`ro.adb.secure` is unset:

```bash
adb shell getprop ro.adb.secure     # empty
adb shell ls -l /data/misc/adb/     # no adb_keys file exists
```

That is worth pausing on. It means the *first* device that connects to your TV on your
network gets a root-capable shell. There is no trust boundary.

## How the port got there

You never ran `adb tcpip 5555`. You did not need to. `/vendor/etc/init/hw/init.bigfish.rc`
does it at boot:

```
    mkdir /dev/usb-ffs/adb 0770 shell shell
    mount functionfs adb /dev/usb-ffs/adb uid=2000,gid=2000
    setprop persist.adb.tcp.port 5555
    setprop service.adb.tcp.port 5555
```

The `adb_enabled` framework setting is the only gate. Toggling USB debugging flips it, and
adbd starts listening on `0.0.0.0:5555` with no authentication.

## If you cannot reach developer options

Fallback chain that was actually available on this box before ADB:

1. **The TV has a full browser** (`com.yandex.browser.tv`) and an APK installer
   (`com.apkinstaller.ApkInstaller`). You can download an APK from a local HTTP server and
   install it without ADB. That is how the first foothold was obtained.
2. **The app store** (`zeasn.open.technical.tv.store`) also installs APKs.
3. **Wireless debugging** is *not* available on this build — the toggle is absent from
   Developer options. Do not go looking for pairing codes; plain TCP ADB on 5555 is the
   only path, and it works better anyway.

## Making ADB persist

It already does. `persist.adb.tcp.port` is set by init every boot, and the
`adb_enabled` setting survives reboots in the settings database.

Observed across a full power cycle:

```
boot                  ~38 s
adb devices           192.168.100.36:5555  device   (as uid 2000 shell)
```

You need `adb root` again after each reboot (see [04-root.md](04-root.md)), but you never
need to touch the TV UI again.
