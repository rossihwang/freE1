#pragma once
struct lv_display_t;
struct lv_display_t *hal_disp_fb_init(void);
int hal_input_init(void);
void hal_set_key_cb(void (*cb)(int,int));
void hal_set_swipe_cb(void (*cb)(int,int));
void hal_set_touch_enabled(int on);
void hal_disp_set_flush_enabled(int on);
void hal_disp_fb_reclaim(void);
void hal_set_touch_act_cb(void (*cb)(void));
int  hal_input_last_drag(void);
void hal_input_last_xy(int *x, int *y);
