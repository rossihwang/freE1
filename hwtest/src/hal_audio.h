#ifndef E1_HAL_AUDIO_H
#define E1_HAL_AUDIO_H

#include <stdbool.h>

/* Audio paths, all through the device's own arecord/aplay (known-good):
 *   capture: arecord -D hw:2,0 -c 8 -r 32000 -f S32_LE --period-size=2400
 *   playback: aplay  -D hw:0,0 -c 2 -r 32000 -f S32_LE
 *
 * Notes learned from the hardware:
 *  - PDM array needs 32 kHz / S32_LE; opening a stream has a ~0.2 s
 *    full-scale transient, so the meter ignores the first 400 ms.
 *  - The DAC resets 'DAC HPOUT' to -inf whenever a playback stream closes,
 *    so it must be re-armed right before every playback.
 *  - Inserting headphones mutes the speaker in hardware; no LINEOUT switch
 *    is needed.
 */
void  audio_init(void);

/* 8-channel level meter (mic array page) */
void  audio_meter_start(void);
void  audio_meter_stop(void);
bool  audio_meter_active(void);
float audio_level_db(int ch);        /* -90..0 dBFS, smoothed */
float audio_peak_db(int ch);         /* peak hold */
bool  audio_clip(int ch);

/* loopback page: record ch6/ch7 into RAM, normalise, play back */
void  audio_rec_start(void);
void  audio_rec_cancel(void);
bool  audio_recording(void);
bool  audio_rec_done(void);
float audio_rec_progress(void);      /* 0..1 */
void  audio_rec_reset(void);
float audio_rec_gain_db(void);
float audio_rec_peak_db(void);
void  audio_play(void);
void  audio_play_stop(void);
bool  audio_playing(void);
float audio_play_progress(void);
const float *audio_envelope(int *count);  /* 64 buckets, 0..1 */

void  audio_poll(void);

#endif
