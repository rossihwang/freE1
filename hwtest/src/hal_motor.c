#include "hal_motor.h"
#include "hal_keys.h"   /* e1_now_ms */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
    int on;
    int ms;
} step_t;

static const step_t SEQ_SHORT[] = { {1, 120}, {0, 0} };
static const step_t SEQ_DOUBLE[] = { {1, 100}, {0, 80}, {1, 100}, {0, 0} };
static const step_t SEQ_LONG[] = { {1, 700}, {0, 0} };

static const step_t *g_seq;
static int g_step;
static uint32_t g_next;
static bool g_busy;
static int g_done;

static void wv(int on)
{
    int fd = open("/proc/motor_vibr", O_WRONLY);
    if(fd < 0) {
        perror("motor_vibr");
        return;
    }
    if(write(fd, on ? "1\n" : "0\n", 2) < 0) perror("motor_vibr");
    close(fd);
    if(!on) {
        /* keep motor_time at 0 so it can never inhibit the next pulse */
        int fd2 = open("/proc/motor_time", O_WRONLY);
        if(fd2 >= 0) {
            if(write(fd2, "0\n", 2) < 0) {}
            close(fd2);
        }
    }
}

void motor_init(void)
{
    wv(0);
}

void motor_pulse(int on_ms)
{
    wv(1);
    g_seq = NULL;
    g_step = 0;
    g_next = e1_now_ms() + (uint32_t)on_ms;
    g_busy = true;
    g_done++;
}

void motor_pattern(int kind)
{
    switch(kind) {
        case 0: g_seq = SEQ_SHORT; break;
        case 1: g_seq = SEQ_DOUBLE; break;
        default: g_seq = SEQ_LONG; break;
    }
    g_step = 0;
    g_busy = true;
    g_next = 0;         /* apply first step on next poll */
    g_done++;
}

void motor_poll(void)
{
    if(!g_busy) return;
    uint32_t now = e1_now_ms();
    if((int32_t)(now - g_next) < 0) return;

    if(g_seq) {
        const step_t *s = &g_seq[g_step];
        if(s->ms == 0) {
            wv(0);
            g_busy = false;
            g_seq = NULL;
            return;
        }
        wv(s->on);
        g_next = now + (uint32_t)s->ms;
        g_step++;
    }
    else {
        wv(0);
        g_busy = false;
    }
}

bool motor_busy(void) { return g_busy; }
int  motor_patterns_done(void) { return g_done; }
void motor_reset(void) { g_done = 0; }
