/* e1_input.c - raw evdev reader for Sogou E1 (st16xx, MT protocol B) */

#include "input.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input.h>

#define MAXP 2
#define SWIPE_PX 60
#define SWIPE_DX_MAX 120

typedef struct {
    int active;
    int id;
    int x;
    int y;
} contact_t;

static int g_fd = -1;
static int g_slot = 0;
static int g_btn = 0;
static contact_t g_c[MAXP];

static int g_gesture = 0;     /* gesture in progress: suppress pointer */
static int g_pending = 0;     /* swipe consumed by UI */
static int g_track = 0;       /* two-finger tracking armed */
static int g_y0[MAXP];
static int g_x0[MAXP];

static e1_input_stats_t g_st = {160, 160, 0, 0, 0};
static int g_mask;

static int iabs(int v) { return v < 0 ? -v : v; }

static void stats_reset(void)
{
    g_st.max_contacts = 0;
    g_st.cells = 0;
}

void e1_input_reset_stats(void)
{
    stats_reset();
    g_st.max_contacts = 0;
    g_st.cells = 0;
    g_mask = 0;
}

int e1_input_cell_mask(void)
{
    return g_mask;
}

int e1_input_init(const char *dev)
{
    g_fd = open(dev, O_RDONLY | O_NONBLOCK);
    if(g_fd < 0) {
        perror("open input");
        return -1;
    }
    memset(g_c, 0, sizeof(g_c));
    return 0;
}

void e1_input_close(void)
{
    if(g_fd >= 0) close(g_fd);
    g_fd = -1;
}

const e1_input_stats_t *e1_input_stats(void)
{
    return &g_st;
}

int e1_input_take_swipe(void)
{
    int s = g_pending;
    g_pending = 0;
    return s;
}

static int active_count(void)
{
    int n = 0;
    for(int i = 0; i < MAXP; i++) if(g_c[i].active) n++;
    return n;
}

static void cover(int x, int y)
{
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x > 319) x = 319;
    if(y > 319) y = 319;
    int cx = x * 8 / 320;
    int cy = y * 8 / 320;
    int bit = cy * 8 + cx;
    g_mask |= (1 << bit);
    int n = 0;
    for(int i = 0; i < 64; i++) if(g_mask & (1 << i)) n++;
    g_st.cells = n;
}

static void on_frame(void)
{
    int n = active_count();

    if(n > g_st.max_contacts) g_st.max_contacts = n;
    g_st.contacts = n;

    if(n > 0) {
        int idx = g_c[0].active ? 0 : 1;
        g_st.x = g_c[idx].x;
        g_st.y = g_c[idx].y;
        cover(g_st.x, g_st.y);
    }

    /* two-finger vertical swipe */
    if(!g_gesture) {
        if(n >= 2) {
            if(!g_track) {
                g_track = 1;
                for(int i = 0; i < MAXP; i++) { g_y0[i] = g_c[i].y; g_x0[i] = g_c[i].x; }
            }
            else {
                int dy0 = g_c[0].y - g_y0[0];
                int dy1 = g_c[1].y - g_y0[1];
                int dx0 = g_c[0].x - g_x0[0];
                int dx1 = g_c[1].x - g_x0[1];
                if(iabs(dy0) > SWIPE_PX && iabs(dy1) > SWIPE_PX &&
                   ((dy0 > 0) == (dy1 > 0)) &&
                   iabs(dx0) < SWIPE_DX_MAX && iabs(dx1) < SWIPE_DX_MAX) {
                    g_gesture = 1;
                    g_pending = (dy0 < 0) ? 1 : -1;   /* up = next */
                    printf("[input] two-finger swipe %s\n", g_pending > 0 ? "UP(next)" : "DOWN(prev)");
                }
            }
        }
        else {
            g_track = 0;
        }
    }
    if(n == 0) {
        g_gesture = 0;
        g_track = 0;
    }
}

void e1_input_poll(void)
{
    if(g_fd < 0) return;

    struct input_event ev;
    while(read(g_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        switch(ev.type) {
            case EV_ABS:
                switch(ev.code) {
                    case ABS_MT_SLOT:
                        if(ev.value >= 0 && ev.value < MAXP) g_slot = ev.value;
                        break;
                    case ABS_MT_TRACKING_ID:
                        if(ev.value == -1) {
                            g_c[g_slot].active = 0;
                        }
                        else {
                            g_c[g_slot].active = 1;
                            g_c[g_slot].id = ev.value;
                        }
                        break;
                    case ABS_MT_POSITION_X:
                        g_c[g_slot].x = ev.value;
                        break;
                    case ABS_MT_POSITION_Y:
                        g_c[g_slot].y = ev.value;
                        break;
                    default:
                        break;
                }
                break;
            case EV_KEY:
                if(ev.code == BTN_TOUCH) {
                    g_btn = ev.value;
                    if(!g_btn && active_count() == 0) {
                        /* nothing */
                    }
                }
                break;
            case EV_SYN:
                if(ev.code == SYN_REPORT) on_frame();
                break;
            default:
                break;
        }
    }
}

void e1_input_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    e1_input_poll();

    /* A two-finger gesture owns the touch: keep LVGL released so the
     * gesture cannot click any widget underneath. */
    if(g_st.contacts == 0 || g_gesture) {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    else {
        data->state = LV_INDEV_STATE_PRESSED;
    }
    data->point.x = g_st.x;
    data->point.y = g_st.y;
}
