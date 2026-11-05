#pragma once
#include "rec.h"
int  engine_start(const char *dir, int use_device, int omni, int seg_sec, int nch, int rate, int bits);
void engine_stop(void);
int  engine_running(void);
rec_status *engine_status(void);
