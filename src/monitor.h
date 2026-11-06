#pragma once
#include <stdint.h>

/* 录音监听（Monitor）：把采集到的声音实时送到耳机/扬声器。
   与录音共用同一份采样数据（传进来的是已经 AGC 过的 interleaved 立体声帧）。
   只在插耳机时允许开启，避免扬声器形成声反馈啸叫。 */

int  monitor_open(uint32_t rate);        /* 打开 ALSA 播放端；0 成功 */
void monitor_close(void);
int  monitor_is_open(void);
/* 写入 interleaved 立体声 S32_LE 帧；非阻塞语义（内部短时间等待） */
int  monitor_write(const int32_t *frames, uint32_t n);
int  monitor_underruns(void);
int  monitor_drops(void);