#pragma once
#include <stdint.h>

/* 进程内 ALSA 播放器（替代 aplay 子进程）。
   支持：播放 / 暂停继续 / 拖动跳转 / 停止 / 进度查询。
   解码：WAV PCM，S24_3LE(3 字节) 与 S16_LE(2 字节) 两种，均转成 S32_LE 交给 ALSA。 */

typedef struct {
    int     active;        /* 1 = 有正在播放的文件 */
    int     paused;
    int64_t pos_ms;
    int64_t total_ms;
    char    file[192];
} play_state_t;

/* 开始播放（会先停止当前播放）。返回 0 成功。 */
int  play_open(const char *dir, const char *name);
void play_stop(void);
void play_toggle_pause(void);
void play_seek_ms(int64_t ms);
void play_get_state(play_state_t *st);
int  play_is_active(void);
