#pragma once
#include "lvgl.h"
lv_obj_t *wifi_page_create(lv_obj_t *parent);
void wifi_page_on_show(void);
void wifi_page_tick(void);
void wifi_ui_trigger_scan(void);
void wifi_ui_scroll(int dy);
void wifi_ui_demo_open(const char *ssid);
void wifi_ui_demo_close(void);
