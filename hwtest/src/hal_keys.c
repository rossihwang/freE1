#include "hal_keys.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>

typedef struct {
    bool down;
    int count;
    uint32_t down_ms;
    uint32_t last_hold;
} key_state_t;

static int g_fd3 = -1;   /* adc-keys: KEY_1..KEY_5 */
static int g_fd1 = -1;   /* rk8xx_pwrkey: KEY_6 */
static key_state_t g_k[E1K_COUNT];

static const char *g_names[E1K_COUNT] = { "MARK", "STOP", "REC", "VOL+", "VOL-", "POWER" };

uint32_t e1_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void key_event(int code, int value)
{
    if(code < KEY_1 || code > KEY_6) return;
    int idx = code - KEY_1;     /* 2..7 -> 0..5 */
    if(idx < 0 || idx >= E1K_COUNT) return;

    if(value == 1) {            /* press (value 2 = autorepeat, ignored) */
        if(!g_k[idx].down) {
            g_k[idx].down = true;
            g_k[idx].down_ms = e1_now_ms();
            g_k[idx].count++;
        }
    }
    else if(value == 0) {
        if(g_k[idx].down) {
            g_k[idx].down = false;
            g_k[idx].last_hold = e1_now_ms() - g_k[idx].down_ms;
        }
    }
}

static void drain(int fd)
{
    if(fd < 0) return;
    struct input_event ev;
    while(read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if(ev.type == EV_KEY) key_event(ev.code, ev.value);
    }
}

void keys_init(void)
{
    memset(g_k, 0, sizeof(g_k));
    g_fd3 = open("/dev/input/event3", O_RDONLY | O_NONBLOCK);
    g_fd1 = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if(g_fd3 < 0) perror("keys: event3");
    if(g_fd1 < 0) perror("keys: event1");
}

void keys_poll(void)
{
    drain(g_fd3);
    drain(g_fd1);
}

bool keys_down(e1_key_t k)
{
    return k < E1K_COUNT ? g_k[k].down : false;
}

int keys_count(e1_key_t k)
{
    return k < E1K_COUNT ? g_k[k].count : 0;
}

uint32_t keys_hold_ms(e1_key_t k)
{
    if(k >= E1K_COUNT) return 0;
    return g_k[k].down ? (e1_now_ms() - g_k[k].down_ms) : 0;
}

uint32_t keys_last_hold(e1_key_t k)
{
    return k < E1K_COUNT ? g_k[k].last_hold : 0;
}

const char *keys_name(e1_key_t k)
{
    return k < E1K_COUNT ? g_names[k] : "?";
}

void keys_reset(void)
{
    for(int i = 0; i < E1K_COUNT; i++) {
        g_k[i].count = 0;
        g_k[i].last_hold = 0;
    }
}
