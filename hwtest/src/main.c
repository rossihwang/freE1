/* e1hwtest - Sogou E1 hardware test (ZhiyinOS 1.2.3, RK3308)
 *
 * 5 test pages + summary, navigated with a two-finger swipe:
 *   1 TOUCH     2 KEYS + LED     3 LOOPBACK     4 MIC ARRAY     5 VIBRATE
 *
 * Display: /dev/fb0 cannot be mmap'ed on this kernel, so display_fb.c keeps
 * a shadow frame and pwrite()s dirty rows (fb is BGRA = LVGL XRGB8888).
 * Touch: st16xx reports MT protocol B only; input.c reads it directly and
 * also extracts the two-finger page gesture.
 * Audio: arecord/aplay child processes (recipes verified on the device).
 */

#include <lvgl.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>

#include "app.h"
#include "input.h"
#include "display_fb.h"
#include "display_drm.h"
#include "hal_keys.h"
#include "hal_hp.h"
#include "hal_led.h"
#include "hal_motor.h"
#include "hal_audio.h"
#include "pages.h"

app_state_t g_app;
const unsigned char *g_display_frame;

static uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void ui_timer(lv_timer_t *t)
{
    (void)t;
    int sw = e1_input_take_swipe();
    if(sw){
        /* 最后一页（SUMMARY）双指上滑 = 退出，回 freE1 */
        if(sw > 0 && pages_current() == 4) pages_finish();
        else pages_next(sw);
    }
    pages_update();
}

/* The factory app pins scaling_max_freq to 816 MHz. The hardware OPP table
 * goes to 1296 MHz; keep the interactive governor (no permanent full speed)
 * and only lift the policy ceiling. "echo stock > .../cpu" keeps 816. */
static void cpu_policy(void)
{
    char mode[16] = "max";
    FILE *f = fopen("/mnt/sdcard/e1hwtest/cpu", "r");
    if(f) {
        if(fscanf(f, "%15s", mode) != 1) mode[0] = 0;
        fclose(f);
    }
    int maxf = strcmp(mode, "stock") == 0 ? 816000 : 1296000;
    f = fopen("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", "w");
    if(f) {
        fprintf(f, "%d\n", maxf);
        fclose(f);
    }
    fprintf(stderr, "cpu: mode=%s scaling_max_freq->%d\n", mode, maxf);
}

static void set_backlight(int v)
{
    int fd = open("/sys/class/backlight/backlight/brightness", O_WRONLY);
    if(fd >= 0) {
        char b[16];
        int n = snprintf(b, sizeof(b), "%d\n", v);
        if(write(fd, b, (size_t)n) < 0) {}
        close(fd);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    memset(&g_app, 0, sizeof(g_app));

    lv_init();
    lv_tick_set_cb(tick_cb);

    char backend[16] = "fb";
    FILE *bf = fopen("/mnt/sdcard/e1hwtest/backend", "r");
    if(bf) {
        if(fscanf(bf, "%15s", backend) != 1) backend[0] = 0;
        fclose(bf);
    }

    lv_display_t *disp = NULL;
    if(strcmp(backend, "drm") == 0) {
        disp = e1_drm_create("/dev/dri/card0");
        if(disp) g_display_frame = e1_drm_frame();
        else fprintf(stderr, "drm backend failed, falling back to fbdev\n");
    }
    if(!disp) {
        backend[0] = 'f'; backend[1] = 'b'; backend[2] = 0;
        disp = e1_fb_create("/dev/fb0");
        if(disp) g_display_frame = e1_fb_frame();
    }
    if(!disp) {
        fprintf(stderr, "display init failed\n");
        return 1;
    }

    if(e1_input_init("/dev/input/event0") != 0) {
        fprintf(stderr, "touch init failed\n");
        return 1;
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, e1_input_read);
    lv_indev_set_display(indev, disp);

    keys_init();
    hp_init();
    led_init();
    motor_init();
    audio_init();

    pages_build();
    set_backlight(200);
    cpu_policy();

    int refresh_ms = 33;
    FILE *rf = fopen("/mnt/sdcard/e1hwtest/refresh_ms", "r");
    if(rf) {
        int v = 0;
        if(fscanf(rf, "%d", &v) == 1 && v >= 15 && v <= 100) refresh_ms = v;
        fclose(rf);
    }

    lv_timer_t *ui = lv_timer_create(ui_timer, (uint32_t)refresh_ms, NULL);
    lv_timer_t *rt = lv_display_get_refr_timer(disp);
    if(rt) lv_timer_set_period(rt, (uint32_t)refresh_ms);
    lv_timer_t *it = lv_indev_get_read_timer(indev);
    if(it) lv_timer_set_period(it, (uint32_t)refresh_ms);
    (void)ui;

    printf("[main] e1hwtest up (ZhiyinOS hardware test), backend=%s refresh=%dms\n",
           backend, refresh_ms);
    printf("[main] hp=%s\n", hp_inserted() ? "IN" : "OUT");

    for(;;) {
        e1_drm_poll();
        keys_poll();
        hp_poll();
        audio_poll();
        motor_poll();

        uint32_t wait = lv_timer_handler();
        if(wait == LV_NO_TIMER_READY) wait = 5;
        if(wait > 15) wait = 15;
        usleep(wait * 1000);
    }
    return 0;
}
