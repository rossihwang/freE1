# freE1 backlog

Planned work that is **not** implemented yet. One entry per feature; keep the
list short and ordered by value.

Status legend: `idea` / `designed` / `in progress` / `blocked` (needs hardware or
someone at the device).

## Audio

| Item | Status | Notes |
|---|---|---|
| Monitor path (record → headphone) | **done** | Works. Opens `hw:0,0` directly (the `default` plug only granted 4096 frames and underran 40–120 times per 2 min); start threshold 85 ms. Measured 0 underruns / 0 drops over 68 s, delay ~85–130 ms. |
| Mark / bookmark from the MARK key | **done (playback jump not done)** | Recording writes `<file>.marks` (millisecond offsets) and the file row shows a MARK badge; `tools/marks-export.py` converts to Audacity/Reaper/CSV/cue. Jumping between marks during playback is deliberately not implemented. |
| 44.1 kHz support | blocked | The PDM driver returns zeros on ch6/ch7 below 48 kHz (limits L17). Would need either a driver fix or software resampling from 48 k, plus per-channel latency checks. |
| VAD (voice-activated recording) | idea | Threshold tuning needs a real room. |
| High-dynamics peak handling (look-ahead limiter) | **designed** | Measured: peaks in loud takes all sit exactly at −0.9 dBFS, i.e. the limiter is permanently engaged, and it works per 1024-frame block with a 0.5 attack coefficient, so the gain steps between blocks — audible as pumping rather than as clean limiting. No sample exceeded the ceiling, so the mics themselves are not saturating yet. Plan: (1) 2 ms look-ahead limiter, per-sample smoothed gain, 1 ms attack / 100 ms release, ceiling −1.5 dBFS; (2) let the slow AGC own the average level and freeze it while the limiter acts; (3) count pre-AGC samples at or above 0.999 and show a CLIP hint, since that is ADC saturation software cannot undo; (4) fix the comment/code mismatch on the +40 dB ceiling. Verify offline with a synthetic quiet-plus-transient signal (no overshoot, smooth envelope) before trusting it on device. |
| Gain presets (meeting / interview / music) | idea | AGC is currently a single fixed profile. Hardware gain is not adjustable: card 2 (PDM) exposes no ALSA controls, the codec's ADC MIC gain (+0…+20 dB) was measured to have no effect on the array, and the AC107 gain register is behind a bound i2c driver (`Device or resource busy`) with no datasheet. So all gain control is software. |
| Manual gain + AGC on/off | idea | Expose the AGC parameters that are already in `rec.c`. |
| Omni delay alignment | **unblocked** | Mic positions measured (`MIC_ARRAY_GEOMETRY.md`): two rows × three columns, 64 × 25 mm. Note ch6/ch7 do not behave like array elements, so align the six omni channels only. |

## Files

| Item | Status | Notes |
|---|---|---|
| Group by date, total size, multi-select delete | idea | Judged **not necessary** for now; the current list + long-press delete is enough. Revisit if the card holds hundreds of files. |
| Previous / next segment in the transport bar | idea | Files are numbered per segment; a next/prev button would jump between them. |

## Power / hardware

| Item | Status | Notes |
|---|---|---|
| Backlight brightness setting | idea | Fixed at 200 now; screen-off timeout is separate and already configurable. |
| Battery curve refinements | idea | Staged warnings (20/10/5/2%) are in; could add an estimated remaining time. |
| Bluetooth | idea | Factory firmware used `rk_bt_*`. Not verified that the module is usable without the factory stack. |

## Not planned

| Item | Reason |
|---|---|
| Boot splash / shutdown animation | The u-boot phase cannot be changed (see `E1_ZhiyinOS限制清单.md` L14). |
| Cloud transcription | The factory algorithm binaries are closed and undocumented. |
| USB UAC (audio gadget) | The kernel gadget is configured for adb only (limitations L2). |
| Multi-unit sync | Out of scope for v1; revisit with a concrete use case. |
