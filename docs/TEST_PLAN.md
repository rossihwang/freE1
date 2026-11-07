# Test list (2026-11-06/07 session)

Status as of 2026-11-07 00:10 — see the notes under each item.

Items that need a person at the device. Check them off as they pass.

## 1. Monitor (record → headphones)

| Step | Expected |
|---|---|
| Plug in headphones, start recording, tap `MON` | Button turns green (`MON ON`) |
| Speak | Your voice is audible in the headphones, latency ~20–30 ms |
| Tap `MON` again | Monitoring stops, recording continues |
| Unplug headphones, tap `MON` | Warning `Plug in headphones to use monitor.` |
| Stop recording | Monitoring turns off automatically |

Record the subjective latency and whether any dropouts were heard.

**Result (2026-11-07):** works. Three problems were found and fixed on the way:
headphone output was muted by the codec after any plug cycle (`hs_audio_route`),
the MON chip sat 33 px from the panel edge and was awkward to hit, and the
playback buffer was 4096 frames (85 ms) which underran 40–120 times per two
minutes. Monitoring now opens `hw:0,0` (16384 frames) with an 85 ms start
threshold: 0 underruns over 68 s, delay ~85–130 ms.

## 2. Playback transport bar

| Step | Expected |
|---|---|
| Tap a file in Files | Row highlighted light green, transport bar appears, list shrinks |
| Tap `II` | Audio pauses, icon becomes `>` |
| Drag the progress bar | Time label previews the target position |
| Release | Playback jumps to that point |
| Tap `X` | Playback stops, transport bar disappears, list grows back |

**Result (2026-11-07):** the transport bar was removed at the user's request. Tap
on the playing row pauses and resumes, the row shows a background progress bar,
leaving the page stops playback.

## 3. 44.1 kHz / 16 bit

| Step | Expected |
|---|---|
| Settings → Format → `44.1k / 16bit` | Value changes and persists across reboot |
| Record 5 s | File plays back at correct speed and pitch (no chipmunk / slow motion) |

**Result (2026-11-07):** 44.1 kHz **removed**. At 44.1 kHz the PDM array returns
exact zeros on ch6/ch7 (see `E1_ZhiyinOS限制清单.md` L17), so directional mode
recorded silence. The second format option is now 48k/16bit, which halves the
file without touching the rate; container and content verified.

## 4. AGC / recording quality

| Step | Expected |
|---|---|
| Record normal speech at ~50 cm | Clear, no pumping, no clipping |
| Speak loudly | No distortion (peak-aware AGC should prevent clipping) |
| Stay silent | No audible noise floor pumped up |

**Result (2026-11-07):** pass. Speech at −24.3 ± 0.3 dBFS RMS, peaks −11 dBFS,
no sample at full scale, floor −72 dBFS.

## 5. WiFi fallback

| Step | Expected |
|---|---|
| Connect to another AP with a wrong password | After ~25 s a failure is reported and the device returns to the previous network |

**Result (2026-11-07):** the rollback worked (device back on its network within
seconds) but the page stayed on "not connected" because it only refreshed once,
at the moment of failure. The page now polls every 2 s and failed connects put
the previously saved psk back instead of deleting the network.

## 6. Mic array geometry (for Omni alignment)

**Done (2026-11-07).** Measured with the RAW 8CH mode and a click train: six omni
mics in two rows of three, 64 × 25 mm, columns ~30 mm apart, rows ~20 mm. See
`MIC_ARRAY_GEOMETRY.md`. ch6/ch7 turned out not to behave like array elements.

## 7. One-hour segment rotation (2026-11-07)

Started 00:26:35 on battery, stopped 01:33:44 (67 min).

| Check | Expected | Measured |
|---|---|---|
| First segment length | 3600.0 s | **3600.0 s** (`REC_0112`, 1,728,010,024 frames, 1.04 GB) |
| Rotation to a new file | sequence +1, new name | `REC_0113_20261107-0126.wav` |
| Second segment | remainder | 429.4 s, stopped by the STOP key |
| xrun / read failures | 0 | **0** |
| Battery | — | 99% → 91% in 67 min of 24-bit recording (USB unplugged) |
| UI RSS after 8.8 h uptime | flat | 3.1 MB |

**Bug found and fixed:** the screen goes off 60 s into a recording and `g_off_at`
keeps that timestamp, so the moment STOP cleared the "recording" guard the idle
check saw "screen off for over 180 s" and suspended the device 1 ms after the
key press - from the outside a stopped recorder that blacks out and ignores
touches. `set_rec_ui(0)` now resets the idle state. Retested with a 76 s
recording and the screen off: no suspend line after the stop.

Two ~1 GB test recordings are still on the card (`REC_0112`, `REC_0113`).

## 8. LED peak indication (2026-11-07)

The top RGB LED stays dark unless the level written to the file is close to full
scale. Thresholds are on the post-AGC peak, not on the limiter's internal state.

| Level | LED |
|---|---|
| below 0.7 FS | off |
| **0.7 FS** (-3 dBFS) or above | yellow, steady |
| **0.9 FS** or above | red, steady |
| tap the meter area | back to off (also clears the amber peak-hold marks) |
| stop recording / standby | off |

