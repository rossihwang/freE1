#!/usr/bin/env python3
"""麦克风阵列几何测量：从多方位播点播的 8 通道原始录音里求每对麦克风的 TDOA，
进而解出每颗麦克风的二维位置。

用法:
  ./geo-tdoa.py capture.wav                       # 自动切段(四段: 正前/左/右/正后)
  ./geo-tdoa.py capture.wav --segments 84.5,108.5,129.5,152.5
  ./geo-tdoa.py capture.wav --save out.json

方法:
  1. 逐次点播检测（包络跳变），每次取约 20 ms 窗，对每对通道做 GCC-PHAT；
     同一段内取中位数，用 MAD 标出不可靠的通道对
  2. 四方位 TDOA 用一阶平面波关系换成 Δx/Δy:
         c·τ_ij = d_j - d_i = Δx·cosθ + Δy·sinθ
     θ = 0°(正前) 90°(左) 180°(正后) 270°(右)，于是
         Δx = c(τ_前 − τ_后)/2 ,  Δy = c(τ_左 − τ_右)/2
  3. 全部通道对的 Δx/Δy 最小二乘解出坐标（ch0 为原点），并给出闭合残差

只对原始 8 通道录音有意义：引擎落盘的 2 通道是下混结果，通道间时延已被混音破坏。
"""
import json
import sys

import numpy as np

C = 343.0            # 声速 m/s
DIRS = ['front', 'left', 'right', 'back']


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
            data = body                     # arecord 被 kill 时 size 可能是 0xFFFFFFFF
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise SystemExit(f'{path}: 缺少 fmt/data 块')
    tag = int.from_bytes(fmt[0:2], 'little')
    ch = int.from_bytes(fmt[2:4], 'little')
    rate = int.from_bytes(fmt[4:8], 'little')
    bits = int.from_bytes(fmt[14:16], 'little')
    if tag not in (1, 0xFFFE):
        raise SystemExit(f'{path}: 只支持 PCM，拿到 format={tag}')
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
def find_segments(x, rate, min_gap=2.0, below_peak_db=20.0, win_s=0.05):
    mono = np.abs(x).max(axis=1)
    w = max(1, int(win_s * rate))
    n = len(mono) // w
    env = mono[:n * w].reshape(n, w).max(axis=1)
    peak = np.percentile(env, 99.9)
    if peak <= 0:
        return []
    on = env > peak * 10 ** (-below_peak_db / 20.0)
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
            merged.append((s[0], s[1]))
    return [s for s in merged if s[1] - s[0] > 3.0]


# ---------------------------------------------------------------- 带通
def bandpass(a, rate, f1, f2):
    """FFT 砖墙带通（边沿用半个八度的升余弦过渡）。点播是宽带脉冲，带通能
    甩掉 MEMS 直流漂移、风噪和超低频，PHAT 的一致性明显变好。"""
    n = a.shape[0]
    N = 1 << int(np.ceil(np.log2(n)))
    f = np.fft.rfftfreq(N, 1.0 / rate)
    m = np.zeros_like(f)
    m[(f >= f1) & (f <= f2)] = 1.0
    up = (f > f2) & (f < f2 * 2)
    m[up] = 0.5 * (1 + np.cos(np.pi * (f[up] - f2) / f2))
    dn = (f < f1) & (f > f1 / 2)
    m[dn] = 0.5 * (1 + np.cos(np.pi * (f1 - f[dn]) / f1))
    out = np.empty_like(a)
    for c in range(a.shape[1]):
        out[:, c] = np.fft.irfft(np.fft.rfft(a[:, c], N) * m, N)[:n]
    return out


# ---------------------------------------------------------------- 点播检测
def click_onsets(x, rate, thr_db=-30.0, skip_s=0.1):
    """返回每次点播的起始时刻（秒）。点播是短促宽带脉冲，包络跳变即可定位。"""
    hp = np.diff(x, axis=0, prepend=x[:1])          # 一阶差分压掉低频漂移
    mono = np.abs(hp).max(axis=1)
    w = max(1, int(0.002 * rate))
    n = len(mono) // w
    env = mono[:n * w].reshape(n, w).max(axis=1)
    thr = 10 ** (thr_db / 20.0)
    out, i, cool = [], 0, int(skip_s * rate / w)
    while i < len(env):
        if env[i] > thr:
            j = min(len(env), i + 3)
            k = i + int(np.argmax(env[i:j]))
            out.append(k * w / rate)
            i = k + cool
        else:
            i += 1
    return out, hp


