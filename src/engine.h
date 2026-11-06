#pragma once
#include "rec.h"
int  engine_start(const char *dir, int use_device, int omni, int seg_sec, int nch, int rate, int bits);
void engine_stop(void);
void engine_pause(int on);   /* 暂停/继续：文件保持打开，暂停区间不进文件 */
int  engine_running(void);
rec_status *engine_status(void);
