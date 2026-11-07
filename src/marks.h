#pragma once
#include <stdint.h>

/* 录音标记（书签）。
   每个录音旁边一个 sidecar 文件 `<录音名>.marks`，一行一个毫秒偏移：

       17250
       48300

   为什么不写进 WAV 的 cue 块：cue 要重写文件头并 fsync，断电时可能把录音本身
   搞坏；sidecar 写坏最多丢标记。读取时跳过坏行，所以断电最多丢最后一行。 */

/* 追加一个标记。返回 0 成功。写后 fsync（标记很少，开销可忽略）。 */
int marks_append(const char *dir, const char *name, uint64_t ms);

/* 该录音是否至少有一个标记（列表里显示"有没有"）。 */
int marks_has(const char *dir, const char *name);

/* 标记个数（保留给以后的标记列表页/HTTP 接口用）。 */
int marks_count(const char *dir, const char *name);

/* 删除录音时一并删掉 sidecar。 */
void marks_remove(const char *dir, const char *name);
