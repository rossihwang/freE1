#ifndef E1_DISPLAY_DRM_H
#define E1_DISPLAY_DRM_H

#include <stdbool.h>
#include <lvgl.h>

/* Double-buffered DRM/KMS backend: render into a cached shadow frame, copy it
 * into the back dumb buffer, then queue a page flip and wait for the flip
 * completion event before letting LVGL render the next frame. This removes
 * tearing by construction (the buffer being drawn is never the scanned one).
 *
 * The E1 fbdev does not support mmap, FBIO_WAITFORVSYNC, or vblank timestamps,
 * but dumb buffers + page flip + flip events all work. */
lv_display_t *e1_drm_create(const char *card);
void e1_drm_poll(void);
void e1_drm_restore(void);
const uint8_t *e1_drm_frame(void);

#endif
