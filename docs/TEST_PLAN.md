# Tonight's test list

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

## 2. Playback transport bar

| Step | Expected |
|---|---|
| Tap a file in Files | Row highlighted light green, transport bar appears, list shrinks |
| Tap `II` | Audio pauses, icon becomes `>` |
| Drag the progress bar | Time label previews the target position |
| Release | Playback jumps to that point |
| Tap `X` | Playback stops, transport bar disappears, list grows back |

## 3. 44.1 kHz / 16 bit

| Step | Expected |
|---|---|
| Settings → Format → `44.1k / 16bit` | Value changes and persists across reboot |
| Record 5 s | File plays back at correct speed and pitch (no chipmunk / slow motion) |

## 4. AGC / recording quality

| Step | Expected |
|---|---|
| Record normal speech at ~50 cm | Clear, no pumping, no clipping |
| Speak loudly | No distortion (peak-aware AGC should prevent clipping) |
| Stay silent | No audible noise floor pumped up |

## 5. WiFi fallback

| Step | Expected |
|---|---|
| Connect to another AP with a wrong password | After ~25 s a failure is reported and the device returns to the previous network |

## 6. Mic array geometry (for Omni alignment)

Needs a fixed sound source: phone at 1 m, playing a click train. Not yet
prepared — ask for a script that generates the file.
