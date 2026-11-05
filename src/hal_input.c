#include "hal.h"
#include "lvgl.h"
#include "common.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <linux/input.h>

/* st16xx 只报 MT-B（无 ABS_X/Y），这里自己做槽位跟踪 */
static int g_tfd = -1, g_kfd1 = -1, g_kfd2 = -1;
static int g_slot = 0, g_id[10];
static int g_x[10] = {0}, g_y[10] = {0};
static int g_cur = 0, g_was_down = 0;
static int g_dx0 = 0, g_dy0 = 0;
static void (*g_swipe_cb)(int dx, int dy) = NULL;
static int g_touch_enabled = 1;
static int g_maxd = 0;        /* 本次按下期间的最大位移 */
static int g_last_drag = 0;   /* 上一次手势是否算拖动（用来抑制误点击） */
int hal_input_last_drag(void){ return g_last_drag; }
void hal_input_last_xy(int *x, int *y){ if(x) *x=g_x[g_cur]; if(y) *y=g_y[g_cur]; }
static void (*g_touch_act_cb)(void) = NULL;
void hal_set_touch_act_cb(void (*cb)(void)){ g_touch_act_cb = cb; }
void hal_set_touch_enabled(int on){ g_touch_enabled = on; if(!on){ for(int i=0;i<10;i++) g_id[i] = -1; g_was_down = 0; } }
void hal_set_swipe_cb(void (*cb)(int,int)){ g_swipe_cb = cb; }

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data){
    (void)indev;
    struct input_event ev;
    if(!g_touch_enabled){                 /* 息屏：吞掉所有触摸事件，防止误触发 */
        int n=0;
        while(g_tfd >= 0 && read(g_tfd, &ev, sizeof ev) == (ssize_t)sizeof ev) n++;
        if(n){ static int64_t last=0; if(mono_ms()-last>2000){ last=mono_ms(); logmsg("touch: dropped %d ev (disabled)", n); } }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    while(g_tfd >= 0 && read(g_tfd, &ev, sizeof ev) == (ssize_t)sizeof ev){
        if(ev.type == EV_ABS){
            switch(ev.code){
            case ABS_MT_SLOT:      g_slot = ev.value; if(g_slot<0)g_slot=0; if(g_slot>9)g_slot=9; break;
            case ABS_MT_TRACKING_ID:
                g_id[g_slot] = ev.value;
                if(ev.value >= 0) g_cur = g_slot;
                break;
            case ABS_MT_POSITION_X: g_x[g_slot] = ev.value; break;
            case ABS_MT_POSITION_Y: g_y[g_slot] = ev.value; break;
            default: break;
            }
        } else if(ev.type == EV_SYN && ev.code == SYN_REPORT){
            /* 有触点即按下；抬到 0 个触点即松开 */
        }
    }
    int any = 0;
    for(int i=0;i<10;i++) if(g_id[i] >= 0) any++;
    if(any && g_touch_act_cb) g_touch_act_cb();   /* 任何触摸都重置息屏计时 */
    if(any && !g_was_down){ g_dx0 = g_x[g_cur]; g_dy0 = g_y[g_cur]; g_maxd = 0; g_last_drag = 0; }
    if(any){
        int dx = g_x[g_cur]-g_dx0, dy = g_y[g_cur]-g_dy0;
        int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
        int d = adx>ady? adx : ady;
        if(d > g_maxd) g_maxd = d;
    }
    if(!any && g_was_down){ g_last_drag = (g_maxd > 10); }
    if(!any && g_was_down && g_swipe_cb){
        int dx = g_x[g_cur] - g_dx0, dy = g_y[g_cur] - g_dy0;
        int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
        if(adx >= 22 && adx >= ady) g_swipe_cb(dx, dy);
    }
    g_was_down = any;
    if(any){
        data->point.x = g_x[g_cur]; data->point.y = g_y[g_cur];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = g_x[g_cur]; data->point.y = g_y[g_cur];
    }
}

static void (*g_key_cb)(int,int);
void hal_set_key_cb(void (*cb)(int,int)){ g_key_cb=cb; }

static void key_timer_cb(lv_timer_t *t){
    (void)t;
    struct input_event ev;
    for(int k=0;k<2;k++){
        int fd = k? g_kfd2 : g_kfd1;
        while(fd>=0 && read(fd,&ev,sizeof ev)==(ssize_t)sizeof ev){
            if(ev.type==EV_KEY && g_key_cb) g_key_cb(ev.code, ev.value);
        }
    }
}

int hal_input_get_enabled(void){ return g_touch_enabled; }

int hal_input_init(void){
    for(int i=0;i<10;i++) g_id[i] = -1;   /* MT-B: tracking_id < 0 == 无触点 */
    g_tfd  = open("/dev/input/event0", O_RDONLY|O_NONBLOCK);
    g_kfd1 = open("/dev/input/event3", O_RDONLY|O_NONBLOCK);  /* KEY_1..5 */
    g_kfd2 = open("/dev/input/event1", O_RDONLY|O_NONBLOCK);  /* KEY_6    */
    if(g_tfd < 0) logmsg("touch event0 open failed");
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_read);
    lv_timer_create(key_timer_cb, 50, NULL);
    return 0;
}
