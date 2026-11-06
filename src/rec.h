#pragma once
#include <stdint.h>

typedef struct {
    volatile int running;
    volatile int recording;
    volatile int failed;
    volatile int paused;        /* 1 = 暂停：继续读管道但不落盘 */
    volatile int levels[8];     /* 0..100 */
    volatile int db_dbfs;       /* -72..0 */
    volatile unsigned long long frames;
} rec_status;

typedef struct {
    const char *dir;
    int segment_sec;      /* 默认 3600 */
    int use_device;       /* 0 = 模拟源 */
    const char *dev;      /* hw:2,0 */
    int nch_in;           /* 8 */
    int rate;             /* 48000 / 44100 */
    int bits;             /* 24 / 16 */
    int omni;             /* 0=Top(ch6,7) 1=Omni(左右列混音) */
    volatile int *stop;
    void (*on_tick)(uint64_t frames_total, int64_t ms_elapsed, const char *segpath, void *ud);
    void *ud;
    rec_status *status;
} rec_cfg;

int rec_run(rec_cfg *cfg);
int rec_recover(const char *dir);   /* 返回修复的文件数 */
