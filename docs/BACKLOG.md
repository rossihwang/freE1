# freE1 backlog

Planned work that is **not** done yet, grouped by area. Priority is the reader's
call; each entry carries a value note and a rough effort so the order can be
decided rather than guessed.

Status: `idea` / `designed` / `in progress` / `blocked` / `done`.
Effort: **S** ≤ 1 h, **M** ≈ half a day, **L** ≈ 1–2 days, **XL** > 2 days.

## Audio pipeline

| Item | Status | Value | Effort | Notes |
|---|---|---|---|---|
| Omni L/R delay-aligned synthesis | designed, unblocked | high (quality) | M | Design §3.1: align the three mics of each row to their centroid, sum below the crossover, use the nearest single mic above it. Ungated summing combs badly — 30 mm spacing is already half a wavelength at 5.7 kHz. Geometry is now measured (`MIC_ARRAY_GEOMETRY.md`: row pitch ≈ 30 mm, row offset ≈ 20 mm), which was the blocker. Needs fractional delay + crossover + per-mic gain. |
| Per-mic gain calibration | designed | high (quality) | S | Knock test per row, normalise each mic to the same sensitivity, store the table. Measured spread today: the six omni channels sit at −65 dBFS of room noise, ch6/ch7 at −72 dBFS, i.e. ~6.5 dB quieter. |
| Look-ahead peak limiter | designed | high (quality) | M | Loud takes all peak at exactly −0.9 dBFS, the ceiling of the current limiter: it works per 1024-frame block with a 0.5 attack and steps between blocks, audible as pumping. Plan: 2 ms look-ahead, per-sample smoothed gain (1 ms attack / 100 ms release), ceiling −1.5 dBFS, AGC frozen while it acts, plus a CLIP hint when pre-AGC samples reach 0.999 (real ADC saturation). Verify offline on a synthetic quiet-plus-transient signal first. |
| Gain presets + manual gain | idea | medium | S | The AGC has a single profile. Hardware gain is unavailable: card 2 (PDM) exposes no mixer controls, the codec ADC gain (0…+20 dB) was measured to have no effect on the array, and the AC107 gain register sits behind a bound i2c driver with no datasheet. Presets can therefore only move the software AGC (target level, gate, time constants). |
| Monitor: mono downmix in directional mode | idea | low | S | Design §3.2: ch6/ch7 are near-identical (correlation 0.89, zero lag), so sending both to both ears combs. |
| VAD annotation (no gating) | designed | medium | M | Design §3/§5.2: 16 k downmix, 10 ms frames, adaptive floor, 300 ms sustained + 1.5 s hangover, intervals written to the file's metadata. Enables the voice-activated mode below and marks silence for review. |
| Voice-activated recording (`armed` state) | designed | medium | L | 3 s circular pre-roll, arm timeout 5 min, safety stop after 30 min of silence. Blocked on one measurement: idle drain with the PDM stream always on (2100 mAh, unknown). |
| Real-time noise reduction | idea | medium (quality) | XL | A v2 item; the vendor `libssb1` stack is closed and the open alternatives need CPU headroom on an A53. The current output sounds "further away" than the factory app. |
| 44.1 kHz support | blocked | low | L | The PDM driver returns exact zeros on ch6/ch7 below 48 kHz (limits L17). Needs a driver fix, or software resampling from 48 k with per-channel latency checks. |

## Interaction and UI

| Item | Status | Value | Effort | Notes |
|---|---|---|---|---|
| Lock mode (hold MARK 2 s) | designed | high (usability) | M | Design §5.1: a long press of MARK locks the touch layer for the duration of a recording, with a progress ring while holding; only MARK keeps working (marks are still recorded) and STOP is swallowed until unlocked. The design flags the risk: one ADC-divider key is the only exit, so a stuck key would leave no way to stop; the power-key 6 s hard reset is the fallback and belongs in the acceptance matrix. |
| Keyboard symbol layer | idea | high | S | The password keyboard offers 123/abc/ABC only. WPA passphrases routinely contain `!@#$%&*`, so such networks cannot be joined today. The factory QML reference for the layout is kept outside the repo (not published). |
| Record-page waveform | idea | medium | S | The chart appends points only when dBFS > −70 and starts at the right edge, so a take shows a short squiggle instead of a trace. Either fix the series/point-count handling or drive it from a stored peak envelope. |
| "Keep screen on while recording" option | idea | medium | S | The panel times out 60 s into a recording; some users will want it to stay lit. |
| Next / previous segment playback | idea | medium | S | A 1 h recording spans several files and playing one stops at its end. Auto-advance to the next segment (plus a way back) makes reviewing a long session practical. |
| Capacity bar on the Files page | designed | low | S | Design §6.2 asked for one; Storage is only visible in Settings today. |
| Device-side multi-select delete | idea | low | S | HTTP has multi-select; on the device rows are still deleted one at a time via long-press. |
| Marks: jump between marks during playback | parked | low | S | Deliberately not implemented (decision 2026-11-07): the device records marks and shows whether a file has any, nothing more. |