# ---------------------------------------------------------------- GCC-PHAT
def phat_tau(a, b, rate, max_lag_s=0.0005):
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    R = np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n))
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
    if 0 < k < len(cc) - 1:
        y0, y1, y2 = cc[k - 1], cc[k], cc[k + 1]
        den = y0 - 2 * y1 + y2
        d = 0.5 * (y0 - y2) / den if abs(den) > 1e-15 else 0.0
    else:
        d = 0.0
    return float((lags[k] + d) / rate), peak


def segment_taus(x, rate, t0, t1, onsets, pre_s=0.002, post_s=0.020):
    """段内逐次点播求 TDOA，返回 (中位数矩阵, MAD矩阵, 峰值中位数, 次数)"""
    nch = x.shape[1]
    windows = []
    for t in onsets:
        if not (t0 <= t <= t1):
            continue
        a = int((t - pre_s) * rate)
        b = int((t + post_s) * rate)
        if a < 0 or b > len(x):
            continue
        windows.append(x[a:b])
    if not windows:
        return None
    per = np.zeros((len(windows), nch, nch))
    pk = np.zeros((len(windows), nch, nch))
    for wi, w in enumerate(windows):
        for ci in range(nch):
            for cj in range(ci + 1, nch):
                tau, p = phat_tau(w[:, ci], w[:, cj], rate)
                per[wi, ci, cj] = tau
                per[wi, cj, ci] = -tau
                pk[wi, ci, cj] = pk[wi, cj, ci] = p
    med = np.median(per, axis=0)
    mad = np.median(np.abs(per - med), axis=0)
    return med, mad, np.median(pk, axis=0), len(windows)


# ---------------------------------------------------------------- 出图
def draw_plot(pts, path, pad_mm=8.0):
    """把拟合出的坐标画成示意图（PIL，8 px/mm，+x 朝上=正前，+y 朝右=左）。"""
    from PIL import Image, ImageDraw, ImageFont
    scale = 8.0
    xs = [p[0] for p in pts.values()]
    ys = [p[1] for p in pts.values()]
    x0, x1 = min(xs) - pad_mm, max(xs) + pad_mm
    y0, y1 = min(ys) - pad_mm, max(ys) + pad_mm
    W = max(220, int((y1 - y0) * scale) + 120)
    H = max(220, int((x1 - x0) * scale) + 120)
    img = Image.new('RGB', (W, H), (10, 12, 16))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 14)
        small = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 12)
    except Exception:
        font = small = ImageFont.load_default()
    def px(x_mm, y_mm):
        return (60 + (y_mm - y0) * scale, 60 + (x1 - x_mm) * scale)
    # 5 mm 网格
    gx = y0 - (y0 % 5)
    while gx <= y1:
        a = px(x0, gx); b = px(x1, gx)
        d.line([a, b], fill=(24, 30, 38))
        gx += 5
    gy = x0 - (x0 % 5)
    while gy <= x1:
        a = px(gy, y0); b = px(gy, y1)
        d.line([a, b], fill=(24, 30, 38))
        gy += 5
    # 坐标轴
    d.line([px(x0, 0), px(x1, 0)], fill=(44, 56, 70))
    d.line([px(0, y0), px(0, y1)], fill=(44, 56, 70))
    d.text(px(x1 - 2, 1.5), 'front', fill=(120, 140, 160), font=small)
    d.text(px(-1.5, y1 - 2), 'left', fill=(120, 140, 160), font=small)
    for name, (x, y) in pts.items():
        c = px(x, y)
        r = 7
        d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=(40, 220, 130))
        d.text((c[0] + 10, c[1] - 7), name, fill=(210, 220, 230), font=font)
    d.text((10, 10), 'mic positions (8 px/mm, grid 5 mm)', fill=(140, 150, 165), font=small)
    img.save(path)


