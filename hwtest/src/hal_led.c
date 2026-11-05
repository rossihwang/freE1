#include "hal_led.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static bool g_top;
static int g_color = 1;
static int g_pwm = 127;
static bool g_key;

static void wfile(const char *path, int v)
{
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d\n", v);
    int fd = open(path, O_WRONLY);
    if(fd < 0) {
        perror(path);
        return;
    }
    if(write(fd, buf, (size_t)n) < 0) perror(path);
    close(fd);
}

void led_top(bool on)
{
    g_top = on;
    wfile("/proc/awled_enable", on ? 1 : 0);
}

void led_color(int c)
{
    if(c < 0) c = 0;
    if(c > 6) c = 6;
    g_color = c;
    wfile("/proc/awled_color", c);
}

void led_pwm(int v)
{
    if(v < 0) v = 0;
    if(v > 255) v = 255;
    g_pwm = v;
    wfile("/proc/awled_pwm", v);
}

void led_key(bool on)
{
    g_key = on;
    wfile("/proc/leds_gpio", on ? 1 : 0);
}

int  led_color_get(void) { return g_color; }
int  led_pwm_get(void)   { return g_pwm; }
bool led_top_get(void)   { return g_top; }
bool led_key_get(void)   { return g_key; }

void led_init(void)
{
    /* start from a known state: green, medium brightness */
    led_pwm(127);
    led_color(2);
    led_top(true);
    led_key(false);
}
