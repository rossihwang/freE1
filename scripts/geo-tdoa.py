#!/usr/bin/env python3
"""麦克风阵列几何测量：从多点播点播的 8 通道原始录音里求每对麦克风的 TDOA。

用法:
  ./geo-tdoa.py capture.wav                     # 自动切段 + 打印 TDOA 矩阵
  ./geo-tdoa.py capture.wav --save tdoa.json    # 同时存机读结果
  ./geo-tdoa.py capture.wav --segments 3.0,18.5,34.0,49.5   # 手工指定四段起点

方法:
  1. 按能量包络切出四段（对应正前/左/右/后四个播点位置），段间静默 > 1.5 s
  2. 每段内对每对通道做 GCC-PHAT（相位变换广义互相关），峰值处抛物线插值到
     亚采样精度（48 kHz 下 1/10 sample ≈ 0.2 µs ≈ 0.07 mm）
  3. 四个方向的 TDOA 用一阶平面波关系换算成麦克风之间的 Δx/Δy:
         Δd_ij(θ) = c·τ_ij(θ) ≈ -(Δx·cosθ + Δy·sinθ)
     θ = 0°(正前) / 90°(左) / 270°(右) / 180°(正后)
  4. 由全部通道对的 Δx/Δy 最小二乘解出每颗麦克风的二维坐标（以 0 号为中心）

注意: 只对原始 8 通道录音有意义。引擎落盘的 2 通道是下混结果，通道间时延已经
被混音破坏，只能拿来看电平/时序，不能量阵列。
"""
import json
import sys

import numpy as np

SPEED_OF_SOUND = 343.0


# ---------------------------------------------------------------- WAV 读取
def read_wav(path):
    raw = open(path, 'rb').read()
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        raise SystemExit(f'{path}: 不是 RIFF/WAVE')
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        size = int.from_bytes(raw[pos + 4:pos + 8], 'little')
        body = raw[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = body
        elif cid == b'data':
            data = body                     # arecord 中途被杀时 size 可能是 0xFFFFFFFF
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise SystemExit(f'{path}: 缺少 fmt/data 块')
    tag = int.from_bytes(fmt[0:2], 'little')
    ch = int.from_bytes(fmt[2:4], 'little')
    rate = int.from_bytes(fmt[4:8], 'little')
    bits = int.from_bytes(fmt[14:16], 'little')
    if tag not in (1, 0xFFFE):
        raise SystemExit(f'{path}: 只支持 PCM/EXTENSIBLE，拿到 format={tag}')
    if bits % 8:
        raise SystemExit(f'{path}: 不支持的位深 {bits}')
    bps = bits // 8
    frame = ch * bps
    n = len(data) // frame
    data = data[:n * frame]
    if bps == 2:
        a = np.frombuffer(data, dtype='<i2').astype(np.float64) / 32768.0
    elif bps == 3:
        b = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - (1 << 24), v)
        a = v.astype(np.float64) / float(1 << 23)
    elif bps == 4:
        a = np.frombuffer(data, dtype='<i4').astype(np.float64) / float(1 << 31)
    else:
        raise SystemExit(f'{path}: 不支持的位深 {bits}')
    return a.reshape(n, ch), rate, bits


# ---------------------------------------------------------------- 切段
def find_segments(x, rate, min_gap=1.5, thr_db=12.0, win_s=0.02):
    """按能量包络切出有声段。返回 [(start_s, end_s), ...]"""
    mono = np.abs(x).max(axis=1)
    w = max(1, int(win_s * rate))
    n = len(mono) // w
    env = mono[:n * w].reshape(n, w).max(axis=1)
    floor = np.percentile(env, 10)
    peak = np.percentile(env, 99.5)
    if peak <= 0:
        return []
    thr = max(floor * (10 ** (thr_db / 20.0)), peak * 1e-3)
    on = env > thr
    segs, start = [], None
    for i, v in enumerate(on):
        if v and start is None:
            start = i
        elif not v and start is not None:
            segs.append((start * w / rate, i * w / rate))
            start = None
    if start is not None:
        segs.append((start * w / rate, n * w / rate))
    merged = []
    for s in segs:
        if merged and s[0] - merged[-1][1] < min_gap:
            merged[-1] = (merged[-1][0], s[1])
        else:
            merged.append(list(s) if False else (s[0], s[1]))
    # 丢掉太短的（起采爆音、单次碰撞）
    return [s for s in merged if s[1] - s[0] > 2.0]