## Files and data

| Item | Status | Value | Effort | Notes |
|---|---|---|---|---|
| Per-file JSON sidecar | designed | medium | S | Design §4 wants `device_id`, mode, gain, dual timestamps, `vad_intervals` and `marks` beside each WAV. Today there is only `session.json` for the current segment plus a `.marks` file. |
| Daily subdirectories (`/YYYYMMDD/`) | designed | low | S | The design's layout. Everything currently sits in one directory, which is fine at 100 files and awkward at 1000. |
| Group by date / total size on Files | idea | low | S | Revisit once the list gets long; previously judged unnecessary. |

## Network and export

| Item | Status | Value | Effort | Notes |
|---|---|---|---|---|
| 16-bit preview transcode | idea | medium | M | 24-bit WAV previews in Chrome/Edge/Firefox but not reliably in iOS Safari. Either transcode on the fly (bit depth and resampling are cheap at 48 k) or offer a "download 16-bit" action. |
| HTTP authentication | idea | medium | M | The delete endpoint now requires POST plus a per-page token, but there is still no login: anyone on the LAN can list and delete. A shared password (or a token shown in Settings) closes that. |
| HTTP playlist / play all | idea | low | S | The page previews one file at a time. |
| WPS / hidden SSID | blocked | low | — | Depends on this `wpa_supplicant` build; needs a `strings` probe first. |
| Signed self-update over HTTP | idea | low | L | Field updates currently mean running `device-app.sh` from a laptop. Feasible, but it adds a remote-code path to a device with no authentication, so it should wait for HTTP authentication. |

## Power and reliability

| Item | Status | Value | Effort | Notes |
|---|---|---|---|---|
| Battery runtime characterisation | idea | medium (claims) | L | The "26 h capacity / 11.7 h recording" numbers are inferred from current measurements, not from a full discharge. A scripted run (`batt-log.sh` already exists) would confirm the real curve. |
| ext2 error monitoring | idea | medium (data) | S | The card is ext2 with no journal; a hard power cut can corrupt it and nothing notices today. Count kernel ext2 warnings and raise a `CARD ERROR` banner instead of carrying on silently. |
| RTC / NTP jump handling | idea | low | S | File names use the local clock, which is wrong until NTP lands, so a jump can make sort order odd. `session.json` carries dual timestamps; the list could sort by mtime instead of name. |

## Repo and maintenance

| Item | Status | Value | Effort | Notes |
|---|---|---|---|---|
| LICENSE | idea | **high (publishing)** | S | The repo is public with no licence file, which leaves reuse legally undefined. Pick one (MIT or Apache-2.0) and state the third-party situation (LVGL is MIT; no vendor binaries are included). |
| Host-side unit tests | idea | medium | M | `build_pc` can exercise `wav` write/repair, `session`, `marks` parsing and the HTTP token check without hardware. Only `swipetest` exists today. |
| CI build | idea | low | M | Cross-building in Actions needs the aarch64 toolchain; a PC-only build plus the unit tests above would already catch most regressions. |
| hwtest: keep as separate tree or fold in | idea | low | M | The hardware test is a separate program with its own LVGL copy and page set, duplicating the display and input layers. Worth deciding whether it stays separate. |

## Not planned

| Item | Reason |
|---|---|
| Cloud transcription / translation / assistant | Vendor algorithms are closed and the services need an account (design §1). |
| Chinese UI / IME | Out of scope for v1 (design §6: English LVGL UI, Montserrat only). |
| Bluetooth | Not verified to work without the factory stack. |
| 96 kHz / "music mode" | The array is documented as 192 kHz capable, but the recorder does not need it. |
| Multi-unit sync | Out of scope for v1 (metadata hooks only). |
| Boot splash / shutdown animation | The u-boot phase cannot be changed (limits L14). |
| USB UAC audio gadget | The vendor kernel exposes only `ffs.adb` and `mtp.gs0` (limits L2/L3). |
| Auto-delete of old recordings when full | Explicit design decision: never delete user data; stop recording instead. |
