#include "pages.h"
#include "app.h"

#include "input.h"
#include "hal_keys.h"
#include "hal_led.h"
#include "hal_motor.h"
#include "hal_hp.h"
#include "hal_audio.h"
#include "report.h"
#include "display_drm.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>

#define NC 320
#define NH 320
#define BAR_H 26
#define NAV_Y 296

static lv_obj_t *g_scr;
static lv_obj_t *g_pages[5];
static lv_obj_t *g_title;
static lv_obj_t *g_hp;
static lv_obj_t *g_batt;
static lv_obj_t *g_dots[6];
static lv_obj_t *g_tearbar;
static lv_obj_t *g_hint;
static bool g_tear;
static int g_tear_x;
static int g_page = -1;

/* touch page */
static lv_obj_t *g_cell[8][8];
static lv_obj_t *g_vline, *g_hline;
static lv_obj_t *g_touch_info;
static int g_cell_painted[8][8];

/* keys page */
static lv_obj_t *g_keychip[E1K_COUNT];
static lv_obj_t *g_keycount[E1K_COUNT];
static lv_obj_t *g_keyhold;
static lv_obj_t *g_leddots[7];
static lv_obj_t *g_ledpwm_lbl;
static lv_obj_t *g_pwrwarn;

/* loopback page */
static lv_obj_t *g_rec_btn;
static lv_obj_t *g_rec_lbl;
static lv_obj_t *g_loop_status;
static lv_obj_t *g_env[48];
static lv_obj_t *g_loop_info;
static bool g_autoplay_pending;

/* mics page */
static lv_obj_t *g_mic[6];
static lv_obj_t *g_mic_ring[6];
static lv_obj_t *g_mic_lbl[6];
static lv_obj_t *g_dir_lbl;

/* vib page */
static lv_obj_t *g_vib_ring;
static lv_obj_t *g_vib_status;

/* verdict buttons: [page][0=PASS,1=FAIL] */
static lv_obj_t *g_sum_row[5];
static lv_obj_t *g_sum_sub[5];
static lv_obj_t *g_serial_lbl;

static const char *g_names[ITEM_COUNT] = { "TOUCH", "KEYS / LED", "LOOPBACK", "MIC ARRAY", "VIBRATE" };
static const char *g_short[ITEM_COUNT] = { "touch", "keys+led", "loopback", "mic array", "vibrate" };

