#!/usr/bin/env python3
"""录音/降噪质量的客观指标（同口径对比多个录音）。

用法：
    python3 analyze.py a.wav b.mp3 ...
    python3 analyze.py --ref baseline.wav new.wav        # 额外估算增益归一后的降噪量

依赖：ffmpeg / ffprobe、numpy。

指标定义（全部在 48 kHz 单声道下计算，避免采样率/声道差异干扰）：
  RMS / 峰值     整段电平
  floor          50 ms 帧电平的第 10 百分位 = 底噪（静音段）
  loud           第 95 百分位 = 有声段
  SNR            loud - floor（两者都来自同一文件，所以和 AGC 增益无关）
  per-band SNR   把帧分成"安静 15%"和"有声 15%"，逐频带算能量差；
                 这是判断降噪到底压了哪一段的关键
  LTAS           长时平均谱（只统计有声帧），以 500–1000 Hz 为 0 dB 归一，
                 用来比较两台设备的音色/语音段提升/带宽
  立体声相关      逐带 L/R 幅度谱相关；mono 下混损失表示 (L+R)/2 相对单通道的响应
"""
import argparse, subprocess, sys
import numpy as np

SR = 48000
BANDS = [(80, 200), (200, 500), (500, 1000), (1000, 2000), (2000, 4000), (4000, 7000)]

def ffprobe_info(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0",
                          "-show_entries", "stream=sample_rate,channels,bits_per_sample,codec_name",
                          "-of", "default=nw=1", path], capture_output=True, text=True).stdout
    return dict(l.split("=", 1) for l in out.strip().split("\n") if "=" in l)

def decode(path, ch=1):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", str(ch), "-ar", str(SR),
                          "-f", "f32le", "-"], capture_output=True).stdout
    a = np.frombuffer(raw, dtype="<f4").astype(np.float64)
    return a if ch == 1 else a.reshape(-1, ch)

def frame_levels(x, ms=50):
    w = int(ms / 1000 * SR); n = len(x) // w
    return np.sqrt((x[:n * w].reshape(n, w) ** 2).mean(axis=1) + 1e-20), w

