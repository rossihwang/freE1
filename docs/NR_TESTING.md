# 录音 / 降噪质量怎么测

一套可复现的客观测法：同一段测试音、同一台设备、同一音量，分别用两个 App 录音，
再用固定指标比较。目的是回答三个问题：

1. 录到的**格式**有没有损失（采样率 / 位深 / 声道）
2. **底噪**有多低、和信号差多少（SNR），降噪到底压了哪一段（逐带 SNR）
3. **音色**是否被人为处理过（语音频段提升、低频切除、带宽上限）

配套脚本在 [`tools/nr-test/`](../tools/nr-test/)：`make_stim.py`（生成测试音）、`analyze.py`（出指标）。

---

## 1. 需要的音频：测试信号

由 `make_stim.py` 生成，30 秒一循环，48 kHz / 16-bit / 立体声：

| 时间 | 内容 | 用途 |
|---|---|---|
| 0–5 s | 数字静音 | 测**底噪**（设备放不出声，录到的就是本机噪声） |
| 5–15 s | 语音状噪声（近似粉噪 + 4 Hz 调幅） | 测**电平/SNR**；调幅让 AGC 像对待语音一样动作 |
| 15–20 s | 数字静音 | 第二段底噪（和第一段互相校验） |
| 20–30 s | 100 Hz → 8 kHz 对数扫频 | 测**频响/带宽/凹陷** |

设计理由：

- **必须要有真静音段**：只有播放器输出 0，才能把"底噪"和"信号"分开，否则只能测总电平
- **语音状噪声而不是单音**：单音会被 AGC 当成稳态信号压平，看不出压缩器对语音的行为；
  粉噪的频谱斜率接近人声长时谱，调幅模拟音节，能暴露压缩器的动作和抽吸
- **扫频而不是白噪**：一遍就能看出整条链的频响形状、带宽上限和梳状凹陷位置
- **循环播放**：录音方不需要和播放同步，事后按段落切分即可（这也是能"随时按录音键"的原因）

生成与校验：

```bash
python3 tools/nr-test/make_stim.py /tmp/stim.wav
# 30.0 s, 48000 Hz, stereo, 5760044 bytes
md5sum /tmp/stim.wav
# 1d1114bb68d99ed2b53286da3df5a6a3   （固定 seed，结果可复现）
```

播放：

```bash
# 推到设备，循环播放（每轮 30 s，80 轮约 40 分钟）
adb push /tmp/stim.wav /tmp/stim.wav
adb shell 'setsid sh -c "i=0; while [ \$i -lt 80 ]; do aplay -q -D default /tmp/stim.wav >/dev/null 2>&1; i=\$((i+1)); done" >/dev/null 2>&1 &'
adb shell 'amixer -c 0 sset Master 70%'     # 固定音量，两次录音必须一致
```

> 用设备自己的扬声器当声源，好处是完全可控、可重复；坏处是声源在设备**下方/后方**，
> 对"朝前的固定波束"不公平（见第 6 节坑 1）。

---

## 2. 录音流程

### 2.1 被测设备：freE1

```bash
adb shell 'printf "rec" > /mnt/sdcard/freE1/.cmd'      # 开始
# 等 45 s 以上（≥ 1.5 个循环）
adb shell 'printf "stopkey" > /mnt/sdcard/freE1/.cmd'  # 停止
adb pull /mnt/sdcard/freE1/REC_xxxx_yyyy.wav .
```

### 2.2 被测设备：原厂 App

- 需要人按一次录音键（或点屏幕），录 40 s 后停止；文件落在
  `/mnt/sdcard/tempUser/<YYYY.MM>/<时间>_<类型>.wav` 与 `..._denoise.mp3`
- 降噪档位存在 `/userdata/databases/E1-Cf.db` 的 `recordparam.NoiseReduceMethod`
  （1 = 无额外降噪，2 = AFE 固定波束，3 = NNSE）

> ⚠️ **不要把档位设成 3**：这台设备的固件没有 `/etc/sogou/front-config/nnse/`，
> App 会因为 NNSE 资产缺失 **段错误（SIGSEGV, exit 139）**，然后被 init 每 1.6 s
> 拉起一次，几分钟后整机被看门狗重启。详见
> [AUDIO_COMPARISON.md](AUDIO_COMPARISON.md) 第 8 节。

### 2.3 两次录音之间必须保持一致

| 条件 | 要求 |
|---|---|
| 声源 | 同一段循环测试音、同一音量（`Master 70%`） |
| 时长 | ≥ 1.5 个循环（45 s+），保证包含完整的静音/噪声/扫频段 |
| 时间 | 尽量相邻时段；房间噪声会漂 |
| 位置 | 设备别挪动；麦克风阵列的朝向会改变波束响应 |

---

## 3. 分析

```bash
python3 tools/nr-test/analyze.py 原厂.wav freE1.wav
python3 tools/nr-test/analyze.py --ref 基线.wav 新版本.wav     # 估算增益归一后的降噪量
```

依赖 `ffmpeg` / `ffprobe` / `numpy`。所有输入先解码成 **48 kHz 单声道**，消除采样率和声道差异。

### 指标定义

| 指标 | 定义 | 说明 |
|---|---|---|
| RMS / 峰值 | 整段 | 绝对电平；受 AGC 影响，跨文件比要小心 |
| `floor` 底噪 | 50 ms 帧电平的 **p10** | 静音段的代表值 |
| `loud` 有声 | 50 ms 帧电平的 **p95** | 噪声段/扫频段的代表值 |
| `SNR` | `loud − floor` | **同一文件内**计算，所以和 AGC 增益无关 |
| 逐带 SNR | 把帧分成安静 15% / 有声 15%，逐频带算能量差 | **判断降噪压了哪一段的核心指标** |
| LTAS | 只统计有声帧的长时平均谱，以 500–1000 Hz 为 0 dB | 看音色、语音段提升、带宽墙 |
| 立体声相关 | 逐带 L/R 幅度谱相关 | 判断阵列相干性 |
| 下混损失 | `(L+R)/2` 相对单通道的频响 | 负值 = 相消（梳状） |
| 增益归一底噪 | `floor_ref + Δgain` 对比实测 floor | 估算"超出线性放大的那部分降噪" |

