#pragma once
#include <stdint.h>

typedef struct {
    char dir[400];
    char state[16];      /* idle | recording | closed */
    unsigned seq;
    char path[512];
    uint64_t frames;
    uint32_t rate;
    int ch;
    uint32_t rtc_start;
    int64_t  ms_start;
    int recovered;       /* 启动时修过几个文件 */
} session_t;

int session_load(const char *dir, session_t *s);
int session_save(const session_t *s);
