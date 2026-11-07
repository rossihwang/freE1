#include "lvgl.h"
#include "ui.h"
#include "common.h"
#ifndef FREEL1_NO_PNG
#include "png.h"
#endif
#include <unistd.h>
#include <math.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include "engine.h"
#include "http.h"
#include "wifi_ui.h"
#include "audio_play.h"
#include "monitor.h"
#include "wifi.h"
#include "hal_sysfs.h"
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
int hal_input_get_enabled(void);
int hal_input_last_drag(void);
void wifi_t9_press(int k);
int  wifi_overlay_open(void);

#define W 320
#define H 320
#define C_BG      0x05060A
#define C_PANEL   0x0D0F14
#define C_LINE    0x1E222B
#define C_TEXT    0xE8E8E9
#define C_DIM     0x6B7280
#define C_GREEN   0x3CFF7A
#define C_RED     0xFF3B30
#define REC_SIZE  88

static void ui_activity(void);
static void show_warn(const char *msg);
static int g_last_key_code = -1;
static int64_t g_last_activity;
static int64_t g_start_ms;
static void guard_load(void);
static int wlan_ip(char *out, size_t n);
static void pwr_relay_cb(lv_timer_t *t);
static int64_t g_ignore_pwr_until;
static void files_refresh(void);
static void led_set(int on);
static void screen_set(int on);
static void motor_pulse(void);


#define NPAGES 3
static lv_obj_t *pages[NPAGES];
#define MAXSTACK 8
static lv_obj_t *g_stack[MAXSTACK];
static int g_depth = 0;
static lv_obj_t *timer_lbl, *state_lbl, *dbfs_lbl, *mode_lbl;
static lv_obj_t *g_mode_btn;      /* 拾音模式胶囊（原始 8 通道采集期间要禁用） */
static lv_obj_t *batt_lbl, *stor_lbl, *files_hdr, *clock_lbl;
static lv_obj_t *viz_omni, *viz_top;
static lv_obj_t *bars[6];
static lv_obj_t *bars_top[2];
/* 8 通道原始采集状态（麦克风阵列几何测量），见 geo_start() */
static pid_t   g_geo_pid = -1;
static int     g_geo_on;
static int64_t g_geo_t0;
static int64_t g_geo_stop_arm;   /* 非 0 = 已经按过一次停止，等第二次确认 */
static int g_paused;             /* 录音暂停镜像（engine 侧 st->paused 的副本） */

static lv_obj_t *mk_panel(lv_obj_t *parent,int x,int y,int w,int h,int r,uint32_t bg);
static lv_obj_t *mk_hbar(lv_obj_t *parent,int x,int y,int w,int h);

/* 电平条 + 峰值保持标记（amber 竖线） */
typedef struct { lv_obj_t *bar, *mark; int x,y,w,h,max; } meter_t;
static meter_t meters[8];

static void mark_place(meter_t *m){
    if(!m->mark) return;
    int px = m->x + (m->w-3)*m->max/100;
    lv_obj_set_pos(m->mark, px, m->y-2);
}
static meter_t *mk_meter(lv_obj_t *parent,int x,int y,int w,int h){
    int idx=0; for(;idx<8;idx++) if(!meters[idx].bar) break;
    if(idx>=8) return NULL;
    meter_t *m=&meters[idx];
    m->x=x; m->y=y; m->w=w; m->h=h; m->max=0;
    m->bar=mk_hbar(parent,x,y,w,h);
    m->mark=mk_panel(parent,x,y-2,3,h+4,1,0xFFC53D);
    lv_obj_add_flag(m->mark,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m->mark,LV_OBJ_FLAG_CLICKABLE);
    return m;
}
static void meter_update(meter_t *m,int v){
    if(!m||!m->bar) return;
    if(v<0) v=0; if(v>100) v=100;
    lv_bar_set_value(m->bar,v,LV_ANIM_OFF);
    if(v>m->max){
        m->max=v;
        mark_place(m);
        lv_obj_remove_flag(m->mark,LV_OBJ_FLAG_HIDDEN);
    }
}
static void meters_reset(void){
    for(int i=0;i<8;i++){
        if(!meters[i].bar) continue;
        meters[i].max=0;
        if(meters[i].mark) lv_obj_add_flag(meters[i].mark,LV_OBJ_FLAG_HIDDEN);
    }
}
static int64_t g_ignore_tap_until = 0;   /* 弹层刚关：这段时间内的点击一律忽略 */
static int tap_ok(void){ return !hal_input_last_drag() && mono_ms() > g_ignore_tap_until; }
static void tap_guard(void){ g_ignore_tap_until = mono_ms() + 400; }
static void reset_cb(lv_event_t *e){ (void)e; if(!tap_ok()) return; ui_activity(); meters_reset(); }

static lv_obj_t *chart, *ser1, *ser2;
static lv_obj_t *rec_btn, *rec_lbl;
static int recording = 0, page_idx = 0, input_mode = 0; /* 0=TOP 1=OMNI */
static uint32_t rec_sec = 0;
static char g_dir[400] = "recordings";
static int  g_device = 0;
static lv_obj_t *files_box;
static lv_obj_t *settings_box;
static int g_monitor_on = 0;
static lv_obj_t *monitor_btn = NULL, *monitor_lbl = NULL;
static lv_obj_t *storage_val, *http_val, *perf_val, *fmt_val, *adb_val;
static int g_adb_on = -1;

static int adb_running(void){
    DIR *d = opendir("/proc");
    if(!d) return 0;
    int found = 0;
    struct dirent *e;
    while((e = readdir(d))){
        if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64]; snprintf(p,sizeof p,"/proc/%s/comm", e->d_name);
        FILE *f = fopen(p,"r"); if(!f) continue;
        char nm[32] = {0};
        if(fgets(nm,sizeof nm,f) && !strncmp(nm,"adbd",4)) found = 1;
        fclose(f);
        if(found) break;
    }
    closedir(d);
    return found;
}

static void adb_set_value(int on){
    if(!adb_val) return;
    lv_label_set_text(adb_val, on ? "ON" : "OFF");
    lv_obj_set_style_text_color(adb_val, lv_color_hex(on ? C_GREEN : C_DIM), 0);
}

static void adb_cb(lv_event_t *e){
    (void)e;
    if(!tap_ok()) return;
    int on = adb_running();
    if(on){ system("killall -9 adbd >/dev/null 2>&1"); logmsg("adb: stopped"); }
    else  { system("/usr/bin/adbd >/dev/null 2>&1 &"); logmsg("adb: started"); }
    usleep(300000);
    g_adb_on = adb_running();
    adb_set_value(g_adb_on);
}
static int g_perf_on = 0;

static void set_perf(int on){
    g_perf_on = on;
    FILE *f=fopen("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq","w");
    if(f){ fprintf(f, on?"1296000\n":"816000\n"); fclose(f); }
    if(perf_val){
        lv_label_set_text(perf_val, on?"ON":"OFF");
        lv_obj_set_style_text_color(perf_val, lv_color_hex(on?C_GREEN:C_DIM), 0);
    }
    logmsg("performance mode: %s", on?"ON":"OFF");
}
static void perf_cb(lv_event_t *e){ (void)e; if(!tap_ok()) return; set_perf(!g_perf_on); }
static lv_obj_t *http_row_val, *storage_val;
static int  g_http_port = 0;
static int  g_fmt = 0;          /* 0 = 48k/24bit, 1 = 44.1k/16bit */
/* 第二档只降位深、不降采样率：44.1 kHz 下 PDM 阵列的 ch6/ch7 恒为 0
   （实测 48k 全部 8 路正常，44.1k 只有 ch0-5 有数据），定向模式会录成静音，
   所以不提供 44.1k。 */
static const int FMT_RATE[2] = {48000, 48000};
static const int FMT_BITS[2] = {24, 16};
static int  g_idle_min = 60;     /* 空闲多少秒后熄屏：30/60/90 */
static double g_guard_free_gb = 0.2;  /* 满盘阈值（可被 guard.conf 覆盖，便于测试） */
static int    g_guard_batt    = 5;   /* 低电阈值 */
static int    g_batt_level_shown = 0;  /* 已提示到哪一级：20/10/5 */
static int    g_guard_ignore_chg = 0; /* 测试用：忽略充电状态 */
static lv_obj_t *idle_val = NULL;
#define IDLE_OPTS 3
static const int IDLE_VALS[IDLE_OPTS] = {30, 60, 90};

static int wlan_ip(char *out, size_t n){
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "wlan0");
    int rc = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    if(rc != 0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in*)&ifr.ifr_addr;
    return inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)n) ? 0 : -1;
}

static void guard_load(void){
    char path[512]; snprintf(path,sizeof path,"%s/guard.conf", g_dir);
    FILE *f=fopen(path,"r"); if(!f) return;
    char line[128];
    while(fgets(line,sizeof line,f)){
        double d; int i;
        if(sscanf(line,"free_gb=%lf",&d)==1) g_guard_free_gb = d;
        else if(sscanf(line,"batt_pct=%d",&i)==1) g_guard_batt = i;
        else if(sscanf(line,"ignore_charging=%d",&i)==1) g_guard_ignore_chg = i;
    }
    fclose(f);
    logmsg("guard: free<%.2f GB, batt<%d%%", g_guard_free_gb, g_guard_batt);
}

static void fmt_save(void){
    char path[512]; snprintf(path,sizeof path,"%s/format", g_dir);
    FILE *f=fopen(path,"w"); if(f){ fprintf(f,"%d\n", g_fmt); fclose(f); }
}
static void fmt_load(void){
    char path[512]; snprintf(path,sizeof path,"%s/format", g_dir);
    FILE *f=fopen(path,"r"); if(!f) return;
    int v=0; if(fscanf(f,"%d",&v)==1 && (v==0||v==1)) g_fmt=v;
    fclose(f);
}
static void mode_save(void){
    char path[512]; snprintf(path,sizeof path,"%s/input_mode", g_dir);
    FILE *f=fopen(path,"w"); if(f){ fprintf(f,"%d\n", input_mode); fclose(f); fflush(f); }
}
static void mode_load(void){
    char path[512]; snprintf(path,sizeof path,"%s/input_mode", g_dir);
    FILE *f=fopen(path,"r"); if(!f) return;
    int v=0; if(fscanf(f,"%d",&v)==1 && (v==0||v==1)) input_mode=v;
    fclose(f);
}

static void fmt_cb(lv_event_t *e){
    (void)e;
    if(!tap_ok()) return;
    if(engine_running()){ show_warn("Stop recording before changing format."); return; }
    g_fmt = !g_fmt;
    if(fmt_val) lv_label_set_text(fmt_val, g_fmt? "48k / 16bit" : "48k / 24bit");
    fmt_save();
    logmsg("format -> %s", g_fmt? "48k/16bit" : "48k/24bit");
}

