#include "wifi_ui.h"
#include "wifi.h"
#include "common.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define C_BG    0x05060A
#define C_PANEL 0x0D0F14
#define C_LINE  0x1E222B
#define C_TEXT  0xE8E8E9
#define C_DIM   0x6B7280
#define C_GREEN 0x3CFF7A
#define C_RED   0xFF3B30

static lv_obj_t *status_lbl, *list_box, *scan_btn_lbl;
static char pwd_buf[64];
static int  pwd_buf_len = 0;
static lv_obj_t *overlay, *pwd_ta, *kb, *ssid_lbl;
static char g_sel_ssid[64];
static volatile int g_scan_busy = 0, g_scan_done = 0;
static volatile int g_conn_busy = 0, g_conn_result = 0;
static wifi_ap g_aps[16];
static int g_naps = 0;

static lv_obj_t *lab(lv_obj_t *p, const char *t, const lv_font_t *f, uint32_t c){
    lv_obj_t *l=lv_label_create(p); lv_label_set_text(l,t);
    if(f) lv_obj_set_style_text_font(l,f,0);
    lv_obj_set_style_text_color(l,lv_color_hex(c),0);
    return l;
}
static lv_obj_t *panel(lv_obj_t *p,int x,int y,int w,int h,int r,uint32_t bg){
    lv_obj_t *o=lv_obj_create(p); lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(bg),0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,r,0);
    return o;
}

static void refresh_status(void){
    wifi_state st; wifi_status(&st);
    char b[128];
    if(!st.available) snprintf(b,sizeof b,"no wpa_cli");
    else if(st.connected) snprintf(b,sizeof b,"%s  |  %s", st.ssid, st.ip);
    else snprintf(b,sizeof b,"not connected");
    if(status_lbl) lv_label_set_text(status_lbl,b);
}

static void *scan_thread(void *a){
    (void)a;
    logmsg("scan_thread: avail=%d", wifi_available());
    g_naps = wifi_scan(g_aps, 16);
    logmsg("scan_thread: got %d", g_naps);
    g_scan_done = 1;
    return NULL;
}
static void start_scan(void){
    if(g_scan_busy) return;
    g_scan_busy = 1; g_scan_done = 0;
    pthread_t t; pthread_create(&t,NULL,scan_thread,NULL); pthread_detach(t);
    if(scan_btn_lbl) lv_label_set_text(scan_btn_lbl,"SCANNING");
}

static void cancel_btn_cb(lv_event_t *e);
static void t9_commit(void);
static void t9_update_field(void);

static void close_overlay(void){
    if(overlay){ lv_obj_delete(overlay); overlay=NULL; kb=NULL; pwd_ta=NULL; }
}

static void *connect_thread(void *arg){
    (void)arg;
    static char pwd[64];
    snprintf(pwd,sizeof pwd,"%s", pwd_buf);
    int ok = (wifi_connect(g_sel_ssid, pwd)==0);
    g_conn_result = ok ? 1 : -1;
    g_conn_busy = 0;
    return NULL;
}

static void overlay_ready_cb(lv_event_t *e){
    (void)e;
    if(g_conn_busy) return;
    t9_commit(); t9_update_field();
    g_conn_busy = 1;
    pthread_t t; pthread_create(&t,NULL,connect_thread,NULL); pthread_detach(t);
}
static void overlay_cancel_cb(lv_event_t *e){ (void)e; close_overlay(); }

/* ---------- 分离式键盘：字母模式（多点触按）/ 数字模式（直接输入）/ 符号模式 ---------- */
#define T9_NKEYS 12
static const char *M_LOWER[9] = {".,!?","abc","def","ghi","jkl","mno","pqrs","tuv","wxyz"};
static const char *M_UPPER[9] = {".,!?","ABC","DEF","GHI","JKL","MNO","PQRS","TUV","WXYZ"};
static const char *M_DIGIT[9] = {"1","2","3","4","5","6","7","8","9"};

static int  t9_mode = 0;          /* 0=123 1=abc 2=ABC */
static int  t9_last_key = -1, t9_idx = 0;
static int64_t t9_last_ms = 0;
static char t9_pending = 0;
static lv_obj_t *t9_btns[T9_NKEYS];
static lv_obj_t *t9_lbls[T9_NKEYS];

static void t9_commit(void){
    if(t9_pending && pwd_buf_len < (int)sizeof pwd_buf - 1){
        pwd_buf[pwd_buf_len++] = t9_pending;
        pwd_buf[pwd_buf_len] = 0;
    }
    t9_pending = 0; t9_last_key = -1; t9_idx = 0;
}

static void t9_update_field(void){
    if(!pwd_ta) return;
    char view[80];
    snprintf(view,sizeof view,"%s", pwd_buf);
    if(t9_pending){                        /* 待定字符立刻可见 */
        size_t n = strlen(view);
        if(n < sizeof view - 1){ view[n] = t9_pending; view[n+1] = 0; }
    }
    lv_textarea_set_text(pwd_ta, view);
}

