#pragma once
int ui_run_x11(const char *dir, int device, int http_port);
int ui_run_device(const char *dir, int http_port);
int ui_run_shots(const char *dir, const char *outdir);
int ui_test_swipe(void);
int ui_get_page(void);
