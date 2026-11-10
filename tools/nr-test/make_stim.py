#!/usr/bin/env python3
"""生成降噪/录音质量 A/B 测试信号（30 秒循环，48 kHz / 16-bit / 立体声）。

结构（一个循环）：
    0–5 s    数字静音          → 测底噪
    5–15 s   语音状噪声         → 测电平、SNR（4 Hz 幅度调制 + 近似粉噪）
    15–20 s  数字静音          → 测底噪（第二段）
    20–30 s  100 Hz → 8 kHz 扫频 → 测频响/带宽

用法：
    python3 make_stim.py out.wav [--rate 48000] [--seconds 30]

为什么这样设计：
  * 数字静音：被测设备放不出任何声音，录到的就是它的本底噪声（电噪声+环境），
    这是唯一能把"底噪"和"信号"分开的段落。
  * 语音状噪声：粉噪（-3 dB/oct）接近人声长时频谱，再用 4 Hz 调幅模拟音节节奏，
    让 AGC/压缩器像对待语音一样工作；比单音更能暴露压缩器的动作。
  * 扫频：频率随时间线性增长（对数），一遍就能看出整条链的频响、带宽上限和凹陷。
  * 30 秒一轮 + 循环播放：录音方不需要和播放对齐，事后按段落切分即可。
"""
import argparse, math, random, struct, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    sr, total = a.rate, a.rate * a.seconds
    random.seed(a.seed)
    buf = bytearray()
    pink = 0.0
    for i in range(total):
        t = i / sr
        if t < 5 or 15 <= t < 20:
            v = 0.0
        elif t < 15:
            pink = 0.98 * pink + 0.02 * (random.random() * 2 - 1)
            am = 0.5 + 0.5 * math.sin(2 * math.pi * 4 * t)
            v = 0.35 * pink * am
        else:
            f = 100.0 * (80.0 ** ((t - 20.0) / 10.0))      # 100 Hz -> 8 kHz
            v = 0.30 * math.sin(2 * math.pi * f * (t - 20.0))
        s = max(-32768, min(32767, int(v * 32767)))
        buf += struct.pack("<hh", s, s)
    data = bytes(buf)
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
           struct.pack("<IHHIIHH", 16, 1, 2, sr, sr * 4, 4, 16) +
           b"data" + struct.pack("<I", len(data)))
    with open(a.out, "wb") as f:
        f.write(hdr + data)
    print("%s: %.1f s, %d Hz, stereo, %d bytes" % (a.out, a.seconds, sr, len(hdr) + len(data)))

if __name__ == "__main__":
    main()