**Result (2026-11-07):** states verified with `clipsim 0|1|2` (`test: clipsim N`
in the log), reading `/proc/awled_enable|color|pwm` each time: off = `0/-/-`,
yellow = `1/4/200`, red = `1/1/240`, reset and stop = `0/-/-`. A real burst played
through the speaker only reached 0.386 FS and correctly lit nothing, so the
thresholds are not decorative - a real 0.7 event needs someone talking loudly
right at the array.

Two driver details had to be learned the hard way:
- writing `awled_color` or `awled_pwm` clears `awled_enable`, so the order must be
  colour, brightness, enable;
- `awled_enable` and `awled_pwm` read back as defaults (0 / 127) whenever the LED
  is off, which looks like a failed write but is not.

## 9. Volume keys (2026-11-07)

| Step | Expected |
|---|---|
| Press volume up / down | 5% steps, toast `VOL nn%`, softvol `Master` follows |
| Reboot | level restored from `volume` |

**Result (2026-11-07):** works. The device has five keys; the two volume keys
(`KEY_4`/`KEY_5`, codes 5/6) had no handler before. `vol 40` moved `Master` to
40% (-24 dB), `vol 70` to 70% (-12 dB), and the value survives a restart. The
monitor path bypasses softvol (it opens `hw:0,0` directly), so volume keys do not
change monitoring level.

## 10. Fast-forward by holding the playing row (2026-11-07)

| Step | Expected |
|---|---|
| Tap a file, then hold the same row | Scans forward at ~4x, toast `FF 4x` |
| Release | Back to normal speed at the new position |
| Long-press a row that is not playing | Delete dialog, as before |

**Result (2026-11-07):** 25 s take played in 24.2 s normally and 6.4 s with the
scan held (3.8x). The scan advances the file position inside the player instead
of seeking, so the PCM stream is never reconfigured and it does not click.

## 11. Clock drift and list ordering (2026-11-07)

A 1.5 h recording appeared to be missing. It was not: `REC_0154` held exactly
3600.0 s and `REC_0155` the remaining 705 s, but the RTC has no backup battery, so
after a power cycle the clock was 2019 and every file from that session carried a
2019 timestamp. Sorting by mtime pushed the newest takes to the bottom of a
24-file list.

| Check | Expected | Measured |
|---|---|---|
| Sort order | by sequence number, not mtime | `0157 → 0156 → 0155 → 0154 …` |
| Clock at boot | restored from `last_time` when the RTC is behind | `clock: implausible (2019)` then `clock: restored 1570186800 -> 1794050702` |
| `last_time` monotonic | never overwritten with an earlier value | written at most once a minute, only forwards |

## 12. HTTP page (2026-11-07)

| Check | Expected | Result |
|---|---|---|
| Preview in the browser | play button per row, streams via Range | 206 with `Content-Range`, `Accept-Ranges: bytes` |
| Delete a selection | POST with the page token, files and `.marks` removed | `?del=2&fail=0`, both files and sidecars gone |
| `GET /delete` | refused | 405 |
| POST without token | refused | 403 |
| Traversal name (`../system/SogouE1`) | refused, file untouched | `del=0&fail=1`, launcher md5 unchanged |
| Device list refresh | Files page updates without paging | list refreshed after an HTTP delete |

## 13. hwtest navigation (2026-11-07)

Single-finger drag used to scroll the whole page (LVGL screens are scrollable by
default and the content is taller than 320 px); the five page dots along the
bottom sat on top of page content. Both are gone, pages are sized to the full
height below the title bar, and the `2-finger swipe` hint that overlapped the
buttons was removed with them. Verify by eye: no dots, and dragging on the touch
grid paints cells instead of moving the page.

## 14. RTC (2026-11-07)

The RK808 PMIC has an RTC (`/dev/rtc0`, `hctosys=1`), so the kernel loads the
system clock from it at boot. Nothing had ever written it, which is the real
reason a power cycle lands in 2019 - NTP corrected the system clock but never the
RTC, and `rcS` has no `hwclock -w`.

| Check | Expected | Measured |
|---|---|---|
| RTC exists | `/dev/rtc0`, `rk808-rtc` | yes, `hctosys: 1` |
| Write it in UTC | `hwclock -u -w` makes `since_epoch` equal `date +%s` | 1794056617 = 1794056617 |
| Reboot keeps time | kernel loads the written value | `setting system clock to 2026-11-07 13:03:54 UTC` |
| Firmware self-heals | startup and every 10 min, write when >60 s apart | `rtc: synced (rtc 1570154401, sys 1794056730)` after dragging it back to 2019 |
| Guard | never write an implausible system time | skipped below 2024 |

Power loss (2026-11-07 21:10): powered off with `poweroff`, waited for the SSH
connection to die, then pressed the power key. The kernel loaded
`setting system clock to 2026-11-07 13:10:48 UTC (1794057048)` against a recorded
power-off time of 1794057039 - nine seconds earlier, i.e. the RTC kept counting
while the device was off. So there is a backup cell for the PMIC RTC and the 2019
values were purely "never written", not "no battery".

With the firmware now writing the RTC, the device keeps the correct time across a
power cycle even with no network, and the `last_time` restore becomes a fallback
rather than the only line of defence.