### 为什么这样设计

- **跨文件不要直接比绝对电平**：两个 App 的 AGC 目标不同（实测差 12.7 dB）。
  有意义的是**同一个文件里**信号比底噪高多少（SNR），以及**同一信号下**不同设备压掉了多少噪声
- **逐带 SNR 比总 SNR 更能说明问题**：总 SNR 会被低频隆隆声和带宽差异带偏；
  分频带看才知道降噪是"压了宽带噪声"还是"只砍了低频"
- **LTAS 用相对值**：以 500–1000 Hz 归一，只看形状（语音段有没有被抬、高频有没有被切）

---

## 4. 基线数据（2026-11-10，同一段测试音）

| | 原厂 一级 | 原厂 二级 | freE1 |
|---|---|---|---|
| 格式 | 16 kHz/16-bit/单声道 | 同左 | **48 kHz/24-bit/立体声** |
| RMS | −19.7 dBFS | −19.6 dBFS | −31.7 dBFS |
| 峰值 | −1.1 dBFS | **−0.2 dBFS** | −15.5 dBFS |
| 底噪 | −55.7 dBFS | −48.3 dBFS | −61.7 dBFS |
| 有声 | −9.7 dBFS | −10.9 dBFS | −22.4 dBFS |
| SNR | 46.0 dB | 37.4 dB | 39.3 dB |
| 1–2 kHz SNR | 51.1 dB | 45.4 dB | 39.6 dB |
| 4–7 kHz SNR | 51.4 dB | 44.4 dB | 48.1 dB |
| 7.6 kHz | **−42.9 dB**（16k 上限） | −45.6 dB | −2.8 dB |

- 原厂的语音带 SNR 明显更高（1–2 kHz 高 11.5 dB），代价是只剩 16 kHz/单声道
- 原厂二级在这套几何下反而更差（波束朝前、声源在下方），这个对比对二级不公平
- 原厂三级在这台设备上跑不起来（资产缺失，见上）

完整解读见 [AUDIO_COMPARISON.md](AUDIO_COMPARISON.md)。

---

## 5. 复现步骤（一条龙）

```bash
# 1) 生成测试音并推到设备
python3 tools/nr-test/make_stim.py /tmp/stim.wav
adb push /tmp/stim.wav /tmp/stim.wav

# 2) 循环播放 + 固定音量
adb shell 'setsid sh -c "i=0; while [ \$i -lt 80 ]; do aplay -q -D default /tmp/stim.wav >/dev/null 2>&1; i=\$((i+1)); done" >/dev/null 2>&1 &'
adb shell 'amixer -c 0 sset Master 70%'

# 3) freE1 录音 45 s
adb shell 'printf "rec" > /mnt/sdcard/freE1/.cmd'; sleep 45
adb shell 'printf "stopkey" > /mnt/sdcard/freE1/.cmd'; sleep 2
adb shell 'ls -t /mnt/sdcard/freE1/REC_*.wav | head -1'
adb pull <上一步的文件> /tmp/free1.wav

# 4) 原厂录音：换槽位后人工按一次录音键（40 s），再取文件
# 5) 分析
python3 tools/nr-test/analyze.py /tmp/factory.wav /tmp/free1.wav --ref /tmp/factory.wav
```

---

## 6. 已知的坑

1. **波束方向性**：测试音从设备自身扬声器（朝下/朝后）发出，朝前的固定波束会把**信号**
   一起压掉，看起来"降噪更差"。公平评估二级/三级要用**正前方 0.5–1 m 的独立声源**
   （手机/音箱播放同一段测试音）。
2. **采样率天花板**：原厂整条链 16 kHz，7.5 kHz 以上必然没有；比较带宽前先看格式，
   否则会把"格式限制"误读成"降噪切高频"。
3. **AGC 增益不同**：跨文件比绝对电平没有意义；用文件内 SNR 或增益归一后的底噪。
4. **播放间隙**：`aplay` 每轮结束到下一轮开始有几十毫秒空隙，会让"静音段"多出边界；
   用分位数（p10/p95）而不是最小值/最大值可缓解。
5. **音量会被重置**：freE1 的启动脚本会把 `Master` 设成 98%，每次重启后要重新设 70%。
6. **原厂档位 3 会崩溃循环**（资产缺失），别开；开了要准备用 USB adb 恢复。
7. **原厂会空闲自动关机**（`AutoPowerOffTime=12` 分钟），长测试请插 USB。
8. **静音底噪会漂**：同一房间不同时段差 2–4 dB 很常见；两次录音尽量相邻，
   并同时看逐带 SNR（对整体底噪漂移不敏感）。

---

## 7. 工具清单

| 文件 | 作用 |
|---|---|
| [`tools/nr-test/make_stim.py`](../tools/nr-test/make_stim.py) | 生成 30 s 循环测试音（固定 seed，md5 可校验） |
| [`tools/nr-test/analyze.py`](../tools/nr-test/analyze.py) | 输出格式、电平、底噪、SNR、逐带 SNR、LTAS、立体声相关、下混损失 |
| [AUDIO_COMPARISON.md](AUDIO_COMPARISON.md) | 用这套方法得到的原厂 vs freE1 实测结果与解读 |
