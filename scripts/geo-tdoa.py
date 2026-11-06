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
def click_onsets(x, rate, thr_db=-30.0, skip_s=0.35):
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


# ---------------------------------------------------------------- 绕圈幅度法
def amplitude_distances(taus):
    """声源绕设备转一圈时，τ_ij(t) ≈ (Δp·u(t))/c 是一条正弦；
    它的幅度 (max−min)/2 就等于 |Δp_ij|/c，与声源放在哪些角度无关。
    这比"四个方位两点差分"稳健得多：后者在摆放角度有偏差时会系统性低估间距。"""
    n = max(max(k) for k in taus) + 1
    D = np.zeros((n, n))
    info = {}
    for (i, j), v in taus.items():
        if len(v) < 8:
            continue
        lo, hi = np.percentile(v, 2.5), np.percentile(v, 97.5)
        D[i, j] = D[j, i] = (hi - lo) / 2.0 * C * 1e3        # mm
        info[(i, j)] = (lo, hi, len(v))
    return D, info


def mds_coords(D, idx):
    """经典 MDS：只用两两间距还原二维坐标（朝向/镜像未定）。"""
    S = D[np.ix_(idx, idx)]
    n = len(idx)
    J = np.eye(n) - np.ones((n, n)) / n
    B = -0.5 * J @ (S ** 2) @ J
    w, V = np.linalg.eigh(B)
    order = np.argsort(w)[::-1]
    w, V = w[order], V[:, order]
    coords = V[:, :2] * np.sqrt(np.maximum(w[:2], 0))
    coords -= coords[0]
    return coords


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
    angles = None
    amplitude = False
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
        elif argv[i] == '--amplitude':
            amplitude = True
            i += 1
        elif argv[i] == '--angles':
            angles = [float(v) for v in argv[i + 1].split(',')]
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
        if angles and idx < len(angles):
            name = f'{angles[idx]:.0f}deg'
        elif idx < len(DIRS):
            name = DIRS[idx]
        else:
            name = f'seg{idx}'
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

    names_done = [seg['name'] for seg in out['segments']]

    if amplitude:
        taus = {}
        for ci in range(nch):
            for cj in range(ci + 1, nch):
                v = []
                for t in onsets:
                    a = int((t - 0.002) * rate)
                    b = int((t + 0.020) * rate)
                    if a < 0 or b > len(hp):
                        continue
                    w = hp[a:b]
                    v.append(phat_tau(w[:, ci], w[:, cj], rate, 0.0012)[0])
                taus[(ci, cj)] = np.array(v)
        D, info = amplitude_distances(taus)
        fit_ch = chans if chans else list(range(min(6, nch)))
        print('\n绕圈幅度法（不依赖声源角度）两两间距 mm:')
        print('      ' + ' '.join(f'ch{cj:<5}' for cj in range(nch)))
        for ci in range(nch):
            print(f'  ch{ci} ' + ' '.join(f'{D[ci, cj]:+6.1f}' for cj in range(nch)))
        coords = mds_coords(D, fit_ch)
        print('\nMDS 坐标（mm，ch0 原点，朝向未定）:')
        for k, ci in enumerate(fit_ch):
            print(f'  ch{ci}: ({coords[k, 0]:+7.1f}, {coords[k, 1]:+7.1f})  '
                  f'|p|={np.linalg.norm(coords[k]):.1f}')
        span = coords.max(axis=0) - coords.min(axis=0)
        print(f'  张角: {span[0]:.1f} mm × {span[1]:.1f} mm')
        resid = []
        for a in range(len(fit_ch)):
            for b in range(a + 1, len(fit_ch)):
                resid.append(abs(np.linalg.norm(coords[a] - coords[b]) - D[fit_ch[a], fit_ch[b]]))
        print(f'  距离复原残差中位数: {np.median(resid):.1f} mm')
        if plot:
            draw_plot({f'ch{c}': tuple(coords[k]) for k, c in enumerate(fit_ch)}, plot)
            print(f'写入 {plot}')
        if save:
            out['amplitude_dist_mm'] = D.tolist()
            out['mds_mm'] = {f'ch{c}': coords[k].tolist() for k, c in enumerate(fit_ch)}
            json.dump(out, open(save, 'w'), indent=1)
            print(f'写入 {save}')
        return

    if angles:
        # 每个方位一组方程 c·τ = Δx·cosθ + Δy·sinθ，逐对解 dx/dy（8 个方位即超定）
        use = [(n, np.deg2rad(a)) for n, a in zip(names_done, angles) if n in med]
        if len(use) < 2:
            print('\n可用段不足，跳过几何拟合。')
            return
        dx = np.zeros((nch, nch))
        dy = np.zeros((nch, nch))
        for ci in range(nch):
            for cj in range(ci + 1, nch):
                G = np.array([[np.cos(th), np.sin(th)] for _, th in use])
                rhs = np.array([C * med[n][ci, cj] for n, _ in use])
                sol1, *_ = np.linalg.lstsq(G, rhs, rcond=None)
                dx[ci, cj], dx[cj, ci] = sol1[0], -sol1[0]
                dy[ci, cj], dy[cj, ci] = sol1[1], -sol1[1]
        print(f'\n逐对最小二乘：{len(use)} 个方位, 角度 ' +
              ' '.join(f'{np.rad2deg(th):.0f}°' for _, th in use))
    else:
        if not {'front', 'back', 'left', 'right'} <= set(med):
            print('\n缺方向，跳过几何拟合。段顺序应为 正前 / 左 / 右 / 正后，'
                  '或用 --angles 指定每个方位角。')
            return
        tf, tb = med['front'], med['back']
        tl, tr = med['left'], med['right']
        dx = (tf - tb) * C / 2.0      # c·τ = Δx·cosθ + Δy·sinθ（θ=0 前, 180 后）
        dy = (tl - tr) * C / 2.0      # θ=90 左, 270 右

    worst = max(np.max(np.median(out['tdoa'][k]['mad_s'], axis=0)) for k in names_done)
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
        print('  未参与拟合的通道（只列相对 ch0 的 TDOA）:')
        for ci in range(nch):
            if ci in fit_ch:
                continue
            cells = ' '.join(f'{n}={med[n][0, ci] * 1e6:+5.0f}µs'
                             for n in names_done if n in med)
            print(f'    ch{ci}: {cells}')
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
