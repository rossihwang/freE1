#pragma once
#include <stdio.h>
#include <stdint.h>

typedef struct {
    FILE *f;
    char path[512];
    uint32_t rate;
    uint16_t ch;
    uint16_t bits;          /* 16 或 24 */
    uint64_t data_bytes;
} wav_t;

/* 打开 24-bit PCM WAV（S24_3LE），写占位头 */
int  wav_open(wav_t *w, const char *path, uint32_t rate, uint16_t ch, uint16_t bits);
/* 从 nch_src 路 S32 交错帧里按 map 选 2 路，转 S24_3LE 追加 */
int  wav_append(wav_t *w, const int32_t *src, uint32_t frames, int nch_src, const int map[2]);
int  wav_close(wav_t *w, int do_sync);
/* 断电后修复：把 RIFF/data 长度改成实际文件大小；返回帧数 */
int  wav_repair(const char *path, uint64_t *frames_out);
