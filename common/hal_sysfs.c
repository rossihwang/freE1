#include "hal_sysfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static void wr(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if(fd < 0) return;
    if(write(fd, val, strlen(val)) < 0) { /* ignore */ }
    close(fd);
}

static void wr_int(const char *path, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d\n", v);
    wr(path, b);
}

static int rd_int(const char *path)
{
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    int v = -1;
    if(fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ---------------- LED ---------------- */
static bool g_top_on;
static int  g_color = 1, g_pwm = 127;
static bool g_key_on;

void hs_led_top(bool on)  { g_top_on = on; wr_int("/proc/awled_enable", on ? 1 : 0); }
void hs_led_color(int c)  { if(c < 0) c = 0; if(c > 6) c = 6; g_color = c; wr_int("/proc/awled_color", c); }
void hs_led_pwm(int v)    { if(v < 0) v = 0; if(v > 255) v = 255; g_pwm = v; wr_int("/proc/awled_pwm", v); }
int  hs_led_color_get(void) { return g_color; }
int  hs_led_pwm_get(void)   { return g_pwm; }
bool hs_led_top_get(void)   { return g_top_on; }

void hs_led_key(bool on)  { g_key_on = on; wr_int("/proc/leds_gpio", on ? 1 : 0); }
bool hs_led_key_get(void) { return g_key_on; }

/* ---------------- 马达 ---------------- */
typedef struct { int on; int ms; } step_t;

static const step_t SEQ_SHORT[]  = { {1,120}, {0,0} };
static const step_t SEQ_DOUBLE[] = { {1,100}, {0,80}, {1,100}, {0,0} };
static const step_t SEQ_LONG[]   = { {1,700}, {0,0} };

static const step_t *g_seq;
static int g_step;
static uint32_t g_next;
static bool g_busy;
static int g_done;

void hs_motor(bool on) { wr("/proc/motor_vibr", on ? "1\n" : "0\n"); }

void hs_motor_reset(void) { hs_motor(false); g_seq = NULL; g_busy = false; g_step = 0; g_done = 0; g_next = 0; }
int  hs_motor_done(void)  { return g_done; }
bool hs_motor_busy(void)  { return g_busy; }

void hs_motor_pattern(int kind)
{
    const step_t *s = (kind == 1) ? SEQ_DOUBLE : (kind == 2 ? SEQ_LONG : SEQ_SHORT);
    g_seq = s; g_step = 0; g_busy = true;
    hs_motor(s[0].on);
    g_next = now_ms() + (uint32_t)s[0].ms;
}

void hs_motor_poll(void)
{
    if(!g_busy || !g_seq) return;
    if((int32_t)(now_ms() - g_next) < 0) return;
    g_step++;
    const step_t *st = &g_seq[g_step];
    hs_motor(st->on);
    if(st->ms == 0 && !st->on) {
        g_busy = false; g_seq = NULL; g_done++;
        return;
    }
    g_next = now_ms() + (uint32_t)st->ms;
}

/* ---------------- 背光 ---------------- */
int hs_backlight_set(int v)
{
    if(v < 0) v = 0;
    if(v > 255) v = 255;
    wr_int("/sys/class/backlight/backlight/brightness", v);
    return hs_backlight_get();
}
int hs_backlight_get(void) { return rd_int("/sys/class/backlight/backlight/brightness"); }

/* ---------------- 耳机 ---------------- */
int hs_hp_inserted(void) { return rd_int("/proc/headset_inserted"); }

/* ---------------- 电池 ---------------- */
int hs_battery_capacity(void)    { return rd_int("/sys/class/power_supply/battery/capacity"); }
int hs_battery_voltage_uv(void)  { return rd_int("/sys/class/power_supply/battery/voltage_now"); }

int hs_battery_charging(void)
{
    FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
    if(!f) return 0;
    char b[32] = {0};
    if(!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    return (strstr(b, "Charging") || strstr(b, "Full")) ? 1 : 0;
}

int hs_usb_online(void)
{
    int v = rd_int("/sys/class/power_supply/usb/online");
    if(v < 0) v = rd_int("/sys/class/power_supply/ac/online");
    return v > 0 ? 1 : 0;
}

/* ---------------- CPU ---------------- */
void hs_cpu_max_freq(int khz) { wr_int("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", khz); }