def ltas(x, w=4096):
    hop = w // 2
    acc = np.zeros(w // 2 + 1); cnt = 0
    for i in range((len(x) - w) // hop):
        seg = x[i * hop:i * hop + w]
        if np.sqrt((seg ** 2).mean()) < 3e-3:      # 只统计有声帧
            continue
        acc += np.abs(np.fft.rfft(seg * np.hanning(w))) ** 2; cnt += 1
    return np.fft.rfftfreq(w, 1 / SR), acc / max(cnt, 1)

def band_level(segs, w, lo, hi):
    X = np.abs(np.fft.rfft(segs * np.hanning(w), axis=1)) ** 2
    f = np.fft.rfftfreq(w, 1 / SR)
    m = (f >= lo) & (f < hi)
    return 10 * np.log10(X[:, m].mean() + 1e-20)

def report(path, ref=None):
    info = ffprobe_info(path)
    x = decode(path)
    lvl, w = frame_levels(x)
    db = 20 * np.log10(lvl + 1e-12)
    floor, loud = np.percentile(db, 10), np.percentile(db, 95)
    n = len(x) // w
    seg = x[:n * w].reshape(n, w)
    e = np.sqrt((seg ** 2).mean(axis=1))
    quiet, loudf = seg[e <= np.percentile(e, 15)], seg[e >= np.percentile(e, 85)]

    print("=== %s" % path)
    print("  格式 : %s %s Hz %s ch %s bit" % (info.get("codec_name"), info.get("sample_rate"),
                                             info.get("channels"), info.get("bits_per_sample") or "?"))
    print("  电平 : RMS %6.1f dBFS | 峰值 %6.1f dBFS" %
          (20 * np.log10(np.sqrt((x ** 2).mean()) + 1e-12), 20 * np.log10(np.abs(x).max() + 1e-12)))
    print("  底噪 %6.1f dBFS | 有声 %6.1f dBFS | SNR %5.1f dB" % (floor, loud, loud - floor))
    print("  频带       安静段    有声段      SNR")
    for lo, hi in BANDS:
        q = band_level(quiet, w, lo, hi); s = band_level(loudf, w, lo, hi)
        print("  %5d-%5d  %7.1f  %7.1f  %6.1f" % (lo, hi, q, s, s - q))

    f, P = ltas(x)
    edges = np.geomspace(100, 10000, 18)
    lv = np.array([10 * np.log10(P[(f >= a) & (f < b)].sum() + 1e-20)
                   for a, b in zip(edges[:-1], edges[1:])])
    ref_band = (edges[:-1] >= 500) & (edges[:-1] < 1000)
    lv = lv - lv[ref_band].mean()
    print("  LTAS（500–1000 Hz = 0 dB，抽点）:")
    for idx in (0, 2, 4, 7, 9, 11, 13, 15, 16):
        if idx < len(lv):
            print("    %6.0f Hz  %+6.1f dB" % (edges[idx], lv[idx]))

    if int(info.get("channels", "1")) >= 2:
        st = decode(path, 2); L, R = st[:, 0], st[:, 1]
        ww = 4096; hop = ww // 2
        print("  立体声相关（逐带）:")
        for lo, hi in [(80, 300), (300, 1000), (1000, 3000), (3000, 8000), (8000, 16000)]:
            cs = []
            for i in range((len(L) - ww) // hop):
                a, b = L[i * hop:i * hop + ww], R[i * hop:i * hop + ww]
                if np.sqrt((a ** 2).mean()) < 3e-3:
                    continue
                A, B = np.fft.rfft(a * np.hanning(ww)), np.fft.rfft(b * np.hanning(ww))
                fq = np.fft.rfftfreq(ww, 1 / SR); m = (fq >= lo) & (fq < hi)
                cs.append(np.corrcoef(np.abs(A[m]), np.abs(B[m]))[0, 1])
            if cs:
                print("    %5d-%5d Hz: %+.2f" % (lo, hi, float(np.mean(cs))))
        accL = np.zeros(ww // 2 + 1); accM = np.zeros(ww // 2 + 1); cnt = 0
        for i in range((len(L) - ww) // hop):
            a, b = L[i * hop:i * hop + ww], R[i * hop:i * hop + ww]
            if np.sqrt((a ** 2).mean()) < 3e-3:
                continue
            win = np.hanning(ww)
            accL += np.abs(np.fft.rfft(a * win)) ** 2
            accM += np.abs(np.fft.rfft((a + b) / 2 * win)) ** 2
            cnt += 1
        fq = np.fft.rfftfreq(ww, 1 / SR)
        print("  单声道下混损失（相对单通道）:")
        for lo, hi in [(300, 1000), (1000, 3000), (3000, 8000), (8000, 16000), (16000, 22000)]:
            m = (fq >= lo) & (fq < hi)
            print("    %5d-%5d Hz: %+5.1f dB" % (lo, hi, 10 * np.log10(accM[m].mean() / accL[m].mean() + 1e-12)))
    if ref is not None:
        rx = decode(ref); rlvl, _ = frame_levels(rx); rdb = 20 * np.log10(rlvl + 1e-12)
        rfloor, rloud = np.percentile(rdb, 10), np.percentile(rdb, 95)
        gain = loud - rloud
        expected = rfloor + gain                 # 纯线性放大后的底噪
        print("  相对基线 '%s':" % ref)
        print("    增益差 %+.1f dB | 期望底噪 %+.1f dBFS | 实测 %+.1f dBFS | 额外降噪 %.1f dB"
              % (gain, expected, floor, expected - floor))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--ref", help="基线文件，用于估算增益归一后的降噪量")
    a = ap.parse_args()
    for i, p in enumerate(a.files):
        if i: print()
        report(p, a.ref)

if __name__ == "__main__":
    main()
