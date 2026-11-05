#ifndef E1_DISPLAY_FB_H
#define E1_DISPLAY_FB_H

#include <lvgl.h>

/* The E1 /dev/fb0 (rockchip DRM fbdev) does not implement mmap:
 * mmap() returns EINVAL. It does implement read()/write() at an offset,
 * and its pixel layout is BGRA in memory (R@16 G@8 B@0), which is exactly
 * LVGL's XRGB8888. So we keep a shadow frame in RAM and pwrite the dirty
 * rows to the framebuffer. */
lv_display_t *e1_fb_create(const char *dev);
const uint8_t *e1_fb_frame(void);

#endif