static void t9_refresh_keys(void){
    for(int i=0;i<9;i++){
        const char *t = (t9_mode==0)?M_DIGIT[i] : (t9_mode==1)?M_LOWER[i] : M_UPPER[i];
        if(t9_lbls[i]) lv_label_set_text(t9_lbls[i], t);
    }
    if(t9_lbls[9])  lv_label_set_text(t9_lbls[9],  t9_mode==0?"abc":(t9_mode==1?"ABC":"123"));
    if(t9_lbls[10]) lv_label_set_text(t9_lbls[10], (t9_mode==0)?"0":"space");
    if(t9_lbls[11]) lv_label_set_text(t9_lbls[11], "< del");
}

static void t9_insert(char c){
    if(pwd_buf_len < (int)sizeof pwd_buf - 1){ pwd_buf[pwd_buf_len++] = c; pwd_buf[pwd_buf_len] = 0; }
    t9_update_field();
}

static void t9_press(int k){
    if(k == 9){                       /* 123 → abc → ABC → 123 */
        t9_commit(); t9_mode = (t9_mode + 1) % 3; t9_refresh_keys(); t9_update_field(); return;
    }
    if(k == 11){                      /* 退格 */
        t9_commit();
        if(pwd_buf_len > 0){ pwd_buf_len--; pwd_buf[pwd_buf_len] = 0; }
        t9_update_field(); return;
    }
    if(k == 10){                      /* 0 或空格 */
        t9_commit();
        t9_insert(t9_mode==0 ? '0' : ' ');
        return;
    }

    if(t9_mode == 0){                 /* 数字：直接输入 */
        t9_commit();
        t9_insert(M_DIGIT[k][0]);
        return;
    }

    /* 字母模式：多点触按 */
    int64_t now = lv_tick_get();
    if(now - t9_last_ms > 900) t9_commit();
    t9_last_ms = now;
    const char *chars = (t9_mode==2)? M_UPPER[k] : M_LOWER[k];
    size_t n = strlen(chars);
    if(t9_last_key == k) t9_idx = (int)((t9_idx + 1) % n);
    else { t9_commit(); t9_last_key = k; t9_idx = 0; }
    t9_pending = chars[t9_idx];
    t9_update_field();
}

static void t9_timer_cb(lv_timer_t *t){
    (void)t;
    if(t9_pending && lv_tick_get() - t9_last_ms > 900){ t9_commit(); t9_update_field(); }
}

void wifi_t9_press(int k){ t9_press(k); }
int  wifi_overlay_open(void){ return overlay != NULL; }

static void t9_key_cb(lv_event_t *e){
    t9_press((int)(intptr_t)lv_event_get_user_data(e));
}

