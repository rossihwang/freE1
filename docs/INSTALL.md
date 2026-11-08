# Installing freE1

Two files and one restart. No toolchain, no flashing, nothing written to the
read-only system partition - the factory app stays where it is and comes back the
moment the two files are removed.

| File | Goes to | What it is |
|---|---|---|
| `freE1` (aarch64 binary) | `/userdata/freE1/freE1` | the UI + recorder |
| `e1ui-launch.sh` | `/userdata/system/SogouE1` | launcher; brings up audio, WiFi, time, SSH, then execs the binary |

The device's own boot chain does the rest:

```
inittab → /usr/bin/SogouLunchProbe.sh → /oem/SogouLunch.sh → /usr/bin/SogouLunch.sh
                                                              ├─ /userdata/system/SogouE1  (present → run freE1)
                                                              └─ /usr/bin/SogouE1          (absent  → factory app)
```

So installing is "put a script in the slot", uninstalling is "delete it".

## What you need

- The E1 (any state, including a factory-fresh one) and its USB cable
- **adb** from Android platform-tools
- The two files: built as described in the [README](../README.md) - `build_e1ui/freE1`
  and `scripts/e1ui-launch.sh` - or taken from a published release, if there is one

The device runs `adbd` from its own firmware as root, so adb works out of the box
over USB and `adb shell` already has the privileges the install needs - no
`adb root`, no sudo. SSH is a different matter: the factory image has no SSH server, so a
freshly-unboxed device only answers on USB until something installs one.

## Windows

1. Install platform-tools (PowerShell, no admin needed for the user install):
   ```powershell
   winget install Google.PlatformTools
   ```
   or download the zip from Google, unpack it and add that folder to `PATH`.
2. Plug the device in, then check it is visible:
   ```powershell
   adb devices
   ```
   It should list one device. If the list stays empty, open Device Manager and
   look for an unknown device with a warning sign - the E1 exposes a plain ADB
   interface, and Windows needs the generic ADB driver (the one from Google's
   platform-tools zip, or any "Universal ADB driver"). Changing the USB port or
   cable is worth trying first.
3. Install (run from the folder holding the two files):
   ```powershell
   adb shell mkdir -p /userdata/freE1
   adb push freE1 /userdata/freE1/freE1
   adb push e1ui-launch.sh /userdata/system/SogouE1
   adb shell chmod +x /userdata/freE1/freE1 /userdata/system/SogouE1
   adb shell "killall SogouE1; killall freE1"
   ```

The `killall` is not optional: the running app has to die so that init re-runs
the launcher, which now finds the slot filled. Expect the screen to go black for
a second or two, then show the freE1 record page.

## Linux

```bash
sudo apt install android-tools-adb        # Debian/Ubuntu
sudo pacman -S android-tools              # Arch
sudo dnf install android-tools            # Fedora

adb devices
```

If `adb devices` shows `no permissions`, add a udev rule for the interface the
device presents and reconnect (or, as a quick check, run `sudo adb kill-server;
sudo adb devices` - if that works, it is a permission problem, not a driver one).

The install commands are identical to the Windows ones:

```bash
adb shell mkdir -p /userdata/freE1
adb push freE1 /userdata/freE1/freE1
adb push e1ui-launch.sh /userdata/system/SogouE1
adb shell chmod +x /userdata/freE1/freE1 /userdata/system/SogouE1
adb shell "killall SogouE1; killall freE1"
```

If the device already answers on SSH (because a previous freE1 install put a
dropbear in `/userdata/bin`), the same thing can be done without a cable:

```bash
scp freE1 root@<device-ip>:/userdata/freE1/freE1
scp e1ui-launch.sh root@<device-ip>:/userdata/system/SogouE1
ssh root@<device-ip> 'chmod +x /userdata/freE1/freE1 /userdata/system/SogouE1; killall SogouE1; killall freE1'
```

`device-app.sh enable` (in this repo) is exactly this, over SSH, with a rollback
if the UI fails to start three times.

## Check it worked

```bash
adb shell 'ps | grep "[f]reE1 ui-dev"'
adb shell 'tail -20 /mnt/sdcard/freE1/boot.log'
```

The screen should show the record page with a running clock. The log line
`device UI up` and a steady stream of `fb: flush ...` entries mean it is alive.

## Update

Repeat the install commands. The binary is replaced and the launcher restarted;
settings (`format`, `volume`, `screen_off_sec`, `input_mode`) and recordings are
untouched, they live in `/mnt/sdcard/freE1`.

## Uninstall

```bash
adb shell 'rm -f /userdata/system/SogouE1; killall freE1'
```

The slot is empty, so the next launcher run execs the factory app. Nothing else
was modified, and the recordings in `/mnt/sdcard/freE1` stay where they are -
delete that directory too if you want them gone.

## If something goes wrong

| Symptom | Likely cause |
|---|---|
| `adb devices` empty | driver (Windows), udev permission (Linux), charge-only cable, or another adb server holding the port (`adb kill-server`) |
| Screen black after install, device otherwise alive | the launcher ran but the binary failed: `adb shell 'tail -30 /mnt/sdcard/freE1/boot.log'`; a missing `libasound.so.2` or a binary built against a newer glibc both look like this |
| Device boots into the factory app | `/userdata/system/SogouE1` missing or not executable |
| Device boots into freE1 but no SSH | the launcher only starts dropbear if `/userdata/bin/dropbear` exists; adb still works |
| Stuck, want the factory app back | `adb shell 'rm -f /userdata/system/SogouE1; killall freE1'`, or power-cycle - the factory app starts by itself once the slot is empty |

The three-failure fallback in `device-app.sh` covers the same ground for the
scripted path: if the UI dies three times within 30 s of boot, the slot script
hands control back to the factory app.
