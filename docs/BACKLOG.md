# freE1 backlog

Planned work that is **not** implemented yet. One entry per feature; keep the
list short and ordered by value.

Status legend: `idea` / `designed` / `in progress` / `blocked` (needs hardware or
someone at the device).

## Audio

| Item | Status | Notes |
|---|---|---|
| Monitor path (record → headphone) | **implemented, pending listening check** | Src/audio path done; 10 s recording with monitor showed 0 underruns / 0 drops. Needs a headphone listening check tonight. |
| VAD (voice-activated recording) | idea | Threshold tuning needs a real room. |
| Gain presets (meeting / interview / music) | idea | AGC is currently a single fixed profile. |
| Manual gain + AGC on/off | idea | Expose the AGC parameters that are already in `rec.c`. |
| Omni delay alignment | blocked | Needs the true mic positions. Current knowledge: 6 mics in two columns, ~9–10 cm × 3–3.5 cm; TDOA from claps was unreliable. Measure with a fixed source at 1 m. |

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