# ---------------------------------------------------------------- 主流程
def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path = argv[1]
    save = None
    manual = None
    band = (1200.0, 10000.0)
    chans = None
    plot = None
    i = 2
    while i < len(argv):
        if argv[i] == '--save':
            save = argv[i + 1]
            i += 2
        elif argv[i] == '--segments':
            manual = [float(v) for v in argv[i + 1].split(',')]
            i += 2
        elif argv[i] == '--band':
            f1, f2 = argv[i + 1].split(',')
            band = (float(f1), float(f2))
            i += 2
        elif argv[i] == '--plot':
            plot = argv[i + 1]
            i += 2
        elif argv[i] == '--channels':
            chans = [int(v) for v in argv[i + 1].split(',')]
            i += 2
        else:
            raise SystemExit(f'未知参数 {argv[i]}')

    x, rate, bits = read_wav(path)
    nch = x.shape[1]
    print(f'{path}: {len(x) / rate:.2f} s, {nch} 通道, {rate} Hz, {bits} bit')
    x = x - x.mean(axis=0, keepdims=True)          # MEMS/PDM 直流偏置
    x = bandpass(x, rate, band[0], band[1])
    print(f'带通 {band[0]:.0f}-{band[1]:.0f} Hz')

    if manual:
        segs = [(s, min(len(x) / rate, s + 15.0)) for s in manual]
    else:
        segs = find_segments(x, rate)
    print(f'{len(segs)} 段: ' + ', '.join(f'{s:.2f}-{e:.2f}s' for s, e in segs))
    if not segs:
        raise SystemExit('没有可用段：检查录音电平，或用 --segments 手工指定')

    onsets, hp = click_onsets(x, rate)
    print(f'检测到 {len(onsets)} 次点播')

    out = {'file': path, 'rate': rate, 'channels': nch, 'segments': [], 'tdoa': {}}
    med = {}
    for idx, (s0, s1) in enumerate(segs):
        name = DIRS[idx] if idx < len(DIRS) else f'seg{idx}'
        r = segment_taus(hp, rate, s0, s1, onsets)
        if r is None:
            print(f'[{name}] {s0:.2f}-{s1:.2f}s 无点播，跳过')
            continue
        m, mad, pk, cnt = r
        med[name] = m
        rms = 20 * np.log10(np.sqrt((x[int(s0 * rate):int(s1 * rate)] ** 2).mean(axis=0)) + 1e-12)
        print(f'\n[{name}] {s0:.2f}-{s1:.2f}s  {cnt} 次点播  rms(dBFS)=' +
              ' '.join(f'{v:.1f}' for v in rms))
        print('      ' + ' '.join(f' ch{cj} ' for cj in range(nch)) + '  (µs)')
        for ci in range(nch):
            cells = []
            for cj in range(nch):
                cells.append('    .' if ci == cj else f'{m[ci, cj] * 1e6:+5.0f}')
            print(f'  ch{ci} ' + ' '.join(cells))
        out['segments'].append({'name': name, 'start_s': s0, 'end_s': s1, 'clicks': cnt})
        out['tdoa'][name] = {'tau_s': m.tolist(), 'mad_s': mad.tolist(), 'clicks': cnt}

    if not {'front', 'back', 'left', 'right'} <= set(med):
        print('\n缺方向，跳过几何拟合。段顺序应为 正前 / 左 / 右 / 正后。')
        return

    tf, tb = med['front'], med['back']
    tl, tr = med['left'], med['right']
    dx = (tf - tb) * C / 2.0          # c·τ = Δx·cosθ + Δy·sinθ（θ=0 前, 180 后）
    dy = (tl - tr) * C / 2.0          # θ=90 左, 270 右

    worst = max(np.max(np.median(out['tdoa'][k]['mad_s'], axis=0)) for k in DIRS)
    print(f'\n通道对时延离散度（四段 MAD 的最大中位数）: {worst * 1e6:.1f} µs '
          f'≈ {worst * C * 1e3:.1f} mm')

    fit_ch = chans if chans else list(range(min(6, nch)))
    print('参与拟合的通道: ' + ' '.join(f'ch{c}' for c in fit_ch) +
          ('  (其余通道打印但不参与，见下)' if len(fit_ch) < nch else ''))

    print('\n通道间距（一阶平面波，mm，正 = 该通道在 +x / +y 侧）:')
    print('      ' + ' '.join(f'ch{cj:<5}' for cj in range(nch)))
    for ci in range(nch):
        print(f'  ch{ci} ' + ' '.join(f'{dx[ci, cj] * 1e3:+6.1f}/{dy[ci, cj] * 1e3:+6.1f}'
                                      for cj in range(nch)))

    # 闭合检查：Δx_ij + Δx_jk 应等于 Δx_ik
    err = []
    for ci in fit_ch:
        for cj in fit_ch:
            for ck in fit_ch:
                if len({ci, cj, ck}) == 3:
                    err.append([dx[ci, cj] + dx[cj, ck] - dx[ci, ck],
                                dy[ci, cj] + dy[cj, ck] - dy[ci, ck]])
    err = np.abs(np.array(err)) * 1e3
    print(f'\n闭合残差: |dx| 中位数 {np.median(err[:, 0]):.1f} mm, '
          f'|dy| 中位数 {np.median(err[:, 1]):.1f} mm')

    # Huber 加权最小二乘：闭合残差大的通道对自动降权
    rows, obs = [], []
    for ci in fit_ch:
        for cj in fit_ch:
            if ci < cj:
                rows.append((ci, cj))
                obs.append([dx[ci, cj], dy[ci, cj]])
    obs = np.array(obs)
    nf = len(fit_ch)
    A = np.zeros((len(rows), nf))
    for r, (ci, cj) in enumerate(rows):
        A[r, fit_ch.index(ci)] = 1.0
        A[r, fit_ch.index(cj)] = -1.0
    A2 = np.vstack([A, [[1e6] + [0.0] * (nf - 1)]])
    w = np.ones(len(rows))
    for _ in range(12):
        W = np.concatenate([w, [1e-6]])
        Aw = A2 * W[:, None]
        sol, *_ = np.linalg.lstsq(Aw, np.vstack([obs, [[0.0, 0.0]]]) * W[:, None], rcond=None)
        resid = np.linalg.norm(A @ sol - obs, axis=1)
        s = 1.4826 * np.median(resid)
        s = max(s, 0.002)
        w = np.minimum(1.0, 1.5 * s / np.maximum(resid, 1e-9))

    sol -= sol[0]
    print('\n麦克风坐标（ch0 = 原点，+x = 正前，+y = 左侧，mm）:')
    for k, ci in enumerate(fit_ch):
        print(f'  ch{ci}: ({sol[k, 0] * 1e3:+7.1f}, {sol[k, 1] * 1e3:+7.1f})')
    span = sol.max(axis=0) - sol.min(axis=0)
    print(f'  张角: {span[0] * 1e3:.1f} mm × {span[1] * 1e3:.1f} mm')
    print(f'  残差中位数: {np.median(resid) * 1e3:.1f} mm  '
          f'（被降权(<0.5)的通道对: '
          + (', '.join(f'ch{rows[r][0]}-ch{rows[r][1]}({w[r]:.2f})'
                       for r in range(len(rows)) if w[r] < 0.5) or '无') + '）')
    if len(fit_ch) < nch:
        print('  未参与拟合的通道（此处只打印原始 TDOA，不做坐标）:')
        for ci in range(nch):
            if ci in fit_ch:
                continue
            print(f'    ch{ci}: 正前 {tf[0, ci] * 1e6:+6.0f} µs, 左 {tl[0, ci] * 1e6:+6.0f} µs, '
                  f'右 {tr[0, ci] * 1e6:+6.0f} µs, 正后 {tb[0, ci] * 1e6:+6.0f} µs '
                  f'(相对 ch0)')
    out['positions_mm'] = {f'ch{c}': (sol[k] * 1e3).tolist() for k, c in enumerate(fit_ch)}
    out['fit_channels'] = fit_ch
    out['pair_weight'] = {f'ch{rows[r][0]}-ch{rows[r][1]}': float(w[r]) for r in range(len(rows))}

    if plot:
        draw_plot({f'ch{c}': tuple(sol[k] * 1e3) for k, c in enumerate(fit_ch)}, plot)
        print(f'写入 {plot}')

    if save:
        json.dump(out, open(save, 'w'), indent=1)
        print(f'写入 {save}')


if __name__ == '__main__':
    main(sys.argv)
