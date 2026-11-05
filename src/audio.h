#pragma once
#include <stdint.h>

typedef struct {
    void *ctx;
    int  (*read)(void *ctx, int32_t *buf, int frames);   /* 返回实际帧数，<0 出错 */
    void (*close)(void *ctx);
} audio_src;

/* 模拟源：8 路，ch6/ch7 = 1 kHz 测试音，其余为噪声；用于 PC 验证 */
audio_src *audio_open_sim(int nch, int rate);
/* 设备源：arecord 子进程，raw S32_LE 8ch */
audio_src *audio_open_arecord(int nch, int rate, const char *dev);