static lv_obj_t *mk_page(void)
{
    lv_obj_t *p = lv_obj_create(g_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_size(p, NC, NAV_Y - BAR_H);
    lv_obj_set_pos(p, 0, BAR_H);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *mk_lbl(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                        uint32_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    return o;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, int x, int y, int w, int h, const char *txt,
                        lv_event_cb_t cb, void *ud, uint32_t color)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    if(cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void dot_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    pages_show(i);
}

static void build_chrome(void)
{
    g_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(g_scr);
    lv_obj_set_style_bg_color(g_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);

    g_title = mk_lbl(g_scr, "TOUCH", &lv_font_montserrat_14, 0xffffff, 6, 5);
    g_hp = mk_lbl(g_scr, "HP OUT", &lv_font_montserrat_14, 0x666666, 196, 5);
    g_batt = mk_lbl(g_scr, "---%", &lv_font_montserrat_14, 0x888888, 272, 5);

    for(int i = 0; i < 5; i++) {
        lv_obj_t *d = lv_obj_create(g_scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x444444), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_size(d, 7, 7);
        lv_obj_set_pos(d, 320 / 2 - 24 + i * 9, NAV_Y + 8);
        lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(d, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_dots[i] = d;
    }
    g_hint = mk_lbl(g_scr, "2-finger swipe", &lv_font_montserrat_14, 0x333333, 6, NAV_Y + 6);

    /* tear test bar: .cmd tear toggles a fast horizontal sweep */
    g_tearbar = mk_box(g_scr, 0, BAR_H, 10, NAV_Y - BAR_H, 0xffffff, 0);
    lv_obj_add_flag(g_tearbar, LV_OBJ_FLAG_HIDDEN);
}

static void update_chrome(void)
{
    static int last_page = -2, last_hp = -1, last_cap = -2;

    if(last_page != g_page) {
        last_page = g_page;
        lv_label_set_text(g_title, (g_page >= 0 && g_page < 5) ? g_names[g_page] : "");
        if(g_hint) lv_label_set_text(g_hint, g_page == 4 ? "swipe up to exit" : "2-finger swipe");
        for(int i = 0; i < 5; i++) {
            uint32_t c = (i == g_page) ? 0xffffff : 0x444444;
            lv_obj_set_style_bg_color(g_dots[i], lv_color_hex(c), 0);
        }
    }

    int hpi = hp_inserted() ? 1 : 0;
    if(hpi != last_hp) {
        last_hp = hpi;
        lv_label_set_text(g_hp, hpi ? "HP IN" : "HP OUT");
        lv_obj_set_style_text_color(g_hp, lv_color_hex(hpi ? 0x40ff40 : 0x555555), 0);
    }

    static uint32_t last;
    uint32_t now = e1_now_ms();
    if(now - last > 1000) {
        last = now;
        int cap = -1;
        FILE *f = fopen("/sys/class/power_supply/battery/capacity", "r");
        if(f) { if(fscanf(f, "%d", &cap) != 1) cap = -1; fclose(f); }
        if(cap >= 0 && cap != last_cap) {
            last_cap = cap;
            lv_label_set_text_fmt(g_batt, "%d%%", cap);
        }
    }
}

/* ---------------- page 0: touch ---------------- */

static void clear_touch(lv_event_t *e)
{
    (void)e;
    e1_input_reset_stats();
    memset(g_cell_painted, 0, sizeof(g_cell_painted));
    for(int r = 0; r < 8; r++)
        for(int c = 0; c < 8; c++)
            lv_obj_set_style_bg_color(g_cell[r][c], lv_color_hex(0x101010), 0);
}

static void build_touch(lv_obj_t *p)
{
    for(int r = 0; r < 8; r++) {
        for(int c = 0; c < 8; c++) {
            g_cell[r][c] = mk_box(p, 4 + c * 39, 6 + r * 28, 38, 27, 0x101010, 2);
        }
    }
    g_vline = mk_box(p, 0, 0, 2, 240, 0x00ff00, 0);
    g_hline = mk_box(p, 0, 0, 320, 2, 0x00ff00, 0);
    g_touch_info = mk_lbl(p, "cells 0/64   pts 0", &lv_font_montserrat_14, 0xaaaaaa, 4, 232);
    mk_btn(p, 4, 252, 110, 32, "CLEAR", clear_touch, NULL, 0x2a2a2a);
}

static void update_touch(void)
{
    static int lx = -1, ly = -1, lc = -1, lp = -1;
    const e1_input_stats_t *st = e1_input_stats();

    if(st->x != lx) { lx = st->x; lv_obj_set_pos(g_vline, lx, 0); }
    if(st->y != ly) { ly = st->y; lv_obj_set_pos(g_hline, 0, ly); }
    if(st->cells != lc || st->max_contacts != lp) {
        lc = st->cells;
        lp = st->max_contacts;
        lv_label_set_text_fmt(g_touch_info, "cells %d/64   pts %d", lc, lp);
    }

    int mask = e1_input_cell_mask();
    for(int r = 0; r < 8; r++) {
        for(int c = 0; c < 8; c++) {
            int on = (mask >> (r * 8 + c)) & 1;
            if(on != g_cell_painted[r][c]) {
                g_cell_painted[r][c] = on;
                lv_obj_set_style_bg_color(g_cell[r][c], lv_color_hex(on ? 0x0f4d0f : 0x101010), 0);
            }
        }
    }
    snprintf(g_app.items[ITEM_TOUCH].evidence, sizeof(g_app.items[ITEM_TOUCH].evidence),
             "cells %d/64, max pts %d", st->cells, st->max_contacts);
}

/* ---------------- page 1: keys + led ---------------- */

static void led_color_cb(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    led_color(c);
    led_top(true);
    for(int i = 0; i < 7; i++) {
        lv_obj_set_style_border_width(g_leddots[i], 2, 0);
        lv_obj_set_style_border_color(g_leddots[i], lv_color_hex(i == c ? 0xffffff : 0x3a3a3a), 0);
    }
}

static void pwm_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(s);
    led_pwm(v);
    lv_label_set_text_fmt(g_ledpwm_lbl, "%d", v);
}

static void keyled_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    led_key(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void build_keys(lv_obj_t *p)
{
    static const uint32_t chip_colors[E1K_COUNT] = { 0x1b2a3a, 0x1b2a3a, 0x3a1b1b, 0x1b3a1b, 0x1b3a1b, 0x3a2a1b };
    for(int i = 0; i < E1K_COUNT; i++) {
        int x = 8 + (i % 3) * 104;
        int y = 2 + (i / 3) * 48;
        g_keychip[i] = mk_box(p, x, y, 96, 42, chip_colors[i], 6);
        lv_obj_set_style_border_width(g_keychip[i], 2, 0);
        lv_obj_set_style_border_color(g_keychip[i], lv_color_hex(0x000000), 0);
        mk_lbl(g_keychip[i], keys_name((e1_key_t)i), &lv_font_montserrat_14, 0xdddddd, 6, 4);
        g_keycount[i] = mk_lbl(g_keychip[i], "0", &lv_font_montserrat_20, 0xffffff, 66, 16);
    }
    g_keyhold = mk_lbl(p, "", &lv_font_montserrat_14, 0xffcc00, 4, 96);
    g_pwrwarn = mk_lbl(p, "", &lv_font_montserrat_14, 0xff4040, 4, 112);

    mk_lbl(p, "TOP LED", &lv_font_montserrat_14, 0x888888, 4, 130);
    for(int i = 0; i < 7; i++) {
        static const uint32_t cols[7] = { 0x000000, 0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0xff00ff, 0x00ffff };
        g_leddots[i] = mk_box(p, 6 + i * 44, 146, 30, 30, cols[i], LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_color(g_leddots[i], lv_color_hex(i == 2 ? 0xffffff : 0x3a3a3a), 0);
        lv_obj_set_style_border_width(g_leddots[i], i == 2 ? 2 : 1, 0);
        lv_obj_add_flag(g_leddots[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_leddots[i], led_color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lv_obj_t *s = lv_slider_create(p);
    lv_obj_set_size(s, 230, 8);
    lv_obj_set_pos(s, 6, 186);
    lv_slider_set_range(s, 0, 255);
    lv_slider_set_value(s, led_pwm_get(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s, pwm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    g_ledpwm_lbl = mk_lbl(p, "127", &lv_font_montserrat_14, 0xffffff, 248, 180);

    lv_obj_t *sw = lv_switch_create(p);
    lv_obj_set_size(sw, 44, 22);
    lv_obj_set_pos(sw, 6, 204);
    lv_obj_add_event_cb(sw, keyled_cb, LV_EVENT_VALUE_CHANGED, NULL);
    mk_lbl(p, "KEY LED", &lv_font_montserrat_14, 0x888888, 58, 206);

}

static void update_keys(void)
{
    int pressed = 0;
    char hold[64] = "";
    for(int i = 0; i < E1K_COUNT; i++) {
        bool down = keys_down((e1_key_t)i);
        uint32_t h = keys_hold_ms((e1_key_t)i);
        static int last_down[E1K_COUNT] = { -1, -1, -1, -1, -1, -1 };
        static int last_cnt[E1K_COUNT] = { -1, -1, -1, -1, -1, -1 };
        int dn = down ? 1 : 0;
        if(dn != last_down[i]) {
            last_down[i] = dn;
            lv_obj_set_style_border_color(g_keychip[i],
                                          lv_color_hex(down ? 0x00ff00 : 0x000000), 0);
        }
        int cnt = keys_count((e1_key_t)i);
        if(cnt != last_cnt[i]) {
            last_cnt[i] = cnt;
            lv_label_set_text_fmt(g_keycount[i], "%d", cnt);
        }
        if(down) {
            pressed++;
            snprintf(hold, sizeof(hold), "%s held %ums", keys_name((e1_key_t)i), (unsigned)h);
        }
    }
    lv_label_set_text(g_keyhold, hold);
    lv_label_set_text(g_pwrwarn, keys_hold_ms(E1K_PWR) > 2000 ? "POWER HELD >2s: release now!" : "");

    int seen = 0;
    for(int i = 0; i < E1K_COUNT; i++) if(keys_count((e1_key_t)i) > 0) seen++;
    snprintf(g_app.items[ITEM_KEYS].evidence, sizeof(g_app.items[ITEM_KEYS].evidence),
             "%d/6 keys pressed, pwr hold %ums", seen, (unsigned)keys_last_hold(E1K_PWR));
}

/* ---------------- page 2: loopback ---------------- */

static void rec_cb(lv_event_t *e)
{
    (void)e;
    if(audio_recording()) { audio_rec_cancel(); }
    else { audio_rec_start(); g_autoplay_pending = true; }
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    if(audio_rec_done()) audio_play();
}

static void build_loop(lv_obj_t *p)
{
    g_loop_status = mk_lbl(p, "READY", &lv_font_montserrat_14, 0xffffff, 4, 4);

    g_rec_btn = lv_button_create(p);
    lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0x8a1f1f), 0);
    lv_obj_set_style_radius(g_rec_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(g_rec_btn, 110, 110);
    lv_obj_set_pos(g_rec_btn, 105, 20);
    g_rec_lbl = lv_label_create(g_rec_btn);
    lv_label_set_text(g_rec_lbl, "REC");
    lv_obj_set_style_text_font(g_rec_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(g_rec_lbl);
    lv_obj_add_event_cb(g_rec_btn, rec_cb, LV_EVENT_CLICKED, NULL);

    for(int i = 0; i < 48; i++) {
        g_env[i] = mk_box(p, 4 + i * 6, 176, 4, 2, 0x1f6f1f, 0);
    }

    g_loop_info = mk_lbl(p, "", &lv_font_montserrat_14, 0xaaaaaa, 4, 182);

    mk_btn(p, 4, 250, 110, 32, "PLAY", play_cb, NULL, 0x1f4f7a);
    mk_lbl(p, "speak / tap during the 5 s", &lv_font_montserrat_14, 0x555555, 122, 256);
}

static void update_loop(void)
{
    if(audio_recording()) {
        lv_label_set_text_fmt(g_loop_status, "REC  %d%%", (int)(audio_rec_progress() * 100));
        lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0xff2020), 0);
        lv_label_set_text(g_rec_lbl, "STOP");
    }
    else if(audio_playing()) {
        lv_label_set_text_fmt(g_loop_status, "PLAY %d%%", (int)(audio_play_progress() * 100));
        lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0x8a1f1f), 0);
        lv_label_set_text(g_rec_lbl, "REC");
    }
    else if(audio_rec_done()) {
        lv_label_set_text(g_loop_status, "DONE");
        lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0x8a1f1f), 0);
        lv_label_set_text(g_rec_lbl, "REC");
    }
    else {
        lv_label_set_text(g_loop_status, "READY");
        lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0x8a1f1f), 0);
        lv_label_set_text(g_rec_lbl, "REC");
    }

    if(g_autoplay_pending && audio_rec_done() && !audio_playing()) {
        g_autoplay_pending = false;
        audio_play();
    }

    if(audio_rec_done()) {
        int n = 0;
        const float *env = audio_envelope(&n);
        for(int i = 0; i < 48 && i < n; i++) {
            int h = 2 + (int)(env[i] * 38.0f);
            lv_obj_set_size(g_env[i], 4, h);
            lv_obj_set_y(g_env[i], 176 - h);
        }
    }
    {
        char b[96];
        snprintf(b, sizeof(b), "%s  gain +%d dB  peak %d dBFS",
                 hp_inserted() ? "HP IN" : "SPK",
                 (int)lroundf(audio_rec_gain_db()), (int)lroundf(audio_rec_peak_db()));
        lv_label_set_text(g_loop_info, b);
    }
    snprintf(g_app.items[ITEM_LOOP].evidence, sizeof(g_app.items[ITEM_LOOP].evidence),
             "gain +%.0fdB, peak %.0fdBFS, %s",
             audio_rec_gain_db(), audio_rec_peak_db(), hp_inserted() ? "HP" : "SPK");
}

/* ---------------- page 3: mics ---------------- */

static const int g_mic_ch[6] = { 5, 4, 2, 3, 0, 1 };   /* TL TR ML MR BL BR */

static void build_mics(lv_obj_t *p)
{
    for(int i = 0; i < 6; i++) {
        int cx = (i % 2) ? 240 : 80;
        int cy = 42 + (i / 2) * 76;
        g_mic_ring[i] = mk_box(p, cx - 28, cy - 28, 56, 56, 0x000000, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(g_mic_ring[i], 1, 0);
        lv_obj_set_style_border_color(g_mic_ring[i], lv_color_hex(0x2a2a2a), 0);
        g_mic[i] = mk_box(p, cx - 13, cy - 13, 26, 26, 0x1f6f1f, LV_RADIUS_CIRCLE);
        g_mic_lbl[i] = mk_lbl(p, "", &lv_font_montserrat_14, 0x888888, cx - 28, cy + 29);
    }
    g_dir_lbl = mk_lbl(p, "", &lv_font_montserrat_14, 0x555555, 4, 276);
}

static uint32_t db_color(float db)
{
    if(db > -6) return 0xff3030;
    if(db > -18) return 0xffaa00;
    if(db > -40) return 0x40ff40;
    return 0x2a5a2a;
}

static void update_mics(void)
{
    float mx = -90;
    for(int i = 0; i < 6; i++) {
        int ch = g_mic_ch[i];
        float db = audio_level_db(ch);
        if(db > mx) mx = db;
        static int last_r[6] = { -1, -1, -1, -1, -1, -1 };
        static int last_db[6] = { 999, 999, 999, 999, 999, 999 };
        static uint32_t last_bg[6];
        static uint32_t last_ring[6];
        static int last_clip[6] = { -1, -1, -1, -1, -1, -1 };

        float norm = (db + 60.0f) / 60.0f;
        if(norm < 0) norm = 0;
        if(norm > 1) norm = 1;
        int r = 9 + (int)(norm * 11.0f);
        r &= ~1;                       /* 2 px steps: less redraw on noise */
        int cx = (i % 2) ? 240 : 80;
        int cy = 42 + (i / 2) * 76;
        if(r != last_r[i]) {
            last_r[i] = r;
            lv_obj_set_size(g_mic[i], r * 2, r * 2);
            lv_obj_set_pos(g_mic[i], cx - r, cy - r);
        }
        uint32_t bg = db_color(db);
        if(bg != last_bg[i]) {
            last_bg[i] = bg;
            lv_obj_set_style_bg_color(g_mic[i], lv_color_hex(bg), 0);
        }

        bool clip = audio_clip(ch);
        float pk = audio_peak_db(ch);
        uint32_t rc = clip ? 0xff3030 : (pk > -35.0f ? 0x00aa00 : 0x2a2a2a);
        int clipi = clip ? 2 : 1;
        if(rc != last_ring[i] || clipi != last_clip[i]) {
            last_ring[i] = rc;
            last_clip[i] = clipi;
            lv_obj_set_style_border_color(g_mic_ring[i], lv_color_hex(rc), 0);
            lv_obj_set_style_border_width(g_mic_ring[i], clipi, 0);
        }

        int dbi = ((int)lroundf(db) / 3) * 3;   /* 3 dB steps for the label */
        if(dbi != last_db[i]) {
            last_db[i] = dbi;
            char b[24];
            snprintf(b, sizeof(b), "ch%d %d", ch, dbi);
            lv_label_set_text(g_mic_lbl[i], b);
        }
    }
    {
        static int l6 = 999, l7 = 999;
        int d6 = (int)lroundf(audio_level_db(6)), d7 = (int)lroundf(audio_level_db(7));
        if(d6 != l6 || d7 != l7) {
            l6 = d6; l7 = d7;
            char b[40];
            snprintf(b, sizeof(b), "DIR L/R  %d / %d dB", d6, d7);
            lv_label_set_text(g_dir_lbl, b);
        }
    }
    snprintf(g_app.items[ITEM_MICS].evidence, sizeof(g_app.items[ITEM_MICS].evidence),
             "omni peak max %.0f dBFS", mx);
}

/* ---------------- page 4: vibrate ---------------- */

static void vib_short(lv_event_t *e)  { (void)e; motor_pattern(0); }
static void vib_double(lv_event_t *e) { (void)e; motor_pattern(1); }
static void vib_long(lv_event_t *e)   { (void)e; motor_pattern(2); }

static void build_vib(lv_obj_t *p)
{
    mk_btn(p, 4, 8, 100, 44, "SHORT", vib_short, NULL, 0x2a2a2a);
    mk_btn(p, 110, 8, 100, 44, "DOUBLE", vib_double, NULL, 0x2a2a2a);
    mk_btn(p, 216, 8, 100, 44, "LONG", vib_long, NULL, 0x2a2a2a);

    g_vib_ring = mk_box(p, 100, 66, 120, 120, 0x101010, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(g_vib_ring, 3, 0);
    lv_obj_set_style_border_color(g_vib_ring, lv_color_hex(0x333333), 0);
    g_vib_status = mk_lbl(p, "idle", &lv_font_montserrat_20, 0x888888, 134, 112);

    mk_lbl(p, "Confirm each pulse by hand.", &lv_font_montserrat_14, 0x666666, 6, 196);
}

static void update_vib(void)
{
    bool busy = motor_busy();
    lv_obj_set_style_border_color(g_vib_ring, lv_color_hex(busy ? 0x00ff00 : 0x333333), 0);
    lv_label_set_text(g_vib_status, busy ? "ON" : "idle");
    lv_obj_set_style_text_color(g_vib_status, lv_color_hex(busy ? 0x40ff40 : 0x888888), 0);
    snprintf(g_app.items[ITEM_VIB].evidence, sizeof(g_app.items[ITEM_VIB].evidence),
             "%d pulses fired", motor_patterns_done());
}

/* ---------------- page 5: summary ---------------- */

static void finish_cb(lv_event_t *e)
{
    (void)e;
    pages_finish();
}

static void build_summary(lv_obj_t *p)
{
    for(int i = 0; i < ITEM_COUNT; i++) {
        g_sum_row[i] = mk_lbl(p, g_short[i], &lv_font_montserrat_14, 0xffffff, 4, 6 + i * 32);
        g_sum_sub[i] = mk_lbl(p, "-", &lv_font_montserrat_14, 0x666666, 4, 22 + i * 32);
    }
    g_serial_lbl = mk_lbl(p, "", &lv_font_montserrat_14, 0x555555, 4, 168);
    mk_btn(p, 90, 236, 140, 26, "FINISH", finish_cb, NULL, 0x1f4f7a);
}

static void update_summary(void)
{
    for(int i = 0; i < ITEM_COUNT; i++) {
        verdict_t v = g_app.items[i].verdict;
        uint32_t c = v == VERDICT_PASS ? 0x40ff40 : (v == VERDICT_FAIL ? 0xff4040 : 0x888888);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s  %s", g_short[i],
                 v == VERDICT_PASS ? "PASS" : (v == VERDICT_FAIL ? "FAIL" : "----"));
        lv_label_set_text(g_sum_row[i], buf);
        lv_obj_set_style_text_color(g_sum_row[i], lv_color_hex(c), 0);
        lv_label_set_text(g_sum_sub[i], g_app.items[i].evidence);
    }
    static char serial[96];
    if(!serial[0]) {
        char cpu[32] = "?", sn[32] = "?";
        report_ids(cpu, sizeof(cpu), sn, sizeof(sn));
        snprintf(serial, sizeof(serial), "SN %s\nCPU %s", sn, cpu);
    }
    lv_label_set_text(g_serial_lbl, serial);
}

/* ---------------- public ---------------- */

void pages_build(void)
{
    build_chrome();
    for(int i = 0; i < 5; i++) g_pages[i] = mk_page();
    build_touch(g_pages[0]);
    build_keys(g_pages[1]);
    build_loop(g_pages[2]);
    build_mics(g_pages[3]);
    build_vib(g_pages[4]);
    lv_screen_load(g_scr);
    pages_show(0);
}

void pages_show(int idx)
{
    if(idx < 0) idx = 0;
    if(idx > 5) idx = 5;
    if(g_page != idx) {
        if(g_page >= 0) lv_obj_add_flag(g_pages[g_page], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_pages[idx], LV_OBJ_FLAG_HIDDEN);
        g_page = idx;
        g_app.page = idx;
    }

    /* capture only runs on the pages that need it */
    if(idx == ITEM_MICS) {
        if(!audio_meter_active()) audio_meter_start();
    }
    else {
        if(audio_meter_active()) audio_meter_stop();
    }
    if(idx != ITEM_LOOP) {
        if(audio_recording()) audio_rec_cancel();
        if(audio_playing()) audio_play_stop();
    }
}

void pages_next(int dir)
{
    pages_show((g_page + (dir > 0 ? 1 : -1) + 5) % 5);
}

int pages_current(void) { return g_page; }

static void debug_cmd(void)
{
    FILE *f = fopen("/mnt/sdcard/e1hwtest/.cmd", "r");
    if(!f) return;
    char line[64] = "";
    int have = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    unlink("/mnt/sdcard/e1hwtest/.cmd");
    if(have) {
        line[strcspn(line, "\r\n")] = 0;
        int a = 0, b = 0;
        if(!strcmp(line, "rec")) {
            audio_rec_start();
            g_autoplay_pending = true;
        }
        else if(!strcmp(line, "play")) {
            audio_play();
        }
        else if(!strcmp(line, "vib0")) motor_pattern(0);
        else if(!strcmp(line, "vib1")) motor_pattern(1);
        else if(!strcmp(line, "vib2")) motor_pattern(2);
        else if(!strcmp(line, "finish")) pages_finish();
        else if(!strcmp(line, "tear")) {
            g_tear = !g_tear;
            if(g_tear) lv_obj_remove_flag(g_tearbar, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(g_tearbar, LV_OBJ_FLAG_HIDDEN);
        }
        else if(!strcmp(line, "shot")) {
            FILE *o = fopen("/mnt/sdcard/e1hwtest/shot.raw", "wb");
            if(o) {
                if(g_display_frame) fwrite(g_display_frame, 1, 320u * 320u * 4u, o);
                fclose(o);
            }
        }
        else if(sscanf(line, "led %d %d", &a, &b) == 2) { led_color(a); led_pwm(b); led_top(true); }
        else if(sscanf(line, "kbled %d", &a) == 1) led_key(a != 0);
        else if(sscanf(line, "pass %d", &a) == 1 && a >= 0 && a < ITEM_COUNT) g_app.items[a].verdict = VERDICT_PASS;
        else if(sscanf(line, "fail %d", &a) == 1 && a >= 0 && a < ITEM_COUNT) g_app.items[a].verdict = VERDICT_FAIL;
        fprintf(stderr, "debug cmd: %s\n", line);
    }
}

static void update_evidence(void)
{
    const e1_input_stats_t *st = e1_input_stats();
    snprintf(g_app.items[ITEM_TOUCH].evidence, sizeof(g_app.items[ITEM_TOUCH].evidence),
             "cells %d/64, max pts %d", st->cells, st->max_contacts);

    int seen = 0;
    for(int i = 0; i < E1K_COUNT; i++) if(keys_count((e1_key_t)i) > 0) seen++;
    snprintf(g_app.items[ITEM_KEYS].evidence, sizeof(g_app.items[ITEM_KEYS].evidence),
             "%d/6 keys pressed, pwr hold %ums", seen, (unsigned)keys_last_hold(E1K_PWR));

    snprintf(g_app.items[ITEM_LOOP].evidence, sizeof(g_app.items[ITEM_LOOP].evidence),
             "gain +%d dB, peak %d dBFS, %s",
             (int)lroundf(audio_rec_gain_db()), (int)lroundf(audio_rec_peak_db()),
             hp_inserted() ? "HP" : "SPK");

    float mx = -90;
    for(int i = 0; i < 6; i++) {
        float db = audio_level_db(g_mic_ch[i]);
        if(db > mx) mx = db;
    }
    snprintf(g_app.items[ITEM_MICS].evidence, sizeof(g_app.items[ITEM_MICS].evidence),
             "omni peak max %d dBFS", (int)lroundf(mx));

    snprintf(g_app.items[ITEM_VIB].evidence, sizeof(g_app.items[ITEM_VIB].evidence),
             "%d pulses fired", motor_patterns_done());
}

void pages_update(void)
{
    static uint32_t last_chk;
    uint32_t now = e1_now_ms();
    if(now - last_chk > 400) {
        last_chk = now;
        FILE *f = fopen("/mnt/sdcard/e1hwtest/.page", "r");
        if(f) {
            int p = -1;
            if(fscanf(f, "%d", &p) == 1 && p >= 0 && p <= 4) pages_show(p);
            fclose(f);
            unlink("/mnt/sdcard/e1hwtest/.page");
        }
        debug_cmd();
    }

    if(g_tear) {
        g_tear_x = (g_tear_x + 10) % 320;
        lv_obj_set_x(g_tearbar, g_tear_x);
    }

    update_evidence();
    update_chrome();
    switch(g_page) {
        case ITEM_TOUCH: update_touch(); break;
        case ITEM_KEYS:  update_keys();  break;
        case ITEM_LOOP:  update_loop();  break;
        case ITEM_MICS:  update_mics();  break;
        case ITEM_VIB:   update_vib();   break;
        default: break;
    }

}

void pages_finish(void)
{
    report_write();
    audio_meter_stop();
    audio_play_stop();
    motor_init();
    led_top(false);
    led_key(false);

    FILE *f = fopen("/userdata/log/e1hwtest.log", "a");
    if(f) { fprintf(f, "finish: returning to host UI\n"); fclose(f); }

    e1_drm_restore();   /* 把 CRTC 还原到启动前的 fb（即 freE1 的 fb0） */
    fprintf(stderr, "finish: drm restored, exiting\n");
    exit(0);
}