static void idle_save(void){
    char path[512]; snprintf(path,sizeof path,"%s/screen_off_sec", g_dir);
    FILE *f=fopen(path,"w"); if(f){ fprintf(f,"%d\n", g_idle_min); fclose(f); }
}
static void idle_load(void){
    char path[512]; snprintf(path,sizeof path,"%s/screen_off_sec", g_dir);
    FILE *f=fopen(path,"r"); if(!f) return;
    int v=0; if(fscanf(f,"%d",&v)==1){ for(int i=0;i<IDLE_OPTS;i++) if(IDLE_VALS[i]==v) g_idle_min=v; }
    fclose(f);
}
static void idle_cb(lv_event_t *e){
    (void)e;
    if(!tap_ok()) return;
    int idx=0; for(int i=0;i<IDLE_OPTS;i++) if(IDLE_VALS[i]==g_idle_min) idx=i;
    g_idle_min = IDLE_VALS[(idx+1)%IDLE_OPTS];
    idle_save();
    if(idle_val){ char b[16]; snprintf(b,sizeof b,"%d s",g_idle_min); lv_label_set_text(idle_val,b); }
    logmsg("screen off timeout -> %d s", g_idle_min);
}
static int  g_sleep_after = 180; /* 熄屏后再多少秒进入休眠 */
static int  g_idle_handled = 0;  /* 本次空闲周期是否已处理 */
static int64_t g_off_at = 0;     /* 熄屏时刻（0 = 未熄屏） */
static int64_t g_wake_at = 0;    /* 上次唤醒的时间（用于重置 idle 周期） */
static int g_shots = 0;

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color){
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    if(font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}
static lv_obj_t *mk_panel(lv_obj_t *parent, int x,int y,int w,int h,int r,uint32_t bg){
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, r, 0);
    return o;
}
static lv_obj_t *mk_hbar(lv_obj_t *parent,int x,int y,int w,int h){
    lv_obj_t *b=lv_bar_create(parent);
    lv_obj_set_size(b,w,h); lv_obj_set_pos(b,x,y);
    lv_bar_set_range(b,0,100); lv_bar_set_value(b,0,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_LINE),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_radius(b,w/2,LV_PART_MAIN);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_GREEN),LV_PART_INDICATOR);
    lv_obj_set_style_radius(b,h/2,LV_PART_INDICATOR);
    lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static void apply_mode(void){
    if(input_mode==0){
        lv_obj_add_flag(viz_omni,LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(viz_top,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(mode_lbl,"DIRECTIONAL");
        if(chart){ lv_obj_set_pos(chart,10,246); lv_obj_set_size(chart,300,36); }
        if(dbfs_lbl) lv_obj_set_pos(dbfs_lbl,10,286);
    } else {
        lv_obj_remove_flag(viz_omni,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(viz_top,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(mode_lbl,"OMNI");
        if(chart){ lv_obj_set_pos(chart,10,246); lv_obj_set_size(chart,300,36); }
        if(dbfs_lbl) lv_obj_set_pos(dbfs_lbl,10,286);
    }
}

static void no_scroll_recursive(lv_obj_t *o){
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    uint32_t n = lv_obj_get_child_count(o);
    for(uint32_t i=0;i<n;i++) no_scroll_recursive(lv_obj_get_child(o,(int32_t)i));
}

static void show_page(int p);
static int64_t g_hw_reclaim_at;
static char g_play_file[192];
static lv_obj_t *g_row_fill = NULL;      /* 播放中那一行的背景进度条 */
static int g_row_fill_pct = -1;
#define ROW_W     304
static void play_file(const char *name);
static void files_progress_update(const play_state_t *ps);
static lv_group_t *g_group;
static void key_cb(lv_event_t *e){
    uint32_t k = lv_event_get_key(e);
    if(k==LV_KEY_RIGHT)      show_page((page_idx+1)%NPAGES);
    else if(k==LV_KEY_LEFT)  show_page((page_idx+2)%NPAGES);
}


/* ---- 子页栈：设置类子页 push 进来，`<` 回到底下的页面 ---- */
static void ui_pop_page(void);
static void ui_pop_page(void){
    if(g_depth<=0) return;
    lv_obj_t *top=g_stack[--g_depth];
    if(top) lv_obj_delete(top);
    if(page_idx < NPAGES) lv_obj_remove_flag(pages[page_idx], LV_OBJ_FLAG_HIDDEN);
}
static void pop_cb(lv_event_t *e){ (void)e; ui_pop_page(); }
static void ui_push_page(lv_obj_t *p){
    if(g_depth>=MAXSTACK){ lv_obj_delete(p); return; }
    g_stack[g_depth++]=p;
    if(page_idx < NPAGES) lv_obj_add_flag(pages[page_idx], LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *b=lv_button_create(p);
    lv_obj_set_size(b,48,34); lv_obj_set_pos(b,6,2);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_radius(b,13,0);
    lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_set_style_border_color(b,lv_color_hex(C_LINE),0);
    lv_obj_t *l=mk_label(b,"<",&lv_font_montserrat_14,C_GREEN);
    lv_obj_center(l);
    lv_obj_add_event_cb(b,pop_cb,LV_EVENT_CLICKED,NULL);
}
static void back_cb(lv_event_t *e){ (void)e; show_page(0); }
static void add_back_btn(lv_obj_t *p){
    lv_obj_t *b=lv_button_create(p);
    lv_obj_set_size(b,48,34); lv_obj_set_pos(b,6,2);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_radius(b,13,0);
    lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_set_style_border_color(b,lv_color_hex(C_LINE),0);
    lv_obj_t *l=mk_label(b,"<",&lv_font_montserrat_14,C_GREEN);
    lv_obj_center(l);
    lv_obj_add_event_cb(b,back_cb,LV_EVENT_CLICKED,NULL);
}
static void show_page(int p){
    logmsg("page -> %d (+%lld ms)", p, (long long)(mono_ms() - g_start_ms));
    if(p != 1 && play_is_active()){ play_stop(); g_play_file[0] = 0; logmsg("play: stopped (page change)"); }   /* 无底部栏：离开文件页 = 停止 */
    if(p != 0 && g_start_ms && (mono_ms() - g_start_ms) < 800){
        logmsg("page: ignored (startup window)");
        return;
    }
    page_idx = p;
    for(int i=0;i<NPAGES;i++){
        if(i==p) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if(p==1) files_refresh();
    if(g_group) lv_group_focus_obj(pages[p]);
}


static void ui_activity(void);   /* 定义在后面 */
static void set_rec_ui(int on){
    if(recording != on){ led_set(on); motor_pulse(); }
    recording = on;
    if(!on) g_paused = 0;
    /* 大按钮三态：待机 REC / 录音 PAUSE / 暂停 RESUME（停止交给 STOP 键或长按） */
    const char *bl = !on ? "REC" : (g_paused ? "RESUME" : "PAUSE");
    uint32_t bc = !on ? C_GREEN : (g_paused ? 0xFFC400 : C_RED);
    /* RESUME 比 PAUSE 宽，20 号字会顶出圆形按钮，暂停态降一号 */
    lv_obj_set_style_text_font(rec_lbl, g_paused ? &lv_font_montserrat_14 : &lv_font_montserrat_20, 0);
    lv_label_set_text(rec_lbl, bl);
    lv_obj_set_style_bg_color(rec_btn, lv_color_hex(bc), 0);
    lv_label_set_text(state_lbl, !on ? "STANDBY" : (g_paused ? "PAUSED" : "RECORDING"));
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(!on ? C_DIM : (g_paused ? 0xFFC400 : C_RED)), 0);
    if(!on){
        /* 停止后必须清掉上次录音的计时/电平/波形，否则屏幕看着像还在录 */
        rec_sec = 0;
        if(timer_lbl) lv_label_set_text(timer_lbl, "00:00:00");
        for(int i=0;i<8;i++){
            if(meters[i].bar){ lv_bar_set_value(meters[i].bar, 0, LV_ANIM_OFF); }
            if(meters[i].mark){ lv_obj_add_flag(meters[i].mark, LV_OBJ_FLAG_HIDDEN); }
        }
        if(chart){
            lv_chart_set_all_value(chart, ser1, 0);
            lv_chart_set_all_value(chart, ser2, 0);
            lv_chart_refresh(chart);
            lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);   /* 待机不留一条平线 */
        }
        if(dbfs_lbl) lv_label_set_text(dbfs_lbl, "-- dBFS");
        if(g_mode_btn){ lv_obj_add_flag(g_mode_btn, LV_OBJ_FLAG_CLICKABLE); apply_mode(); }
        /* 录音时屏幕早已自动熄灭，g_off_at 是很久以前的值；停止后不重置的话，
           下一个 tick 就会认为"熄屏已超过 180 s"而立即进入休眠（实测停录后 1 ms 就睡）。 */
        ui_activity();
        g_off_at = 0;
    } else if(chart){
        lv_obj_remove_flag(chart, LV_OBJ_FLAG_HIDDEN);
    }
}
/* 录音反馈：按键红灯 + 顶部 LED（红=录音/绿=待机）+ 马达短振 */
static void sysfs_write(const char *path, const char *val);
static int64_t g_last_activity = 0;
static int g_idle_handled_fwd;
static void ui_activity(void){ g_last_activity = mono_ms(); g_off_at = 0; g_idle_handled = 0; }

/* 深度休眠：只靠电源键唤醒（本机没有任何 wakeup 源注册）
   进入前必须：停录音、sync、记录时间 */
static void do_suspend(void){
    if(engine_running()) return;                 /* 录音中绝不睡 */
    if(g_geo_on) return;                         /* 原始采集进行中也不睡 */
    if(g_depth>0) return;                        /* 子页里不睡 */
    logmsg("suspend: going to sleep");
    hs_motor(false);          /* 别带着振动状态睡过去 */
    hs_led_key(false);
    hs_led_top(false);
    sync();
    FILE *f=fopen("/mnt/sdcard/freE1/suspend.log","a");
    if(f){ fprintf(f,"SLEEP %s\n", ctime((time_t[]){time(NULL)})); fclose(f); }
    f=fopen("/sys/power/state","w");
    if(f){ fputs("mem",f); fclose(f); }
    /* 醒来后：强制重置全部空闲状态（否则会立刻再睡） */
    screen_set(1);
    g_last_activity = mono_ms();
    g_off_at = 0;
    g_idle_handled = 0;
    g_last_activity = mono_ms();
    g_ignore_pwr_until = mono_ms() + 1500;   /* 吞掉唤醒那一次按键 */
    logmsg("suspend: resumed");

    /* 休眠期间 wpa_supplicant / DHCP 可能掉链，醒来后自检一次 */
    {
        char buf[256] = {0};
        FILE *pp = popen("/usr/sbin/wpa_cli -i wlan0 status 2>/dev/null", "r");
        if(pp){ size_t n = fread(buf,1,sizeof buf-1,pp); (void)n; pclose(pp); }
        int connected = strstr(buf, "wpa_state=COMPLETED") != NULL;
        int has_ip = strstr(buf, "ip_address=") != NULL;
        if(!connected || !has_ip){
            logmsg("resume: wifi down, restarting");
            system("/usr/sbin/wpa_supplicant -B -i wlan0 -c /data/cfg/wpa_supplicant.conf >/dev/null 2>&1");
            usleep(1500000);
            system("/sbin/udhcpc -i wlan0 -b -t 8 >/dev/null 2>&1 &");
        } else {
            logmsg("resume: wifi ok");
        }
    }
}

static int g_screen_on = 1;
static void screen_set(int on){
    g_screen_on = on;
    hs_backlight_set(on ? 200 : 0);
    hal_set_touch_enabled(on);           /* 息屏时不处理触摸，防止误触发 */
    hal_disp_set_flush_enabled(on);      /* 熄屏时不往 fb 刷，省电 */
    if(on) lv_obj_invalidate(lv_screen_active());   /* 点亮时整屏重画 */
}
static void sysfs_write(const char *path, const char *val){
    FILE *f=fopen(path,"w"); if(f){ fputs(val,f); fclose(f); }
}
static void led_set(int on){
    hs_led_key(on);
    hs_led_top(true);
    hs_led_color(on ? 1 : 2);        /* 1=红 2=绿 */
    hs_led_pwm(on ? 200 : 80);
}
static void motor_pulse(void){ hs_motor_pattern(0); }

static void monitor_refresh(void);

static void monitor_toggle(void){
    int hp = hs_hp_inserted();
    if(!g_monitor_on){
        if(hp != 1){
            show_warn("Plug in headphones to use monitor.");
            logmsg("monitor: refused (no headphone)");
            return;
        }
        if(!engine_running()){
            show_warn("Start recording to use monitor.");
            logmsg("monitor: refused (not recording)");
            return;
        }
        if(monitor_open(FMT_RATE[g_fmt]) != 0){ show_warn("Monitor failed to open audio."); return; }
        g_monitor_on = 1;
        logmsg("monitor: on");
    } else {
        g_monitor_on = 0;
        monitor_close();
        logmsg("monitor: off");
    }
    monitor_refresh();
}

static void monitor_refresh(void){
    if(!monitor_lbl) return;
    int hp = hs_hp_inserted();
    /* 颜色只表示"能不能监听"：插了耳机=绿，没插=灰；文字表示监听开没开 */
    lv_obj_set_style_text_color(monitor_lbl, lv_color_hex(hp == 1 ? C_GREEN : C_DIM), 0);
    lv_label_set_text(monitor_lbl, g_monitor_on ? "MON ON" : "MON");
}

static void monitor_btn_cb(lv_event_t *e){
    (void)e;
    logmsg("mon: tap (drag=%d)", hal_input_last_drag());
    if(!tap_ok()) return;
    ui_activity();
    monitor_toggle();
}

static void geo_stop(void);
static void toggle_record(void){
    if(g_geo_on){
        /* 原始 8 通道采集要按两次才停：测量时手上一直动着设备，误碰 STOP 一次
           就会把整段采集废掉（实测废过两次）。第一次按只进入待确认状态。 */
        if(!g_geo_stop_arm || mono_ms() - g_geo_stop_arm > 3000){
            g_geo_stop_arm = mono_ms();
            if(state_lbl){
                lv_label_set_text(state_lbl, "TAP AGAIN TO STOP");
                lv_obj_set_style_text_color(state_lbl, lv_color_hex(0xFFC400), 0);
            }
            logmsg("geo: stop armed (press again within 3 s)");
        } else {
            g_geo_stop_arm = 0;
            geo_stop();
            set_rec_ui(0);
        }
        return;
    }
    if(!engine_running() && play_is_active()){ play_stop(); g_play_file[0] = 0; logmsg("play: stopped (record start)"); }
    if(engine_running()){
        engine_stop();
        set_rec_ui(0);
    } else {
        int rc = engine_start(g_dir, g_device, input_mode, 3600, 8, FMT_RATE[g_fmt], FMT_BITS[g_fmt]);
        if(rc==0){ rec_sec = 0; set_rec_ui(1); }
        else { lv_label_set_text(state_lbl, rc==-2 ? "MIC BUSY" : "NO AUDIO");
               lv_obj_set_style_text_color(state_lbl, lv_color_hex(C_RED), 0); }
    }
}
/* REC / PAUSE：未录音则开始；录音中则暂停；暂停中则继续。停止不走这里。 */
static void rec_pause_cycle(void){
    if(g_geo_on){ toggle_record(); return; }        /* 原始采集：按两次停止 */
    if(!engine_running()){ toggle_record(); return; }
    g_paused = !g_paused;
    engine_pause(g_paused);
    set_rec_ui(1);
    logmsg("ui: record %s", g_paused ? "paused" : "resumed");
}
/* STOP：录音/暂停中则停止；否则停回放 */
static void rec_stop_all(void){
    if(g_geo_on){ toggle_record(); return; }
    if(engine_running()){
        engine_stop(); g_paused = 0; set_rec_ui(0);
        logmsg("recorder stopped by key");
        return;
    }
    if(play_is_active()){ play_stop(); g_play_file[0] = 0; logmsg("play: stopped by key"); }
}
static void rec_cb(lv_event_t *e){ (void)e; if(!tap_ok()) return; ui_activity(); rec_pause_cycle(); }
static void rec_long_cb(lv_event_t *e){ (void)e; if(!tap_ok()) return; ui_activity(); rec_stop_all(); }

static void mode_cb(lv_event_t *e){
    (void)e;
    if(!tap_ok()) return;
    ui_activity();
    if(engine_running()) return;    /* 录音中不允许切拾音模式 */
    if(g_geo_on) return;            /* 原始 8 通道采集由 PDM 阵列直接出流，模式无意义 */
    input_mode = !input_mode;
    apply_mode();
    mode_save();
}

/* 自己实现滑动手势：按下记点，松开算位移。鼠标/触摸都适用，阈值 30 px。 */
#define SWIPE_MIN 22
static lv_point_t g_press_pt;
static void swipe_press_cb(lv_event_t *e){
    (void)e;
    /* 灭屏时触摸不唤醒（面板切换会产生虚假触点，会导致 熄屏↔点亮 死循环）；
       唤醒只认电源键。 */
    if(!g_screen_on) return;
    lv_indev_t *indev = lv_indev_active();
    if(indev) lv_indev_get_point(indev, &g_press_pt);
}
static void swipe_release_cb(lv_event_t *e){
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if(!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    int dx = p.x - g_press_pt.x, dy = p.y - g_press_pt.y;
    int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
    if(adx < SWIPE_MIN || adx < ady) return;   /* 水平意图即可翻页（斜一点也算） */
    if(dx > 0) show_page((page_idx+2)%NPAGES);
    else       show_page((page_idx+1)%NPAGES);
}

static void build_record_page(lv_obj_t *p){
    state_lbl = mk_label(p,"STANDBY",&lv_font_montserrat_14,C_DIM);
    lv_obj_align(state_lbl,LV_ALIGN_TOP_MID,0,10);
    stor_lbl = mk_label(p,"--",&lv_font_montserrat_14,C_DIM);
    lv_obj_align(stor_lbl,LV_ALIGN_TOP_LEFT,8,10);
    batt_lbl = mk_label(p,"--",&lv_font_montserrat_14,C_DIM);
    lv_obj_align(batt_lbl,LV_ALIGN_TOP_RIGHT,-8,10);
    clock_lbl = mk_label(p,"--:--",&lv_font_montserrat_14,C_DIM);
    lv_obj_align(clock_lbl,LV_ALIGN_TOP_LEFT,8,26);

    timer_lbl = mk_label(p,"00:00:00",&lv_font_montserrat_24,C_TEXT);
    lv_obj_align(timer_lbl,LV_ALIGN_TOP_MID,0,28);

    lv_obj_t *mb = lv_button_create(p);
    lv_obj_set_size(mb,150,32);
    lv_obj_align(mb,LV_ALIGN_TOP_MID,-40,53);
    lv_obj_set_style_radius(mb,12,0);
    lv_obj_set_style_bg_color(mb,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_shadow_width(mb,0,0);
    lv_obj_set_style_border_width(mb,1,0);
    lv_obj_set_style_border_color(mb,lv_color_hex(C_LINE),0);
    g_mode_btn = mb;
    mode_lbl = mk_label(mb,"DIRECTIONAL",&lv_font_montserrat_14,C_GREEN);
    lv_obj_center(mode_lbl);
    lv_obj_add_event_cb(mb,mode_cb,LV_EVENT_CLICKED,NULL);

    /* 监听开关（仅插耳机可用）：模式 chip 右侧 */
    monitor_btn = lv_button_create(p);
    lv_obj_set_size(monitor_btn,76,32);
    lv_obj_align(monitor_btn,LV_ALIGN_TOP_MID,92,53);   /* 与模式胶囊同高，且不与下方容器重叠 */
    lv_obj_set_style_radius(monitor_btn,12,0);
    lv_obj_set_style_bg_color(monitor_btn,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_shadow_width(monitor_btn,0,0);
    lv_obj_set_style_border_width(monitor_btn,1,0);
    lv_obj_set_style_border_color(monitor_btn,lv_color_hex(C_LINE),0);
    monitor_lbl = mk_label(monitor_btn,"MON",&lv_font_montserrat_14,C_DIM);
    lv_obj_center(monitor_lbl);
    lv_obj_add_event_cb(monitor_btn,monitor_btn_cb,LV_EVENT_CLICKED,NULL);
    monitor_refresh();

    /* ---- 可视化容器（两种拾音各一套）---- */
    viz_omni = mk_panel(p,0,89,W,107,0,C_BG);
    lv_obj_add_flag(viz_omni,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(viz_omni,reset_cb,LV_EVENT_CLICKED,NULL);
    {
        /* 方形分布：左列 3 条（ch5/ch2/ch0），右列 3 条（ch4/ch3/ch1） */
        static const int rowy[3] = {89,113,137};
        for(int i=0;i<3;i++){
            bars[i]   = mk_meter(viz_omni,4,rowy[i]-89,92,12)->bar;
            bars[3+i] = mk_meter(viz_omni,224,rowy[i]-89,92,12)->bar;
        }
    }
    viz_top = mk_panel(p,0,89,W,107,0,C_BG);
    lv_obj_add_flag(viz_top,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(viz_top,reset_cb,LV_EVENT_CLICKED,NULL);
    {
        /* L/R 两条横向电平左右并排（录音键在其下方） */
        /* 定向模式下两颗麦朝向相同，标 L/R 反而误导，不放标签。
           纵向居中于"模式胶囊下沿(89) 到 REC 按钮上沿(150)"之间 */
        bars_top[0]=mk_meter(viz_top,8,14,148,30)->bar;
        bars_top[1]=mk_meter(viz_top,164,14,148,30)->bar;
    }

    rec_btn = lv_button_create(p);
    lv_obj_set_size(rec_btn,REC_SIZE,REC_SIZE);
    lv_obj_align(rec_btn,LV_ALIGN_TOP_MID,0,150);
    lv_obj_set_style_radius(rec_btn,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_color(rec_btn,lv_color_hex(C_GREEN),0);
    lv_obj_set_style_shadow_width(rec_btn,0,0);
    rec_lbl = mk_label(rec_btn,"REC",&lv_font_montserrat_20,0x05060A);
    lv_obj_center(rec_lbl);
    lv_obj_add_event_cb(rec_btn,rec_cb,LV_EVENT_SHORT_CLICKED,NULL);
    lv_obj_add_event_cb(rec_btn,rec_long_cb,LV_EVENT_LONG_PRESSED,NULL);   /* 长按 = 停止 */

    chart = lv_chart_create(p);
    lv_obj_set_size(chart,300,40);
    lv_obj_set_pos(chart,10,206);
    lv_obj_add_flag(chart,LV_OBJ_FLAG_HIDDEN);     /* 待机不显示波形，开录才出现 */
    lv_obj_set_style_bg_opa(chart,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(chart,0,0);
    lv_obj_set_style_pad_all(chart,0,0);
    lv_obj_set_style_size(chart,0,0,LV_PART_INDICATOR);
    lv_chart_set_type(chart,LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart,0,0);
    lv_chart_set_point_count(chart,64);
    lv_chart_set_range(chart,LV_CHART_AXIS_PRIMARY_Y,-64,0);
    lv_chart_set_update_mode(chart,LV_CHART_UPDATE_MODE_SHIFT);
    ser1=lv_chart_add_series(chart,lv_color_hex(C_GREEN),LV_CHART_AXIS_PRIMARY_Y);
    ser2=lv_chart_add_series(chart,lv_color_hex(0x1F8F4A),LV_CHART_AXIS_PRIMARY_Y);

    dbfs_lbl = mk_label(p,"-- dBFS",&lv_font_montserrat_14,C_DIM);
    lv_obj_set_pos(dbfs_lbl,10,250);



}

static void del_row_cb(lv_event_t *e);
static void play_row_cb(lv_event_t *e);

typedef struct { char name[192]; long size; double dur; long mt; int rate, bits; } finfo;
static int finfo_cmp(const void *a, const void *b){
    const finfo *x=a,*y=b;
    return (y->mt > x->mt) - (y->mt < x->mt);
}
static void files_refresh(void){
    if(!files_box) return;
    lv_coord_t keep_y = lv_obj_get_scroll_y(files_box);   /* 记住滚动位置，重建后恢复 */
    lv_obj_clean(files_box);
    static finfo arr[128]; int n=0;
    DIR *d=opendir(g_dir);
    if(d){
        struct dirent *e;
        while((e=readdir(d)) && n<128){
            if(!strstr(e->d_name,".wav")) continue;
            char path[600]; snprintf(path,sizeof path,"%s/%s",g_dir,e->d_name);
            FILE *f=fopen(path,"rb"); if(!f) continue;
            unsigned char h[44]; if(fread(h,1,44,f)!=44){ fclose(f); continue; }
            unsigned long data = h[40]|(h[41]<<8)|(h[42]<<16)|((unsigned long)h[43]<<24);
            unsigned long br   = h[28]|(h[29]<<8)|(h[30]<<16)|((unsigned long)h[31]<<24);
            unsigned long rate = h[24]|(h[25]<<8)|(h[26]<<16)|((unsigned long)h[27]<<24);
            unsigned bits      = h[34]|(h[35]<<8);
            fseek(f,0,SEEK_END); long sz=ftell(f); fclose(f);
            finfo *it=&arr[n++];
            snprintf(it->name,sizeof it->name,"%s",e->d_name);
            it->size=sz; it->dur = br? (double)data/br : 0;
            it->rate=(int)rate; it->bits=(int)bits;
            struct stat stt; it->mt = stat(path,&stt)==0 ? (long)stt.st_mtime : 0;
        }
        closedir(d);
    }
    g_row_fill = NULL;                 /* 行要重建，旧指针作废 */
    qsort(arr,n,sizeof arr[0],finfo_cmp);
    if(n==0){
        lv_obj_t *l=mk_label(files_box,"no recordings",&lv_font_montserrat_14,C_DIM);
        lv_obj_align(l,LV_ALIGN_TOP_MID,0,40);
        return;
    }
    int y=0;
    for(int i=0;i<n;i++){
        int play_row = 0;
        lv_obj_t *row=mk_panel(files_box,8,y,304,84,12,C_PANEL);
        char disp[128];
        snprintf(disp,sizeof disp,"%s",arr[i].name);
        { char *p2=disp;
          if(!strncmp(p2,"REC_",4)) p2+=4;
          char *dot=strrchr(p2,'.'); if(dot) *dot=0;
          char *us=strchr(p2,'_'); if(us) *us=' ';
          memmove(disp,p2,strlen(p2)+1); }
        char shown[160];
        snprintf(shown,sizeof shown,"%s", disp);
        if(play_is_active() && !strcmp(arr[i].name,g_play_file)){
            lv_obj_set_style_bg_color(row, lv_color_hex(0x16241C), 0);   /* 播放行底色压暗，让进度条显出来 */
            play_row = 1;
            /* 背景进度条：宽度随播放进度增长，文字在它上面 */
            g_row_fill = lv_obj_create(row);
            lv_obj_remove_style_all(g_row_fill);
            lv_obj_set_size(g_row_fill, 2, 84);
            lv_obj_set_pos(g_row_fill, 0, 0);
            lv_obj_set_style_bg_color(g_row_fill, lv_color_hex(0x2E7D50), 0);
            lv_obj_set_style_bg_opa(g_row_fill, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(g_row_fill, 12, 0);
            lv_obj_remove_flag(g_row_fill, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(g_row_fill, LV_OBJ_FLAG_SCROLLABLE);
            g_row_fill_pct = -1;
        }
        lv_obj_t *nm=mk_label(row,shown,&lv_font_montserrat_20, C_TEXT);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(nm, 276);
        lv_obj_align(nm,LV_ALIGN_LEFT_MID,12,-14);
        char sub[96]; unsigned secs=(unsigned)(arr[i].dur+0.5);
        char rs[12];
        if(arr[i].rate == 48000)      snprintf(rs,sizeof rs,"48k");
        else if(arr[i].rate == 44100) snprintf(rs,sizeof rs,"44.1k");
        else                          snprintf(rs,sizeof rs,"%d",arr[i].rate);
        snprintf(sub,sizeof sub,"%02u:%02u:%02u  |  %.1f MB  |  %s/%dbit",
                 secs/3600,(secs/60)%60,secs%60,arr[i].size/1048576.0, rs, arr[i].bits);
        lv_obj_t *sb=mk_label(row,sub,&lv_font_montserrat_14, play_row? 0xD8E6DD : C_DIM);
        lv_obj_align(sb,LV_ALIGN_LEFT_MID,12,14);
        lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row,del_row_cb,LV_EVENT_LONG_PRESSED,(void*)arr[i].name);
        lv_obj_add_event_cb(row,play_row_cb,LV_EVENT_SHORT_CLICKED,(void*)arr[i].name);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);   /* 让页级滑动能收到 */
        y+=90;
    }
    lv_obj_update_layout(files_box);
    lv_obj_scroll_to_y(files_box, keep_y, LV_ANIM_OFF);
}

static void http_set_value(int on, const char *text){
    if(!http_val) return;
    lv_label_set_text(http_val, text);
    lv_obj_set_style_text_font(http_val, on ? &lv_font_montserrat_14 : &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(http_val, lv_color_hex(on ? C_GREEN : C_DIM), 0);
}

static void http_toggle_cb(lv_event_t *e){
    (void)e;
    if(!tap_ok()) return;
    if(http_running()){ http_stop(); http_set_value(0, "OFF"); }
    else if(http_start(g_dir,8080)==0){
        http_set_value(1, "ON :8080");
    } else http_set_value(0, "FAILED");
}

static lv_obj_t *mk_row(lv_obj_t *parent,int y,const char *k,const char *v,uint32_t vc, lv_obj_t **val_out){
    lv_obj_t *row = mk_panel(parent,8,y,304,84,12,C_PANEL);
    lv_obj_t *l1 = mk_label(row,k,&lv_font_montserrat_20,C_TEXT);
    lv_obj_align(l1,LV_ALIGN_LEFT_MID,12,0);
    lv_obj_t *l2 = mk_label(row,v,&lv_font_montserrat_20,vc);
    lv_label_set_long_mode(l2, LV_LABEL_LONG_CLIP);   /* 设置值都很短，不做动画（会拖垮 UI） */
    lv_obj_set_width(l2, 180);
    lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(l2,LV_ALIGN_RIGHT_MID,-12,0);
    if(val_out) *val_out=l2;
    return row;
}

static void build_files_page(lv_obj_t *p){
    lv_obj_t *t = mk_label(p,"FILES",&lv_font_montserrat_14,C_DIM);
    lv_obj_align(t,LV_ALIGN_TOP_MID,0,10);
    files_hdr = mk_label(p,"",&lv_font_montserrat_14,C_RED);
    lv_obj_align(files_hdr,LV_ALIGN_TOP_RIGHT,-10,10);
    files_box = mk_panel(p,0,26,W,276,0,C_BG);   /* 不再留底部控制栏的空间 */
    lv_obj_remove_flag(files_box,LV_OBJ_FLAG_CLICKABLE);
    /* 底部控制栏已去掉：播放/暂停靠点行本身，换页即停止播放 */
    files_refresh();
}

static void ui_open_wifi(void){
    lv_obj_t *p = mk_panel(lv_screen_active(),0,0,W,H,0,C_BG);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    wifi_page_create(p);
    ui_push_page(p);
    wifi_page_on_show();
}
/* 8 通道原始采集（麦克风阵列几何/时延测量用）。
   录音引擎只落 2 通道，量阵列必须拿原始 8 通道，所以这里单独起 arecord；
   但状态完全走 UI：屏幕显示 RAW 8CH + 计时，STOP 可停，采集期间不熄屏不休眠。
   不在 tick 里画电平/波形——arecord 的数据不经过进程内，画不了真值，
   宁可什么都不画，也不要用模拟曲线冒充。 */
static void geo_start(int secs);
static void geo_stop(void){
    if(g_geo_pid > 0){
        kill(g_geo_pid, SIGINT);              /* arecord 收到 SIGINT 会回写 WAV 头 */
        int reaped = 0;
        for(int i=0;i<20 && !reaped;i++){
            if(waitpid(g_geo_pid, NULL, WNOHANG) == g_geo_pid) reaped = 1;
            else usleep(50000);
        }
        if(!reaped){ kill(g_geo_pid, SIGKILL); waitpid(g_geo_pid, NULL, 0); }
        g_geo_pid = -1;
    }
    g_geo_on = 0;
    g_geo_stop_arm = 0;
    logmsg("geo: stopped");
}
static void geo_start(int secs){
    if(g_geo_on || engine_running()) return;
    mkdir("/mnt/sdcard/geo", 0777);
    char secbuf[12]; snprintf(secbuf, sizeof secbuf, "%d", secs);
    sync();
    pid_t p = fork();
    if(p == 0){
        setsid();                              /* 脱离 UI 的进程组，UI 挂了也不带走采集 */
        int fd = open("/dev/null", O_WRONLY);
        if(fd >= 0){ dup2(fd, 1); dup2(fd, 2); }
        execl("/usr/bin/arecord", "arecord", "-D", "hw:2,0", "-c", "8", "-r", "48000",
              "-f", "S32_LE", "-d", secbuf, "/mnt/sdcard/geo/geo.wav", (char*)NULL);
        _exit(127);
    }
    if(p > 0){
        g_geo_pid = p; g_geo_t0 = mono_ms(); g_geo_on = 1; g_geo_stop_arm = 0;
        set_rec_ui(1);
        lv_label_set_text(rec_lbl, "STOP");        /* 采集期间单击是"待确认停止" */
        lv_label_set_text(state_lbl, "RAW 8CH");
        lv_obj_set_style_text_color(state_lbl, lv_color_hex(C_RED), 0);
        if(chart) lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
        /* 原始采集固定拿全部 8 路麦克风，拾音模式开关在这一模式下没有意义 */
        if(g_mode_btn){ lv_obj_remove_flag(g_mode_btn, LV_OBJ_FLAG_CLICKABLE); lv_label_set_text(mode_lbl, "8 MIC ARRAY"); }
        rec_sec = 0;
        screen_set(1);                 /* 熄屏时起采也要让人看到状态 */
        ui_activity();
        logmsg("geo: started pid %d, %d s -> /mnt/sdcard/geo/geo.wav", (int)p, secs);
    } else {
        lv_label_set_text(state_lbl, "NO AUDIO");
        lv_obj_set_style_text_color(state_lbl, lv_color_hex(C_RED), 0);
        logmsg("geo: fork failed");
    }
}

/* 硬件测试：复用独立模块 e1hwtest（6 页全套）。
   不 exec（否则退出要走重启流程、还会闪回原厂画面），改为：让出显示 → 子进程跑 → 收回并重绘。 */
static pid_t g_hw_pid = -1;
static int   g_hw_return_page = -1;
static void hwtest_start(void){
    if(g_hw_pid > 0) return;
    if(g_geo_on){                                  /* 原始 8 通道采集在跑，别抢 PDM */
        show_warn("Stop raw capture before hardware test.");
        logmsg("hwtest: refused (raw 8ch capture)");
        return;
    }
    if(engine_running()){                          /* 录音中禁止进硬件测试（会抢 PDM/codec） */
        show_warn("Stop recording before hardware test.");
        logmsg("hwtest: refused (recording)");
        return;
    }
    if(play_is_active()){ play_stop(); g_play_file[0] = 0; }   /* 先停回放，避免抢 codec */
    sync();
    g_hw_return_page = page_idx;                   /* 记住当前页，回来还在这 */
    hal_set_touch_enabled(0);                      /* 测试期间 freE1 不处理触摸 */
    hal_disp_set_flush_enabled(0);                 /* 让出 framebuffer */
    pid_t p = fork();
    if(p == 0){
        execl("/mnt/sdcard/e1hwtest/e1hwtest", "e1hwtest", (char*)NULL);
        _exit(127);
    }
    if(p > 0){
        g_hw_pid = p;
        logmsg("hwtest: started pid %d", (int)p);
    } else {
        hal_disp_set_flush_enabled(1);
        logmsg("hwtest: fork failed");
    }
}
static void hwtest_row_cb(lv_event_t *e){ (void)e; if(!tap_ok()) return; hwtest_start(); }

static void wifi_row_cb(lv_event_t *e){ (void)e; ui_open_wifi(); }

static void build_settings_page(lv_obj_t *p){
    lv_obj_t *t = mk_label(p,"SETTINGS",&lv_font_montserrat_14,C_DIM);
    lv_obj_align(t,LV_ALIGN_TOP_MID,0,10);
    settings_box = mk_panel(p,0,26,W,276,0,C_BG);
    lv_obj_remove_flag(settings_box,LV_OBJ_FLAG_CLICKABLE);
    int y=0;
    { lv_obj_t *fr=mk_row(settings_box,y, "Format", g_fmt? "48k / 16bit" : "48k / 24bit", C_TEXT, &fmt_val);
      lv_obj_add_flag(fr,LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(fr,fmt_cb,LV_EVENT_CLICKED,NULL); y+=90; }
    { char b[16]; snprintf(b,sizeof b,"%d s",g_idle_min);
      lv_obj_t *ir=mk_row(settings_box,y,"Screen off",b,C_TEXT,&idle_val);
      lv_obj_add_flag(ir,LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(ir,idle_cb,LV_EVENT_CLICKED,NULL); y+=90; }
    mk_row(settings_box,y,"Storage","--",C_TEXT,&storage_val); y+=90;
    lv_obj_t *wr=mk_row(settings_box,y,"WiFi","wlan0",C_TEXT,NULL);
    lv_obj_add_flag(wr,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wr,wifi_row_cb,LV_EVENT_CLICKED,NULL); y+=90;
    lv_obj_t *hr=mk_row(settings_box,y,"HTTP server",
                        g_http_port? "ON" : "OFF",
                        g_http_port? C_GREEN : C_DIM, &http_val);
    if(http_val){
        lv_obj_set_width(http_val, 190);
        http_set_value(g_http_port ? 1 : 0, g_http_port ? "ON" : "OFF");
    }
    lv_obj_add_flag(hr,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hr,http_toggle_cb,LV_EVENT_CLICKED,NULL); y+=90;
    g_adb_on = adb_running();
    { lv_obj_t *ar=mk_row(settings_box,y,"ADB", g_adb_on?"ON":"OFF",
                          g_adb_on?C_GREEN:C_DIM, &adb_val);
      lv_obj_add_flag(ar,LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(ar,adb_cb,LV_EVENT_CLICKED,NULL); y+=90; }
    lv_obj_t *pr=mk_row(settings_box,y,"Performance mode", g_perf_on?"ON":"OFF",
                        g_perf_on?C_GREEN:C_DIM, &perf_val);
    lv_obj_add_flag(pr,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pr,perf_cb,LV_EVENT_CLICKED,NULL); y+=90;
    lv_obj_t *hw=mk_row(settings_box,y,"Hardware test",">",C_GREEN,NULL);
    lv_obj_add_flag(hw,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hw,hwtest_row_cb,LV_EVENT_CLICKED,NULL);
}

static void build_ui(void){
    lv_obj_t *scr=lv_screen_active();
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_BG),0);
    lv_obj_set_style_bg_opa(scr,LV_OPA_COVER,0);
    for(int i=0;i<NPAGES;i++){
        pages[i]=mk_panel(scr,0,0,W,H,0,C_BG);
        lv_obj_remove_flag(pages[i],LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pages[i],LV_OBJ_FLAG_CLICKABLE);
        if(i != 0) lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);  /* 首帧只露 Record，避免闪其它页 */
    }
    build_record_page(pages[0]);
    build_files_page(pages[1]);
    build_settings_page(pages[2]);
    for(int i=0;i<NPAGES;i++) no_scroll_recursive(pages[i]);
    /* 列表页允许纵向滚动（4 行可见），横向滑动仍然是翻页 */
    if(files_box){ lv_obj_add_flag(files_box,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scroll_dir(files_box,LV_DIR_VER); lv_obj_set_scrollbar_mode(files_box,LV_SCROLLBAR_MODE_OFF); }
    if(settings_box){ lv_obj_add_flag(settings_box,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scroll_dir(settings_box,LV_DIR_VER); lv_obj_set_scrollbar_mode(settings_box,LV_SCROLLBAR_MODE_OFF); }
    apply_mode();
    show_page(0);
}

/* 设备端调试钩子：/mnt/sdcard/freE1/.cmd 里写命令，执行后删除 */
static void show_power_dialog(void);
static void power_dlg_close(void);
static void toggle_record(void);
static void do_suspend(void);

static void poll_cmd(void){
    FILE *f=fopen("/mnt/sdcard/freE1/.cmd","rb");
    if(!f) return;
    char b[64]={0}; size_t n=fread(b,1,sizeof b-1,f); (void)n; fclose(f);
    while(n>0 && (b[n-1]=='\n' || b[n-1]=='\r' || b[n-1]==' ')) b[--n]=0;
    remove("/mnt/sdcard/freE1/.cmd");
    if(!strncmp(b,"page ",5)){ int n=atoi(b+5); if(n>=NPAGES) ui_open_wifi(); else show_page(n); }
    else if(!strncmp(b,"scan",4))  wifi_ui_trigger_scan();
    else if(!strncmp(b,"home",4)){ while(g_depth>0) ui_pop_page(); show_page(0); }
    else if(!strncmp(b,"pwd ",4))   wifi_ui_demo_open(b+4);
    else if(!strncmp(b,"close",5)) wifi_ui_demo_close();
    else if(!strncmp(b,"sleep",5)) do_suspend();
    else if(!strncmp(b,"pwr",3)){ ui_on_hw_key(7,1); lv_timer_t *t=lv_timer_create(pwr_relay_cb,120,NULL); lv_timer_set_repeat_count(t,1); }
    else if(!strncmp(b,"play ",5)){ play_stop(); play_file(b+5); }
    else if(!strncmp(b,"pstop",5)){ play_stop(); logmsg("play: stopped (cmd)"); }
    else if(!strncmp(b,"hwt",3)){ hwtest_start(); }
    else if(!strncmp(b,"mon",3)){ monitor_toggle(); logmsg("state: monitor=%d underruns=%d drops=%d", g_monitor_on, monitor_underruns(), monitor_drops()); }
    else if(!strncmp(b,"ppause",6)){ play_toggle_pause(); logmsg("play: toggle pause (cmd)"); }
    else if(!strncmp(b,"pseek ",6)){ play_seek_ms(atoll(b+6)); logmsg("play: seek %s ms (cmd)", b+6); }
    else if(!strncmp(b,"httpoff",7)){ logmsg("test: http_stop begin"); http_stop(); logmsg("test: http_stop done"); }
    else if(!strncmp(b,"httpon",6)){ int r=http_start(g_dir,8080); logmsg("test: http_start -> %d", r); }
    else if(!strncmp(b,"state",5)){
        logmsg("state: screen=%d touch=%d http=%d label=%s", g_screen_on, hal_input_get_enabled(), g_http_port, http_val? lv_label_get_text(http_val) : "-");
    }
    else if(!strncmp(b,"guardcheck",10)){
        int cap=-1; char st[32]={0};
        FILE *bf=fopen("/sys/class/power_supply/battery/capacity","r");
        if(bf){ if(fscanf(bf,"%d",&cap)!=1) cap=-1; fclose(bf); }
        bf=fopen("/sys/class/power_supply/battery/status","r");
        if(bf){ if(fgets(st,sizeof st,bf)){} fclose(bf); }
        logmsg("guardcheck: engine=%d cap=%d status=%.8s batt_thr=%d ignore_chg=%d free_thr=%.2f",
               engine_running(), cap, st, g_guard_batt, g_guard_ignore_chg, g_guard_free_gb);
    }
    else if(!strncmp(b,"t9test",6)){
        if(!wifi_overlay_open()){ logmsg("t9test: overlay closed"); }
        else {
            for(int i=0;i<5;i++){
                int64_t t0=mono_ms();
                wifi_t9_press(5);          /* 按 5 jkl */
                lv_refr_now(NULL);
                logmsg("t9test: press+repaint %lld ms", (long long)(mono_ms()-t0));
            }
        }
    }
    else if(!strncmp(b,"blank",5)) screen_set(0);
    else if(!strncmp(b,"scroll ",7)) wifi_ui_scroll(atoi(b+7));
    else if(!strncmp(b,"scrolls ",8)){ if(settings_box) lv_obj_scroll_by(settings_box,0,-atoi(b+8),LV_ANIM_OFF); }
    else if(!strncmp(b,"scrollend",9)){ if(settings_box) lv_obj_scroll_to_y(settings_box, LV_COORD_MAX, LV_ANIM_OFF); }
    else if(!strncmp(b,"rec",3)) toggle_record();
    else if(!strncmp(b,"pause",5)) rec_pause_cycle();     /* 等同 REC/PAUSE 键 */
    else if(!strncmp(b,"mode ",5)){ if(engine_running()) { logmsg("mode: refused (recording)"); } else { input_mode = atoi(b+5)?1:0; apply_mode(); mode_save(); logmsg("mode -> %d", input_mode); } }
    else if(!strncmp(b,"stopkey",7)) rec_stop_all();      /* 等同 STOP 键 */
    else if(!strncmp(b,"geostop",7)){ if(g_geo_on) geo_stop(); set_rec_ui(0); }
    else if(!strncmp(b,"geo",3)){
        if(g_geo_on){ geo_stop(); set_rec_ui(0); }
        else geo_start(atoi(b+3) > 0 ? atoi(b+3) : 900);   /* "geo 120" 可选秒数 */
    }
    else if(!strncmp(b,"power",5)) show_power_dialog();
    else if(!strncmp(b,"closep",6)) power_dlg_close();
    logmsg("cmd: %s", b);
}

static void ui_tick(lv_timer_t *t){
    (void)t;
    if(recording){
        rec_sec++;
        char buf[16];
        unsigned h=rec_sec/3600, m=(rec_sec/60)%60, s=rec_sec%60;
        snprintf(buf,sizeof buf,"%02u:%02u:%02u",h,m,s);
        lv_label_set_text(timer_lbl,buf);
    }
    if(g_depth>0) wifi_page_tick();
    if(g_device) poll_cmd();
    /* 空闲：N 秒不操作 → 熄屏；再 M 秒 → 深度休眠（电源键唤醒） */
    if(!g_shots && g_device && g_depth==0 && g_last_activity){
        int64_t idle = mono_ms() - g_last_activity;
        if(g_screen_on && idle > (int64_t)g_idle_min*1000){
            screen_set(0);
            g_off_at = mono_ms();
            logmsg("idle: screen off%s", engine_running() ? " (recording, no suspend)" : "");
        } else if(!g_screen_on && g_off_at && !engine_running()
                  && !hs_usb_online() && !hs_battery_charging()   /* 插着 USB 就不休眠 */
                  && (mono_ms() - g_off_at) > (int64_t)g_sleep_after*1000){
            g_off_at = 0;                    /* 先清，避免醒来后立刻再睡 */
            do_suspend();
        }
    }
    /* 硬件测试子进程回收 + 显示接管 */
    hs_motor_poll();          /* 推进振动序列，否则马达会一直震 */
    if(g_hw_reclaim_at && mono_ms() >= g_hw_reclaim_at){
        g_hw_reclaim_at = 0;
        hal_disp_fb_reclaim();
        screen_set(1);                             /* 会重新打开触摸 */
        if(g_hw_return_page >= 0){ show_page(g_hw_return_page); g_hw_return_page = -1; }
        ui_activity();
        g_off_at = 0;
        lv_obj_invalidate(lv_screen_active());
        logmsg("hwtest: display reclaimed");
    }
    if(g_hw_pid > 0){
        int hst = 0;
        if(waitpid(g_hw_pid, &hst, WNOHANG) == g_hw_pid){
            g_hw_pid = -1;
            while(waitpid(-1, NULL, WNOHANG) > 0) { }
            g_hw_reclaim_at = mono_ms() + 300;
            logmsg("hwtest: exited, reclaim scheduled");
        }
    }
    play_state_t ps;
    play_get_state(&ps);
    { static int was_active = 0;
      if(was_active && !ps.active && page_idx == 1) files_refresh();   /* 播完：清掉行高亮 */
      was_active = ps.active; }
    files_progress_update(&ps);
    if(g_geo_on){
        /* 8 通道原始采集：数据不经过本进程，只能显示计时，别画假的电平/波形 */
        if(g_geo_pid > 0 && waitpid(g_geo_pid, NULL, WNOHANG) == g_geo_pid){
            g_geo_pid = -1; g_geo_on = 0;
            logmsg("geo: capture finished (arecord exited)");
            set_rec_ui(0);
        } else {
            if(g_geo_stop_arm && mono_ms() - g_geo_stop_arm > 3000){
                g_geo_stop_arm = 0;      /* 没再按，取消停止 */
                lv_label_set_text(state_lbl, "RAW 8CH");
                lv_obj_set_style_text_color(state_lbl, lv_color_hex(C_RED), 0);
                logmsg("geo: stop cancelled");
            }
            uint32_t s = (uint32_t)((mono_ms() - g_geo_t0) / 1000);
            char buf[16];
            snprintf(buf, sizeof buf, "%02u:%02u:%02u", s/3600, (s/60)%60, s%60);
            if(timer_lbl) lv_label_set_text(timer_lbl, buf);
            rec_sec = s;
            ui_activity();                 /* 采集期间保持亮屏、不进休眠 */
        }
    }
    if(!g_geo_on && !g_shots && recording != engine_running()){
        set_rec_ui(engine_running());
        if(!engine_running()){ ui_activity(); g_off_at = 0; if(g_monitor_on){ g_monitor_on=0; monitor_close(); monitor_refresh(); } }   /* 停录后重新计时，别立刻睡 */
    }
    rec_status *st = engine_status();
    if(st && st->running && st->paused != g_paused){ g_paused = st->paused; set_rec_ui(1); }
    { static int64_t last=0; int64_t now=mono_ms();
      if(now-last>1000){
          /* 顶栏：电量 / 剩余空间 / 耳机 */
          int cap = hs_battery_capacity();
          int hp  = hs_hp_inserted();
          /* 驱动在耳机拔出时把 DAC HPOUT 静音且不会恢复，插拔时必须重新施加路由；
             另外每 30 s 再压一次，防止别的路径又把它关掉 */
          {
              static int hp_prev = -1, hp_n = 0;
              if(hp != hp_prev){
                  hp_prev = hp;
                  hs_audio_route(hp);
                  monitor_refresh();
                  if(hp != 1 && g_monitor_on){
                      g_monitor_on = 0; monitor_close(); monitor_refresh();
                      logmsg("monitor: off (headphone removed)");
                  }
                  logmsg("headphone: %s", hp == 1 ? "inserted" : "removed");
              } else if(++hp_n >= 30){ hp_n = 0; hs_audio_route(hp); }
          }
          int charging = hs_battery_charging();
          if(batt_lbl){
              if(cap<0) lv_label_set_text(batt_lbl,"--");
              else {
                  char b[32];
                  snprintf(b,sizeof b,"%d%%%s%s",cap, charging?"+":"", hp==1?"  HP":"");
                  lv_label_set_text(batt_lbl,b);
                  uint32_t bc = cap<=10 ? C_RED : (cap<=20 ? 0xFFC400 : C_DIM);
                  lv_obj_set_style_text_color(batt_lbl,lv_color_hex(bc),0);
              }
          }
          /* 分级提示：20% 变色（上面已做）、10% 弹一次窗、2% 自动关机 */
          if(cap >= 0 && !charging){
              if(cap <= 20 && g_batt_level_shown < 20){ g_batt_level_shown = 20; logmsg("battery: %d%%", cap); }
              if(cap <= 10 && g_batt_level_shown < 10){
                  g_batt_level_shown = 10;
                  show_warn("Battery low (10%). Recording will stop at 5%.");
                  logmsg("battery: 10%% warning");
              }
              if(cap <= 2 && g_batt_level_shown < 2){
                  g_batt_level_shown = 2;
                  logmsg("battery: 2%%, powering off");
                  if(engine_running()) engine_stop();
                  sync();
                  system("poweroff");
              }
          } else if(cap > 25) {
              g_batt_level_shown = 0;   /* 充电回升后重置 */
          }
          monitor_refresh();
          if(files_hdr) lv_label_set_text(files_hdr, engine_running()?"REC":"");
          { static int n=0; if(++n % 5 == 0){ int a = adb_running(); if(a != g_adb_on){ g_adb_on = a; adb_set_value(a); } } }
          if(http_val && g_http_port){
              char ip[32];
              char want[48];
              if(wlan_ip(ip,sizeof ip)==0) snprintf(want,sizeof want,"%s:%d", ip, g_http_port);
              else                          snprintf(want,sizeof want,"ON :%d", g_http_port);
              if(strcmp(lv_label_get_text(http_val), want) != 0) http_set_value(1, want);
          }
          if(clock_lbl){
              time_t tt=time(NULL); struct tm tm; localtime_r(&tt,&tm);
              char cb[8]; snprintf(cb,sizeof cb,"%02d:%02d",tm.tm_hour,tm.tm_min);
              lv_label_set_text(clock_lbl,cb);
          }
      }
      {   /* 背光状态核对：硬件为准，避免内部状态与硬件漂移 */
          FILE *bf=fopen("/sys/class/backlight/backlight/brightness","r");
          if(bf){
              int bl=-1; if(fscanf(bf,"%d",&bl)!=1) bl=-1; fclose(bf);
              if(bl >= 0){
                  if(bl == 0 && g_screen_on){ g_screen_on = 0; hal_set_touch_enabled(0); hal_disp_set_flush_enabled(0); logmsg("sync: backlight off -> screen off"); }
                  else if(bl > 0 && !g_screen_on){ g_screen_on = 1; hal_set_touch_enabled(1); hal_disp_set_flush_enabled(1); lv_obj_invalidate(lv_screen_active()); logmsg("sync: backlight on -> screen on"); }
              }
          }
      }
      if(storage_val && now-last>1000){
          last=now; struct statvfs vf; double free_gb=-1;
          if(statvfs(g_dir,&vf)==0){
              free_gb=(double)vf.f_bavail*vf.f_frsize/1e9;
              char b[48]; snprintf(b,sizeof b,"%.1f GB free",free_gb); lv_label_set_text(storage_val,b);
              if(stor_lbl){
                  double bps = (double)FMT_RATE[g_fmt] * 2.0 * (FMT_BITS[g_fmt]/8.0);
                  double hours = bps > 0 ? (free_gb*1e9 / bps / 3600.0) : 0;
                  char b2[32];
                  if(hours >= 10) snprintf(b2,sizeof b2,"%.1fG %.0fh", free_gb, hours);
                  else            snprintf(b2,sizeof b2,"%.1fG %.1fh", free_gb, hours);
                  lv_label_set_text(stor_lbl,b2);
              }
          }
          /* 满盘 / 低电保护（仅在录音时动作） */
          if(g_device && engine_running()){
              int cap = hs_battery_capacity();
              int charging = hs_battery_charging();
              if(g_guard_ignore_chg) charging = 0;   /* 测试用 */
              if(free_gb >= 0 && free_gb < g_guard_free_gb){
                  engine_stop(); set_rec_ui(0);
                  lv_label_set_text(state_lbl,"STORAGE FULL");
                  lv_obj_set_style_text_color(state_lbl,lv_color_hex(C_RED),0);
                  logmsg("guard: storage full (%.2f GB), stopped", free_gb);
              } else if(cap >= 0 && cap < g_guard_batt && !charging){
                  engine_stop(); set_rec_ui(0);
                  lv_label_set_text(state_lbl,"LOW BATTERY");
                  lv_obj_set_style_text_color(state_lbl,lv_color_hex(C_RED),0);
                  sync();
                  logmsg("guard: low battery (%d%%), stopped", cap);
              }
          }
      } }
    if(recording && st && st->running){
        for(int i=0;i<6;i++) meter_update(&meters[i], st->levels[i]);
        meter_update(&meters[6], st->levels[6]);
        meter_update(&meters[7], st->levels[7]);
        unsigned long long fr = st->frames;
        rec_sec = (uint32_t)(fr/48000);
        char buf[16]; unsigned h=rec_sec/3600,m=(rec_sec/60)%60,sec=rec_sec%60;
        snprintf(buf,sizeof buf,"%02u:%02u:%02u",h,m,sec);
        lv_label_set_text(timer_lbl,buf);
        char b2[24]; snprintf(b2,sizeof b2,"%d dBFS", st->db_dbfs);
        lv_label_set_text(dbfs_lbl,b2);
        double tt2=(double)(lv_tick_get())/1000.0;
        if(st->db_dbfs>-70 && !g_paused){
            lv_chart_set_next_value(chart,ser1,st->db_dbfs);
            lv_chart_set_next_value(chart,ser2,st->db_dbfs-6-3*sin(tt2*3.1));
        }
        return;
    }
    double tt = (double)(lv_tick_get())/1000.0;
    double base = 0.35 + 0.30*(0.5+0.5*sin(tt*1.7));
    if(!recording || g_geo_on){
        /* 待机时不留任何电平读数：上次的 -72 dBFS 看着仍像在采 */
        /* 原始 8 通道采集同理：进程内没有数据，不画模拟曲线 */
        for(int i=0;i<8;i++) meter_update(&meters[i],0);
        lv_label_set_text(dbfs_lbl,"-- dBFS");
        return;
    }
    for(int i=0;i<6;i++){
        double v = base*(0.55+0.45*sin(tt*2.3+i*1.1));
        meter_update(&meters[i],(int)(v*100));
    }
    meter_update(&meters[6],(int)(base*100*(0.9+0.1*sin(tt*1.3))));
    meter_update(&meters[7],(int)(base*100*(0.9+0.1*sin(tt*1.3+0.4))));
    int db = (int)(-60.0 + 42.0*base);
    char b2[24]; snprintf(b2,sizeof b2,"%d dBFS",db);
    lv_label_set_text(dbfs_lbl,b2);
    lv_chart_set_next_value(chart,ser1,-60+38*base);
    lv_chart_set_next_value(chart,ser2,-60+30*base*(0.6+0.4*sin(tt*3.1)));
}

/* ---------------- headless PNG ---------------- */
static uint32_t g_fb[W*H];
static uint8_t  g_draw[W*H*4];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px){
    int w=lv_area_get_width(a), h=lv_area_get_height(a);
    for(int y=0;y<h;y++)
        memcpy(&g_fb[(size_t)(a->y1+y)*W + a->x1], px + (size_t)y*w*4, (size_t)w*4);
    lv_display_flush_ready(d);
}

static void dump_png(const char *path){
#ifndef FREEL1_NO_PNG
    static uint8_t rgb[W*H*3];
    for(int i=0;i<W*H;i++){
        uint32_t v=g_fb[i];
        rgb[3*i+0]=(v>>16)&0xff;
        rgb[3*i+1]=(v>>8)&0xff;
        rgb[3*i+2]=v&0xff;
    }
    png_write_rgb(path,rgb,W,H);
#else
    (void)path;
#endif
}

int ui_run_shots(const char *dir, const char *outdir){
    snprintf(g_dir,sizeof g_dir,"%s",dir);
    g_shots = 1;
    lv_init();
    lv_display_t *d=lv_display_create(W,H);
    lv_display_set_flush_cb(d,flush_cb);
    lv_display_set_buffers(d,g_draw,NULL,sizeof g_draw,LV_DISPLAY_RENDER_MODE_FULL);
    build_ui();
    lv_timer_create(ui_tick,200,NULL);

    char path[512];
    for(int i=0;i<6;i++){ lv_tick_inc(50); lv_timer_handler(); }
    lv_refr_now(d); snprintf(path,sizeof path,"%s/01-standby.png",outdir); dump_png(path);

    recording=1; lv_label_set_text(rec_lbl,"STOP");
    lv_obj_set_style_bg_color(rec_btn,lv_color_hex(C_RED),0);
    lv_label_set_text(state_lbl,"RECORDING");
    lv_obj_set_style_text_color(state_lbl,lv_color_hex(C_RED),0);
    for(int i=0;i<140;i++){ lv_tick_inc(200); ui_tick(NULL); }
    lv_refr_now(d); snprintf(path,sizeof path,"%s/02-recording-directional.png",outdir); dump_png(path);

    /* 切到全向 */
    input_mode=1; apply_mode();
    for(int i=0;i<20;i++){ lv_tick_inc(200); ui_tick(NULL); }
    lv_refr_now(d); snprintf(path,sizeof path,"%s/03-recording-omni.png",outdir); dump_png(path);


    show_page(1); lv_timer_handler(); lv_refr_now(d);
    snprintf(path,sizeof path,"%s/04-files.png",outdir); dump_png(path);

    show_page(2); lv_timer_handler(); lv_refr_now(d);
    snprintf(path,sizeof path,"%s/05-settings.png",outdir); dump_png(path);

    ui_open_wifi();
    for(int i=0;i<8;i++){ lv_tick_inc(50); lv_timer_handler(); }
    lv_refr_now(d);
    snprintf(path,sizeof path,"%s/06-wifi.png",outdir); dump_png(path);

    wifi_ui_demo_open("Example-AP");
    for(int i=0;i<4;i++){ lv_tick_inc(50); lv_timer_handler(); }
    lv_refr_now(d);
    snprintf(path,sizeof path,"%s/07-wifi-password.png",outdir); dump_png(path);
    wifi_ui_demo_close();
    ui_pop_page();


    logmsg("shots written to %s", outdir);
    return 0;
}

static lv_obj_t *del_dlg = NULL;
static char g_del_name[192];
static lv_obj_t *warn_dlg = NULL;
static void warn_close_cb(lv_event_t *e){ (void)e; if(warn_dlg){ lv_obj_delete(warn_dlg); warn_dlg=NULL; } tap_guard(); }
static void show_warn(const char *msg){
    logmsg("warn: %s (dialog=%p)", msg, (void*)warn_dlg);
    if(warn_dlg) return;
    warn_dlg = mk_panel(lv_screen_active(),0,0,W,H,0,C_BG);
    lv_obj_add_flag(warn_dlg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = mk_label(warn_dlg,"Not available",&lv_font_montserrat_20,C_RED);
    lv_obj_align(t,LV_ALIGN_TOP_MID,0,100);
    lv_obj_t *sub = mk_label(warn_dlg,msg,&lv_font_montserrat_14,C_DIM);
    lv_obj_set_width(sub, 280);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_align(sub,LV_ALIGN_TOP_MID,0,136);
    lv_obj_t *ok = lv_button_create(warn_dlg);
    lv_obj_set_size(ok,140,52);
    lv_obj_align(ok,LV_ALIGN_TOP_MID,0,196);
    lv_obj_set_style_radius(ok,12,0);
    lv_obj_set_style_bg_color(ok,lv_color_hex(C_GREEN),0);
    lv_obj_set_style_shadow_width(ok,0,0);
    lv_obj_t *ol=mk_label(ok,"OK",&lv_font_montserrat_20,C_BG);
    lv_obj_center(ol);
    lv_obj_add_event_cb(ok,warn_close_cb,LV_EVENT_CLICKED,NULL);
}

static lv_obj_t *power_dlg = NULL;
static void power_dlg_close(void){ if(power_dlg){ lv_obj_delete(power_dlg); power_dlg=NULL; } tap_guard(); }
static void del_dlg_close(void){ if(del_dlg){ lv_obj_delete(del_dlg); del_dlg=NULL; } tap_guard(); }
static void del_ok_cb(lv_event_t *e){
    (void)e;
    char path[600]; snprintf(path,sizeof path,"%s/%s",g_dir,g_del_name);
    if(unlink(path)==0) logmsg("deleted %s", g_del_name);
    del_dlg_close(); files_refresh();
}
/* ---------- 播放：进程内 ALSA，底部播放条 ---------- */
/* 行内背景进度条：没有底部控制栏，进度只在被播放的那一行上体现 */
static void files_progress_update(const play_state_t *ps){
    if(!g_row_fill) return;
    if(ps->active && ps->total_ms > 0){
        int p = (int)(ps->pos_ms * 100 / ps->total_ms);
        if(p < 0) p = 0; if(p > 100) p = 100;
        if(p != g_row_fill_pct){
            g_row_fill_pct = p;
            int w = ROW_W * p / 100;
            lv_obj_set_width(g_row_fill, w < 4 ? 4 : w);
        }
    }
}

static void play_file(const char *name){
    if(engine_running()){
        show_warn("Stop recording before playing a file.");
        logmsg("play: refused (recording)");
        return;
    }
    if(play_open(g_dir, name) != 0) return;
    snprintf(g_play_file, sizeof g_play_file, "%s", name);
    g_row_fill_pct = -1;
    logmsg("play: %s", name);
    if(page_idx == 1) files_refresh();
}

static void play_row_cb(lv_event_t *e){
    const char *n = (const char*)lv_event_get_user_data(e);
    if(!tap_ok()) return;               /* 滑动 / 弹层刚关的余波都不算点击 */
    if(engine_running()){
        show_warn("Stop recording before playing a file.");
        logmsg("play: refused (recording)");
        return;
    }
    ui_activity();
    play_state_t ps; play_get_state(&ps);
    if(ps.active && !strcmp(n, g_play_file)){
        play_toggle_pause();            /* 点正在播放的行 = 暂停/继续 */
        logmsg("play: %s", ps.paused ? "resume" : "pause");
    } else {
        play_stop();
        play_file(n);
    }
}

static void del_row_cb(lv_event_t *e){
    const char *n=(const char*)lv_event_get_user_data(e);
    if(engine_running()){ lv_label_set_text(state_lbl,"STOP FIRST"); return; }
    snprintf(g_del_name,sizeof g_del_name,"%s",n);
    del_dlg = mk_panel(lv_screen_active(),0,0,W,H,0,C_BG);
    lv_obj_add_flag(del_dlg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = mk_label(del_dlg,"Delete file?",&lv_font_montserrat_20,C_TEXT);
    lv_obj_align(t,LV_ALIGN_TOP_MID,0,96);
    lv_obj_t *sub = mk_label(del_dlg,g_del_name,&lv_font_montserrat_14,C_DIM);
    lv_obj_align(sub,LV_ALIGN_TOP_MID,0,130);
    lv_obj_t *no=lv_button_create(del_dlg); lv_obj_set_size(no,120,56);
    lv_obj_align(no,LV_ALIGN_TOP_MID,-66,170);
    lv_obj_set_style_radius(no,12,0); lv_obj_set_style_bg_color(no,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_shadow_width(no,0,0); lv_obj_set_style_border_width(no,1,0);
    lv_obj_set_style_border_color(no,lv_color_hex(C_LINE),0);
    lv_obj_t *nl=mk_label(no,"CANCEL",&lv_font_montserrat_14,C_TEXT); lv_obj_center(nl);
    lv_obj_add_event_cb(no,(lv_event_cb_t)del_dlg_close,LV_EVENT_CLICKED,NULL);
    lv_obj_t *ok=lv_button_create(del_dlg); lv_obj_set_size(ok,120,56);
    lv_obj_align(ok,LV_ALIGN_TOP_MID,66,170);
    lv_obj_set_style_radius(ok,12,0); lv_obj_set_style_bg_color(ok,lv_color_hex(C_RED),0);
    lv_obj_set_style_shadow_width(ok,0,0);
    lv_obj_t *ol=mk_label(ok,"DELETE",&lv_font_montserrat_14,0x05060A); lv_obj_center(ol);
    lv_obj_add_event_cb(ok,del_ok_cb,LV_EVENT_CLICKED,NULL);
}
static void power_cancel_cb(lv_event_t *e){ (void)e; power_dlg_close(); }
static void power_ok_cb(lv_event_t *e){
    (void)e;
    power_dlg_close();
    /* 标记 + 清崩溃计数 + 同步 + 关机 */
    FILE *f=fopen("/userdata/freE1/poweroff","w"); if(f){ fputs("1\n",f); fclose(f); }
    remove("/userdata/freE1/cnt");   /* 正常关机不算崩溃 */
    sync();
    logmsg("poweroff requested");
    system("poweroff");
}
static void show_power_dialog(void){
    if(power_dlg) return;
    power_dlg = mk_panel(lv_screen_active(),0,0,W,H,0,C_BG);
    lv_obj_add_flag(power_dlg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = mk_label(power_dlg,"Power off?",&lv_font_montserrat_20,C_TEXT);
    lv_obj_align(t,LV_ALIGN_TOP_MID,0,96);
    lv_obj_t *sub = mk_label(power_dlg,"system will sync, then power off",&lv_font_montserrat_14,C_DIM);
    lv_obj_align(sub,LV_ALIGN_TOP_MID,0,128);
    lv_obj_t *no = lv_button_create(power_dlg);
    lv_obj_set_size(no,120,56); lv_obj_align(no,LV_ALIGN_TOP_MID,-66,168);
    lv_obj_set_style_radius(no,12,0);
    lv_obj_set_style_bg_color(no,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_shadow_width(no,0,0);
    lv_obj_set_style_border_width(no,1,0);
    lv_obj_set_style_border_color(no,lv_color_hex(C_LINE),0);
    lv_obj_t *nl=mk_label(no,"CANCEL",&lv_font_montserrat_14,C_TEXT); lv_obj_center(nl);
    lv_obj_add_event_cb(no,power_cancel_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *ok = lv_button_create(power_dlg);
    lv_obj_set_size(ok,120,56); lv_obj_align(ok,LV_ALIGN_TOP_MID,66,168);
    lv_obj_set_style_radius(ok,12,0);
    lv_obj_set_style_bg_color(ok,lv_color_hex(C_RED),0);
    lv_obj_set_style_shadow_width(ok,0,0);
    lv_obj_t *ol=mk_label(ok,"POWER OFF",&lv_font_montserrat_14,0x05060A); lv_obj_center(ol);
    lv_obj_add_event_cb(ok,power_ok_cb,LV_EVENT_CLICKED,NULL);
}

static int64_t g_pwr_down = 0;
static int g_pwr_woke = 0;   /* this press only wakes, no toggle on release */
static int g_pwr_fired = 0;  /* long-press already triggered (fired while still held) */
static lv_timer_t *g_pwr_timer = NULL;
static void pwr_hold_cb(lv_timer_t *t){
    (void)t;
    g_pwr_timer = NULL;
    if(g_pwr_down && !g_pwr_fired){
        g_pwr_fired = 1;
        if(engine_running()) logmsg("power: recording, ignore long press");
        else show_power_dialog();
    }
}
static void pwr_relay_cb(lv_timer_t *t){ (void)t; ui_on_hw_key(7,0); }

void ui_on_swipe(int dx, int dy){
    (void)dy;
    if(!g_screen_on) return;          /* 灭屏只认电源键 */
    if(g_depth>0) return;             /* 子页由 < 返回 */
    if(dx < 0) show_page((page_idx+1)%NPAGES);
    else       show_page((page_idx+2)%NPAGES);
}

void ui_on_hw_key(int code, int pressed){
    if(pressed) g_last_key_code = code;
    if(code==7){
        if(mono_ms() < g_ignore_pwr_until) return;   /* 刚唤醒：忽略这次投递 */
        if(pressed){
            if(!g_screen_on){
                screen_set(1);
                g_pwr_woke = 1;
                ui_activity();
                g_idle_handled = 0;
            } else {
                g_pwr_down = mono_ms();
                g_pwr_fired = 0;
                if(g_pwr_timer){ lv_timer_delete(g_pwr_timer); g_pwr_timer = NULL; }
                g_pwr_timer = lv_timer_create(pwr_hold_cb, 2000, NULL);
                lv_timer_set_repeat_count(g_pwr_timer, 1);
            }
        } else {
            if(g_pwr_timer){ lv_timer_delete(g_pwr_timer); g_pwr_timer = NULL; }
            if(g_pwr_woke){ g_pwr_woke = 0; return; }
            if(g_pwr_fired){ g_pwr_fired = 0; g_pwr_down = 0; return; }  /* 弹层已出，松手不再处理 */
            if(g_pwr_down){
                g_pwr_down = 0;
                screen_set(0);                          /* 短按主动灭屏（录音中也允许） */
                g_off_at = mono_ms();                   /* 计入休眠倒计时 */
                logmsg("power: screen off (manual)");
            }
        }
        return;
    }
    if(!pressed) return;
    ui_activity();
    g_idle_handled = 0;
    if(!g_screen_on){ screen_set(1); return; }   /* 灭屏时任何键先唤醒 */
    switch(code){
    case 2:  logmsg("key: MARK (no action yet)"); break;   /* KEY_1 MARK：不做换页，书签功能待做 */
    case 3:  rec_stop_all(); break;                         /* KEY_2 STOP：停止录音/暂停，无录音则停回放 */
    case 4:  rec_pause_cycle(); break;                      /* KEY_3 REC/PAUSE：录 → 暂停 → 继续 */
    case 7:  /* KEY_6 POWER：短按亮灭屏，长按 2 s 确认关机 */
        break;
    default: break;
    }
}
int ui_get_page(void){ return page_idx; }

/* ---- 自动化自测：注入假指针设备，验证滑动路径 ---- */
static lv_point_t t_pt; static int t_step, t_dir; static int t_bx=40, t_by=200;
static void fake_read(lv_indev_t *indev, lv_indev_data_t *data){
    (void)indev;
    int step = t_step++;
    if(step < 3){ t_pt.x=t_bx; t_pt.y=t_by; data->state=LV_INDEV_STATE_PRESSED; }
    else if(step < 18){
        data->state=LV_INDEV_STATE_PRESSED;
        t_pt.x = t_bx + (t_dir==0 ? -(step-3)*8 : (t_dir==2 ? (step-3)*8 : 0));
        t_pt.y = t_by - (t_dir==1 ? (step-3)*8 : 0);
    } else {
        data->state=LV_INDEV_STATE_RELEASED;
    }
    data->point = t_pt;
}
int ui_test_swipe(void){
    lv_init();
    lv_display_t *d=lv_display_create(W,H);
    lv_display_set_flush_cb(d,flush_cb);
    lv_display_set_buffers(d,g_draw,NULL,sizeof g_draw,LV_DISPLAY_RENDER_MODE_FULL);
    build_ui();
    lv_indev_t *in=lv_indev_create();
    lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in,fake_read);
    t_step=0; t_dir=3;
    for(int i=0;i<40;i++){ lv_tick_inc(33); lv_timer_handler(); }

    int p0=page_idx;
    t_step=0; t_dir=0;                      /* 向左滑 -> 下一页 */
    for(int i=0;i<60;i++){ lv_tick_inc(33); lv_timer_handler(); }
    int p1=page_idx;
    printf("swipetest: swipe-left   page %d -> %d %s\n", p0, p1, (p1==(p0+1)%NPAGES)?"OK":"FAIL");

    t_step=0; t_dir=2;                      /* 向右滑 -> 上一页 */
    int q0=page_idx;
    for(int i=0;i<60;i++){ lv_tick_inc(33); lv_timer_handler(); }
    int q1=page_idx;
    printf("swipetest: swipe-right  page %d -> %d %s\n", q0, q1, (q1==(q0+2)%NPAGES)?"OK":"FAIL");

    t_step=0; t_dir=1;                      /* 向上滑 -> 不翻页 */
    int r0=page_idx;
    for(int i=0;i<60;i++){ lv_tick_inc(33); lv_timer_handler(); }
    int r1=page_idx;
    printf("swipetest: swipe-up     page %d -> %d %s\n", r0, r1, (r1==r0)?"OK":"FAIL");

    /* 峰值复位：先回到录音页，抬高峰值，再模拟点击音量区 */
    show_page(0);
    for(int i=0;i<10;i++){ lv_tick_inc(33); lv_timer_handler(); }
    meter_update(&meters[0], 80);
    int peak_before = meters[0].max;
    t_step=0; t_dir=3; t_bx=40; t_by=100;
    for(int i=0;i<40;i++){ lv_tick_inc(33); lv_timer_handler(); }
    int peak_after = meters[0].max;
    printf("swipetest: tap-reset    peak %d -> %d %s\n", peak_before, peak_after, (peak_before>0 && peak_after==0)?"OK":"FAIL");

    return (p1==(p0+1)%3 && q1==(q0+2)%3 && r1==r0 && peak_before>0 && peak_after==0) ? 0 : 1;
}

#include "hal.h"

void ui_on_hw_key(int code, int pressed);
static uint32_t dev_tick_cb(void){ return (uint32_t)mono_ms(); }

int ui_run_device(const char *dir, int http_port){
    snprintf(g_dir,sizeof g_dir,"%s",dir);
    g_device = 1;
    g_start_ms = mono_ms();
    { int n=rec_recover(g_dir); if(n) logmsg("startup: recovered %d file(s)",n); }
    idle_load();
    fmt_load();
    mode_load();
    guard_load();
    hs_motor(false);   /* 清掉上次可能卡住的振动 */
    hs_audio_route(hs_hp_inserted());   /* 耳机/扬声器路由（驱动可能把 HPOUT 留在 0） */
    lv_init();
    lv_tick_set_cb(dev_tick_cb);
    lv_display_t *d = hal_disp_fb_init();
    if(!d){ logmsg("device display init failed"); return -1; }
    hal_input_init();
    hal_set_key_cb(ui_on_hw_key);
    hal_set_swipe_cb(ui_on_swipe);
    hal_set_touch_act_cb(ui_activity);
    build_ui();          /* 直接出主界面，不做开机图 */
    g_group = lv_group_create();
    lv_group_set_default(g_group);
    for(int i=0;i<3;i++){
        lv_group_add_obj(g_group, pages[i]);
        lv_obj_add_event_cb(pages[i], key_cb, LV_EVENT_KEY, NULL);
    }
    lv_group_focus_obj(pages[0]);
    if(http_port>0 && http_start(g_dir,http_port)==0){
        g_http_port=http_port;
        if(http_val){                                   /* 启动后回填 IP:port */
            wifi_state ws; char ip[32] = "";
            if(wifi_status(&ws)==0 && ws.ip[0]) snprintf(ip,sizeof ip,"%s", ws.ip);
            char hb[48];
            if(ip[0]) snprintf(hb,sizeof hb,"%s:%d", ip, http_port);
            else      snprintf(hb,sizeof hb,"ON :%d", http_port);
            lv_label_set_text(http_val, hb);
            logmsg("http: display '%s'", hb);
            lv_obj_set_style_text_color(http_val, lv_color_hex(C_GREEN), 0);
        }
    }
    ui_activity();
    lv_timer_create(ui_tick,200,NULL);
    logmsg("device UI up: dir=%s http=%d", g_dir, g_http_port);
    while(1){ lv_timer_handler(); usleep(5000); }
    return 0;
}

#ifdef FREEL1_NO_X11
int ui_run_x11(const char *dir, int device, int http_port){ (void)dir;(void)device;(void)http_port; return -1; }
#else
int ui_run_x11(const char *dir, int device, int http_port){
    snprintf(g_dir,sizeof g_dir,"%s",dir);
    g_device = device;
    { int n=rec_recover(g_dir); if(n) logmsg("startup: recovered %d file(s)",n); }
    idle_load();
    fmt_load();
    guard_load();
    hs_motor(false);   /* 清掉上次可能卡住的振动 */
    if(http_port>0 && http_start(g_dir,http_port)==0) g_http_port=http_port;
    lv_init();
    lv_display_t *d=lv_x11_window_create("freE1 - 320x320",W,H);
    if(!d){ logmsg("x11 display create failed"); return -1; }
    lv_x11_inputs_create(d,NULL);
    build_ui();
    g_group = lv_group_create();
    lv_group_set_default(g_group);
    for(int i=0;i<3;i++){
        lv_group_add_obj(g_group, pages[i]);
        lv_obj_add_event_cb(pages[i], key_cb, LV_EVENT_KEY, NULL);
    }
    lv_group_focus_obj(pages[0]);

    lv_obj_t *hint = lv_label_create(lv_screen_active());
    lv_label_set_text(hint, "drag left/right to switch page   |   arrows");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A3F4A), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_timer_create(ui_tick,200,NULL);
    logmsg("UI running - drag left/right: pages, click REC/mode. HTTP:%s", g_http_port? "on":"off");
    while(1){ lv_timer_handler(); usleep(5000); }
    return 0;
}
#endif