# ---------------------------------------------------------------- GCC-PHAT
def gcc_phat(a, b, rate, max_lag_s=0.0006, up=1):
    """返回 (tau秒, 峰值)。tau > 0 表示 a 比 b 早到。"""
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    A = np.fft.rfft(a, n)
    B = np.fft.rfft(b, n)
    R = A * np.conj(B)
    mag = np.abs(R)
    mag[mag < 1e-12] = 1e-12
    R /= mag
    cc = np.fft.irfft(R, n)
    cc = np.concatenate((cc[-n // 2:], cc[:n // 2]))
    lags = np.arange(-n // 2, n // 2)
    keep = np.abs(lags) <= max(1, int(max_lag_s * rate))
    cc, lags = cc[keep], lags[keep]
    k = int(np.argmax(cc))
    peak = float(cc[k])
    if 0 < k < len(cc) - 1:                       # 抛物线插值到亚采样
        y0, y1, y2 = cc[k - 1], cc[k], cc[k + 1]
        den = (y0 - 2 * y1 + y2)
        delta = 0.5 * (y0 - y2) / den if abs(den) > 1e-15 else 0.0
    else:
        delta = 0.0
    return float((lags[k] + delta) / rate), peak


# ---------------------------------------------------------------- 主流程
def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path = argv[1]
    save = None
    manual = None
    i = 2
    while i < len(argv):
        if argv[i] == '--save':
            save = argv[i + 1]
            i += 2
        elif argv[i] == '--segments':
            manual = [float(v) for v in argv[i + 1].split(',')]
            i += 2
        else:
            raise SystemExit(f'未知参数 {argv[i]}')

    x, rate, bits = read_wav(path)
    nch = x.shape[1]
    dur = len(x) / rate
    print(f'{path}: {dur:.2f} s, {nch} 通道, {rate} Hz, {bits} bit')

    x = x - x.mean(axis=0, keepdims=True)          # PDM/MEMS 直流偏置

    if manual:
        segs = [(s, min(dur, s + 15.0)) for s in manual]
    else:
        segs = find_segments(x, rate)
    print(f'切出 {len(segs)} 段: ' + ', '.join(f'{s:.2f}-{e:.2f}s' for s, e in segs))
    if not segs:
        raise SystemExit('没有可用段，检查录音电平或 --segments')

    names = ['front', 'left', 'right', 'back']
    out = {'file': path, 'rate': rate, 'channels': nch, 'segments': [], 'tdoa': {}}

    for idx, (s0, s1) in enumerate(segs):
        a = int(s0 * rate)
        b = int(s1 * rate)
        seg = x[a:b]
        rms = 20 * np.log10(np.sqrt((seg ** 2).mean(axis=0)) + 1e-12)
        name = names[idx] if idx < len(names) else f'seg{idx}'
        print(f'\n[{name}] {s0:.2f}-{s1:.2f}s  rms(dBFS)=' + ' '.join(f'{v:.1f}' for v in rms))
        mat = np.zeros((nch, nch))
        pk = np.ones((nch, nch))
        for ci in range(nch):
            for cj in range(ci + 1, nch):
                tau, peak = gcc_phat(seg[:, ci], seg[:, cj], rate)
                mat[ci, cj] = tau
                mat[cj, ci] = -tau
                pk[ci, cj] = pk[cj, ci] = peak
        out['segments'].append({'name': name, 'start_s': s0, 'end_s': s1})
        out['tdoa'][name] = {'tau_s': mat.tolist(), 'peak': pk.tolist()}
        for ci in range(nch):
            row = ' '.join(f'{mat[ci, cj] * 1e6:+7.1f}' for cj in range(nch))
            print(f'  ch{ci}: {row}   (µs, 正=本通道先到)')

    # 四方向 → Δx/Δy（一阶平面波）
    order = [s['name'] for s in out['segments']]
    if {'front', 'back'} <= set(order) and {'left', 'right'} <= set(order):
        tf = np.array(out['tdoa']['front']['tau_s'])
        tb = np.array(out['tdoa']['back']['tau_s'])
        tl = np.array(out['tdoa']['left']['tau_s'])
        tr = np.array(out['tdoa']['right']['tau_s'])
        dx = (tb - tf) * SPEED_OF_SOUND / 2.0      # 正后 - 正前
        dy = (tl - tr) * SPEED_OF_SOUND / 2.0      # 左 - 右
        print('\n通道间距估计（一阶平面波，单位 mm，正=该通道在 +x/+y 侧）:')
        print('      ' + ' '.join(f'ch{cj:<5}' for cj in range(nch)))
        for ci in range(nch):
            print(f'  ch{ci} ' + ' '.join(f'{dx[ci, cj] * 1e3:+6.1f}/{dy[ci, cj] * 1e3:+6.1f}'
                                          for cj in range(nch)))
        # 以 ch0 为原点解二维坐标（最小二乘）
        A, rhs = [], []
        for ci in range(nch):
            for cj in range(ci + 1, nch):
                A.append([1.0 if k == ci else (-1.0 if k == cj else 0.0) for k in range(nch)])
                rhs.append([dx[ci, cj], dy[ci, cj]])
        A = np.array(A)
        rhs = np.array(rhs)
        fix = np.zeros((1, nch))
        fix[0, 0] = 1e6
        A = np.vstack([A, fix])
        rhs = np.vstack([rhs, [[0.0, 0.0]]])
        sol, *_ = np.linalg.lstsq(A, rhs, rcond=None)
        sol -= sol[0]
        print('\n麦克风二维坐标（ch0 = 原点，单位 mm）:')
        for ci in range(nch):
            print(f'  ch{ci}: ({sol[ci, 0] * 1e3:+7.1f}, {sol[ci, 1] * 1e3:+7.1f})')
        span = sol.max(axis=0) - sol.min(axis=0)
        print(f'  张角: {span[0] * 1e3:.1f} mm × {span[1] * 1e3:.1f} mm')
        out['positions_mm'] = (sol * 1e3).tolist()

    if save:
        json.dump(out, open(save, 'w'), indent=1)
        print(f'\n写入 {save}')


if __name__ == '__main__':
    main(sys.argv)
