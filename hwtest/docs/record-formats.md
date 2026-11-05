# E1 录音格式调查（ZhiyinOS 1.2.3 实机）

2026-11-04，基于原厂二进制（`SogouE1` 1.2.3）、设备文件、ALSA 实测与测试音矩阵。

## 一、用户可选的录音格式（音乐模式）

来源：`SogouE1` 内嵌 QML `MusicFormatPage` 的 `ListModel`（原文逐条）：

| index | 格式 |
|---|---|
| 0 | LPCM 96KHz/24bit |
| 1 | LPCM 48KHz/24bit |
| 2 | LPCM 48KHz/16bit |
| 3 | LPCM 44.1KHz/24bit |
| 4 | LPCM 44.1KHz/16bit |
| 5 | Mp3 320Kbps |
| 6 | Mp3 192Kbps（QML 默认值） |
| 7 | Mp3 128Kbps |
| 8 | Mp3 48Kbps |

- 该页面只在**音乐模式**下可进（提示文案："音乐模式下 可选择以下录音格式"）。
- 设备实际存储值：`/userdata/databases/E1-Cf.db` → `recordparam.CurRecordFormat = 0`
  → 当前选中的是 **LPCM 96KHz/24bit**。
- 编解码：App 动态链接 `libmp3lame.so.0`，并调用
  `lame_set_in_samplerate / lame_set_out_samplerate / lame_set_brate / lame_set_mode / lame_set_quality`
  （`readelf -sW` 实测符号），与上表的 MP3 档位对应。

## 二、普通录音（会议 / 备忘等）的实际产物

实测文件（`ffprobe` / WAV 头）：

| 产物 | 参数 | 用途 |
|---|---|---|
| `*_meeting.wav` | PCM **16 kHz / 16-bit / 单声道** | 原始录音，供云端转写 |
| `*_meeting_denoise.mp3` | **16 kHz / 单声道 / 160 kbps** | 降噪版，供导出回听 |
| `*_memo_denoise.mp3` | 16 kHz / 单声道 / 160 kbps | 同上 |

即普通录音的格式是**固定的**（WAV + MP3 双产物），不受"音乐格式"菜单影响；
菜单里没有 16 kHz / 160 kbps 这一档。

## 三、底层采集能力（ALSA 实测）

`arecord --dump-hw-params`：

| 设备 | 格式 | 声道 | 采样率 |
|---|---|---|---|
| `hw:0,0`（rk3308-acodec） | S16_LE / S24_LE / S32_LE | 2–8 | 8000–192000 |
| `hw:2,0`（PDM 8 麦阵列） | S16_LE / S24_LE / S32_LE | 2–8 | 8000–192000 |

注意：`S24_LE` 的**容器是 4 字节**（WAV 头 `block_align/ch = 4`），不是 3 字节。

### PDM 阵列真实速率验证（1 kHz / 3 kHz 测试音 + Goertzel）

`hw:2,0`、8 声道、默认 period/buffer：

| 请求速率 | 结果 |
|---|---|
| 8000 / 11025 / 16000 / 22050 / 32000 / 44100 / 48000 / 64000 / 88200 / 96000 / 176400 / 192000 | ✅ 录到测试音，主导频率与播放频率一致 |
| 12000 | ⚠ 被吸附为 **11025**（WAV 头写实际值） |
| 24000 | ⚠ 被吸附为 **22050** |

2 声道（`-c 2`）在 32 kHz 下同样正常。

> 与旧结论的冲突：旧文档称"PDM 必须 32 kHz + S32_LE，否则只有底噪"。
> 本次在 `hw:2,0` 直连、默认 period/buffer 下，16 kHz / S16_LE 也能录到真实音频。
> 旧结论很可能来自 `multi_10` 插件 + 特定 period/buffer 的路径，建议需要时复测。

### `hw:0,0` 不是麦克风阵列

8 声道抓取时只有 **ch0** 有信号，其余通道静音；该通道能录到播放的测试音，
判断是**回采 / AEC 参考**，不是麦克风。原厂 QA 脚本 `/oem/AudioTest/arecord.sh`
用 `hw:0,0` 跑 16/32/48 kHz × 2/4/6/8 声道 × S16/S32，应该是测 codec/AEC 通路。

## 四、24-bit 支持验证

结论：**采集链路确实按 24-bit 工作**，不是"16-bit 补零"。

方法：同参数录 2 s 环境声，分别用 S16_LE / S24_LE / S32_LE，检查样本低位。

| 格式 | 容器/样本 | 低位统计 |
|---|---|---|
| S16_LE | 2 B | 基准 |
| S24_LE | 4 B（ALSA 的 24-bit 用 32-bit 容器） | **bit8–15 非零率 99.5%，256 种取值**；bit0–7 恒为 0（容器填充） |
| S32_LE | 4 B | 同上：低 8 位恒 0，高 24 位有数据 |

即数据是 **24-bit 左对齐在 32-bit 字里**（`v = sample24 << 8`），
所以 S24_LE 与 S32_LE 的"真实有效位"都是 24 bit；`low 8 bits == 0` 是容器填充，
不代表只有 16 bit。

两点边界：

- 这是**采集链路**的能力证明；原厂 App 在音乐模式落盘的 WAV 是否 24-bit，
  尚未端到端实测（需要在原厂 UI 里以音乐模式录一段）。
- 24-bit 是格式上限，不保证 24-bit 有效动态范围：PDM 麦自噪声在 -60 dBFS 量级，
  实际有效位数（ENOB）由麦克风决定，远小于 24。

## 五、本测试程序（e1hwtest）用的格式

- 采集：`arecord -D hw:2,0 -c 8 -r 32000 -f S32_LE`，取 **ch6/ch7**（顶端指向麦）
- 回环：`aplay -D hw:0,0 -c 2 -r 32000 -f S32_LE`，**只驻留 RAM，不落文件**
- 丢弃开流前 400 ms（PDM 上电瞬态）

## 六、未验证 / 待确认

1. 音乐模式落盘 WAV 是否为 **24-bit**（采集链路已证实支持，App 写盘未验证）。
2. 音乐模式 LPCM 的**声道数**：QML 未标注；按两颗指向麦推断为 2 声道，需实机录一段确认。
3. 音乐模式 **MP3 的输出采样率**：LAME `set_out_samplerate` 的实际取值未在运行中抓到。
4. 24-bit WAV 的写盘宽度（3 字节打包 or 4 字节容器）：取决于 App 的 WAV writer，未实测。

## 复现方法

```bash
# ALSA 能力
arecord -D hw:2,0 --dump-hw-params -f S32_LE -c 8 -r 32000 -d 1 /dev/null

# 速率/位深矩阵（需配合 1 kHz 测试音）
see hwtest/tools/pcmstat.c
```

证据文件：
- `SogouE1` QML：`MusicFormatPage`（"音乐格式"）
- `/oem/AudioTest/arecord.sh`、`readme.txt`
- `/userdata/databases/E1-Cf.db`（`recordparam`）
- `hwtest/tools/pcmstat.c`
