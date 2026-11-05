/* display_fb.c - LVGL display driver for the E1 framebuffer (no mmap). */

#include "display_fb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

static int g_fd = -1;
static uint8_t *g_frame;
static int g_w, g_h, g_stride;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)px_map;
    int x1 = area->x1, x2 = area->x2;
    int y1 = area->y1, y2 = area->y2;
    if(x1 < 0) x1 = 0;
    if(y1 < 0) y1 = 0;
    if(x2 >= g_w) x2 = g_w - 1;
    if(y2 >= g_h) y2 = g_h - 1;

    int w = x2 - x1 + 1;
    if(w > 0) {
        for(int y = y1; y <= y2; y++) {
            off_t off = (off_t)y * g_stride + (off_t)x1 * 4;
            ssize_t r = pwrite(g_fd, g_frame + off, (size_t)w * 4, off);
            (void)r;
        }
    }
    lv_display_flush_ready(disp);
}

const uint8_t *e1_fb_frame(void)
{
    return g_frame;
}

lv_display_t *e1_fb_create(const char *dev)
{
    g_fd = open(dev, O_RDWR);
    if(g_fd < 0) {
        fprintf(stderr, "fb: open %s: %s\n", dev, strerror(errno));
        return NULL;
    }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    memset(&v, 0, sizeof(v));
    memset(&f, 0, sizeof(f));
    if(ioctl(g_fd, FBIOGET_VSCREENINFO, &v) != 0 || ioctl(g_fd, FBIOGET_FSCREENINFO, &f) != 0) {
        fprintf(stderr, "fb: ioctl: %s\n", strerror(errno));
        return NULL;
    }

    g_w = v.xres;
    g_h = v.yres;
    g_stride = f.line_length > 0 ? f.line_length : g_w * 4;

    if(v.bits_per_pixel != 32) {
        fprintf(stderr, "fb: expected 32 bpp, got %u\n", v.bits_per_pixel);
        return NULL;
    }

    g_frame = calloc(1, (size_t)g_stride * g_h);
    if(!g_frame) {
        fprintf(stderr, "fb: oom\n");
        return NULL;
    }

    lv_display_t *disp = lv_display_create(g_w, g_h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, g_frame, NULL, (uint32_t)((size_t)g_stride * g_h),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    /* paint black once so a stale factory frame cannot stay on screen */
    pwrite(g_fd, g_frame, (size_t)g_stride * g_h, 0);

    fprintf(stderr, "fb: %dx%d stride=%d bpp=%u rgba=%u/%u %u/%u %u/%u %u/%u\n",
            g_w, g_h, g_stride, v.bits_per_pixel,
            v.red.offset, v.red.length, v.green.offset, v.green.length,
            v.blue.offset, v.blue.length, v.transp.offset, v.transp.length);
    return disp;
}