static void open_password(const char *ssid){
    g_sel_ssid[0]=0; snprintf(g_sel_ssid,sizeof g_sel_ssid,"%s",ssid);
    pwd_buf_len = 0; pwd_buf[0] = 0; t9_pending = 0; t9_last_key = -1; t9_mode = 0;  /* 默认数字 */
    overlay = panel(lv_screen_active(),0,0,320,320,0,C_BG);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    /* 返回键：与其它子页一致 */
    lv_obj_t *bb=lv_button_create(overlay);
    lv_obj_set_size(bb,48,34); lv_obj_set_pos(bb,6,2);
    lv_obj_set_style_bg_color(bb,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_radius(bb,13,0);
    lv_obj_set_style_shadow_width(bb,0,0);
    lv_obj_set_style_border_width(bb,1,0);
    lv_obj_set_style_border_color(bb,lv_color_hex(C_LINE),0);
    lv_obj_t *bl=lab(bb,"<",&lv_font_montserrat_14,C_GREEN);
    lv_obj_center(bl);
    lv_obj_add_event_cb(bb,cancel_btn_cb,LV_EVENT_CLICKED,NULL);

    ssid_lbl = lab(overlay, ssid, &lv_font_montserrat_14, C_GREEN);
    lv_obj_set_width(ssid_lbl, 190);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_RIGHT, -8, 10);

    /* 输入框 + 右侧 CONNECT */
    pwd_ta = lv_textarea_create(overlay);
    lv_obj_set_size(pwd_ta, 196, 40);
    lv_obj_align(pwd_ta, LV_ALIGN_TOP_LEFT, 8, 44);
    lv_textarea_set_one_line(pwd_ta, true);
    lv_obj_set_style_bg_color(pwd_ta, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_text_color(pwd_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(pwd_ta, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_text_font(pwd_ta, &lv_font_montserrat_20, 0);

    lv_obj_t *ok=lv_button_create(overlay);
    lv_obj_set_size(ok,106,40); lv_obj_align(ok,LV_ALIGN_TOP_RIGHT,-8,44);
    lv_obj_set_style_bg_color(ok,lv_color_hex(C_GREEN),0);
    lv_obj_set_style_radius(ok,10,0);
    lv_obj_set_style_shadow_width(ok,0,0);
    lv_obj_t *ol=lab(ok,"CONNECT",&lv_font_montserrat_14,C_BG); lv_obj_center(ol);
    lv_obj_add_event_cb(ok,overlay_ready_cb,LV_EVENT_CLICKED,NULL);

    for(int i=0;i<T9_NKEYS;i++){
        int col = i % 3, row = i / 3;
        lv_obj_t *b = lv_button_create(overlay);
        lv_obj_set_size(b, 100, 50);
        lv_obj_set_pos(b, 6 + col*104, 96 + row*56);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x141922), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3648), LV_STATE_PRESSED);
        t9_btns[i] = b;
        t9_lbls[i] = lab(b, "", &lv_font_montserrat_14, C_TEXT);
        lv_obj_center(t9_lbls[i]);
        lv_obj_add_event_cb(b, t9_key_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    t9_refresh_keys();
    lv_timer_create(t9_timer_cb, 200, NULL);
}

int hal_input_last_drag(void);

static void ap_click_cb(lv_event_t *e){
    if(hal_input_last_drag()) return;   /* 滑动不算点击 */
    const char *ssid = (const char*)lv_event_get_user_data(e);
    open_password(ssid);
}

static void rebuild_list(void){
    if(!list_box) return;
    lv_obj_clean(list_box);
    if(g_naps==0){
        lv_obj_t *l=lab(list_box,"no networks - tap SCAN",&lv_font_montserrat_14,C_DIM);
        lv_obj_align(l,LV_ALIGN_TOP_MID,0,20);
        return;
    }
    static char names[16][64];
    int y=0;
    for(int i=0;i<g_naps;i++){
        snprintf(names[i],sizeof names[i],"%s",g_aps[i].ssid);
        lv_obj_t *row=panel(list_box,8,y,304,72,12,C_PANEL);
        lv_obj_t *n=lab(row,names[i],&lv_font_montserrat_20,C_TEXT);
        lv_label_set_long_mode(n, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(n, 196);
        lv_obj_align(n,LV_ALIGN_LEFT_MID,12,0);
        char sig[24]; snprintf(sig,sizeof sig,"%d dBm%s",g_aps[i].signal, g_aps[i].secure?"  *":"");
        lv_obj_t *s=lab(row,sig,&lv_font_montserrat_14,C_DIM);
        lv_obj_align(s,LV_ALIGN_RIGHT_MID,-12,0);
        lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row,LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(row,ap_click_cb,LV_EVENT_CLICKED,(void*)names[i]);
        y+=74;
    }
}

static void scan_btn_cb(lv_event_t *e){ (void)e; start_scan(); }

static void cancel_btn_cb(lv_event_t *e){ (void)e; close_overlay(); }

lv_obj_t *wifi_page_create(lv_obj_t *parent){
    lv_obj_t *title=lab(parent,"WIFI",&lv_font_montserrat_14,C_DIM);
    lv_obj_align(title,LV_ALIGN_TOP_MID,0,10);

    status_lbl=lab(parent,"--",&lv_font_montserrat_14,C_GREEN);
    lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(status_lbl, 300);
    lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_lbl,LV_ALIGN_TOP_MID,0,62);   /* 单独一行，不挡按钮 */

    lv_obj_t *sb=lv_button_create(parent);
    lv_obj_set_size(sb,120,52);
    lv_obj_align(sb,LV_ALIGN_TOP_RIGHT,-8,8);
    lv_obj_set_style_radius(sb,14,0);
    lv_obj_set_style_bg_color(sb,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_shadow_width(sb,0,0);
    scan_btn_lbl=lab(sb,"SCAN",&lv_font_montserrat_20,C_GREEN);
    lv_obj_center(scan_btn_lbl);
    lv_obj_add_event_cb(sb,scan_btn_cb,LV_EVENT_CLICKED,NULL);



    list_box=panel(parent,0,84,320,236,0,C_BG);
    lv_obj_add_flag(list_box,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_box,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box,LV_SCROLLBAR_MODE_OFF);
    refresh_status();
    rebuild_list();
    return parent;
}

void wifi_ui_trigger_scan(void){ start_scan(); }
void wifi_ui_scroll(int dy){ if(list_box) lv_obj_scroll_by(list_box,0,dy,LV_ANIM_OFF); }
void wifi_ui_demo_open(const char *ssid){ open_password(ssid); }
void wifi_ui_demo_close(void){ close_overlay(); }

void wifi_page_on_show(void){
    refresh_status();
    if(g_scan_busy || g_scan_done==0){ start_scan(); }
}

/* 由 ui.c 的 200ms tick 调用 */
void wifi_page_tick(void){
    if(g_scan_busy && g_scan_done){
        g_scan_busy=0;
        if(scan_btn_lbl) lv_label_set_text(scan_btn_lbl,"SCAN");
        rebuild_list();
        refresh_status();
    }
    if(!g_conn_busy && g_conn_result!=0){
        close_overlay();
        refresh_status();
        g_conn_result = 0;
    }
}
