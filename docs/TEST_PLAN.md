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
