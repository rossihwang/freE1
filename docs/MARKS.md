# 录音标记（MARK）与上位机导入

## 设备端

录音中按 **MARK 键**（最左边的 KEY_1）打一个标记：马达短震一次，屏幕弹出
`MARK 3  00:01:23`，1.5 秒后自动消失。播放/待机时按 MARK 不做事。

标记存在录音旁边的同名 sidecar 文件里：

```
$ cat REC_0112_20261107-0026.wav.marks
17250
48300
91000
```

每行一个**毫秒偏移**（相对该录音开头）。选 sidecar 而不是写进 WAV 的 `cue` 块，
是因为 cue 要重写文件头再 fsync，断电时可能把录音本身搞坏；sidecar 写坏最多丢
最后一行（读取时坏行直接跳过）。写标记会立刻 `fsync`。

文件列表里，有标记的录音右上角显示 **MARK** 角标；删除录音会一并删掉 `.marks`。

## 导出到上位机 DAW

```bash
./tools/marks-export.py REC_0112_20261107-0026.wav            # 一次出三种
./tools/marks-export.py --format reaper REC_xxx.wav           # 只要一种
./tools/marks-export.py --cue out.wav REC_xxx.wav             # 另存带 cue 标记的 WAV
./tools/marks-export.py --dir /mnt/sdcard/freE1               # 整个目录批量
```

| 输出 | 文件 | 喂给谁 | 导入方式 |
|---|---|---|---|
| `audacity` | `*.labels.txt` | **Audacity**、Ocenaudio | 文件 → 导入 → 标签 |
| `reaper` | `*.rpp` | **Reaper** | 直接双击打开，音频轨道 + 标记都在 |
| `csv` | `*.marks.csv` | 脚本 / Excel / 自研工具 | — |
| `cue` | 你指定的 `*.wav` | **Pro Tools / SoundForge / Wavelab** | 打开 WAV，标记自动出现 |

其它 DAW 的对应关系（用现有输出就能到）：

- **Adobe Audition**：用 csv 或 audacity 标签文本，`标记面板 → 导入`
- **DaVinci Resolve / Premiere**：认 EDL，可以从 csv 再转 CMX3600；需要的话再加一个 `--format edl`
- **Logic Pro**：可从 MIDI 文件导入标记（同理可加 `--format midi`）
- **Audacity 之外想批量处理**：csv 是最稳的接口

`cue` 输出只写 WAV 副本，不动原文件；`cue` + `LIST/adtl/labl` 是通用做法，
Pro Tools 一类按样本偏移读，不依赖录音是 48k 还是 44.1k。
