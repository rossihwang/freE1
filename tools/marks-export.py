#!/usr/bin/env python3
"""把录音旁的 `.marks` 标记文件导出成上位机 DAW 能直接读的格式。

用法:
  ./marks-export.py REC_0112_xxx.wav                 # 默认导出 audacity+reaper+csv
  ./marks-export.py --format reaper REC_0112_xxx.wav # 只出某一种
  ./marks-export.py --cue out.wav REC_0112_xxx.wav   # 另存一份带 cue 标记的 WAV copy
  ./marks-export.py --dir /path/to/recordings        # 批量转换整个目录

`.marks` 的格式：每行一个毫秒偏移（见 freE1/src/marks.c）。

支持的输出：
  audacity  标签轨文本（Tab 分隔：起点 终点 标签），Audacity「文件 → 导入 → 标签」
  reaper    一个最小 .rpp 工程：音频 + MARKER 行，双击就能在 Reaper 里看到标记
  csv       index,start_s,end_s,label，给脚本/表格
  cue       在 WAV 副本里写 cue + LIST/adtl/labl 块（Pro Tools、SoundForge、Wavelab 读）
"""
import argparse
import os
import struct
import sys


def read_marks(path):
    """返回排序后的秒偏移列表。坏行跳过（断电可能写坏最后一行）。"""
    out = []
    with open(path, 'r', errors='replace') as f:
        for line in f:
            s = line.strip()
            if not s or not s.lstrip('+').isdigit():
                continue
            out.append(int(s) / 1000.0)
    return sorted(out)


def wav_duration(path):
    with open(path, 'rb') as f:
        head = f.read(44)
        if head[:4] != b'RIFF' or head[8:12] != b'WAVE':
            return 0.0
        # 通过 data 块长度 / byte rate 算时长
        f.seek(12)
        byte_rate = 0
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            cid, size = hdr[:4], struct.unpack('<I', hdr[4:])[0]
            if cid == b'fmt ':
                body = f.read(size)
                byte_rate = struct.unpack_from('<I', body, 8)[0]
            elif cid == b'data':
                if byte_rate:
                    return size / float(byte_rate)
                return 0.0
            else:
                f.seek(size + (size & 1), 1)
    return 0.0


def write_audacity(wav, marks, out):
    with open(out, 'w') as f:
        for i, t in enumerate(marks, 1):
            f.write(f'{t:.3f}\t{t + 0.001:.3f}\tM{i}\n')
    return out


def write_csv(wav, marks, out):
    with open(out, 'w') as f:
        f.write('index,start_s,end_s,label\n')
        for i, t in enumerate(marks, 1):
            f.write(f'{i},{t:.3f},{t + 0.001:.3f},M{i}\n')
    return out


def write_reaper(wav, marks, out):
    name = os.path.basename(wav)
    with open(out, 'w') as f:
        f.write('<REAPER_PROJECT 0.1 "7.0" 1600000000\n')
        f.write('  RIPPLE 0\n  GROUPOVERRIDE 0 0 0\n  AUTOXFADE 129\n')
        f.write('  SAMPLERATE 48000 0 0\n')
        for i, t in enumerate(marks, 1):
            # MARKER <index> <position_s> "<name>" 0 0 1 B
            f.write(f'  MARKER {i} {t:.6f} "M{i}" 0 0 1 B\n')
        f.write('  <TRACK\n')
        f.write('    NAME "recording"\n')
        f.write(f'    <ITEM\n      POSITION 0\n      LENGTH {wav_duration(wav):.6f}\n')
        f.write('      <SOURCE WAVE\n')
        f.write(f'        FILE "{os.path.abspath(wav)}"\n')
        f.write('      >\n    >\n  >\n>\n')
    return out


def write_cue_wav(wav, marks, out):
    """在原 WAV 里插入 cue 与 LIST/adtl/labl 块，另存为新文件（不动原文件）。"""
    with open(wav, 'rb') as f:
        raw = f.read()
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        raise SystemExit(f'{wav}: 不是 WAV')

    # 解析原有块，记录 fmt/data，其余原样保留
    pos, chunks = 12, []
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        size = struct.unpack('<I', raw[pos + 4:pos + 8])[0]
        body = raw[pos + 8:pos + 8 + size]
        chunks.append((cid, body))
        pos += 8 + size + (size & 1)

    fmt = next((b for c, b in chunks if c == b'fmt '), None)
    if fmt is None:
        raise SystemExit(f'{wav}: 没有 fmt 块')
    ch = struct.unpack_from('<H', fmt, 2)[0]
    rate = struct.unpack_from('<I', fmt, 4)[0]

    # cue 块：一个 cue point 对应一个标记
    cue = struct.pack('<I', len(marks))
    for i, t in enumerate(marks, 1):
        sample = int(round(t * rate))
        # id, position, datachunk(0=data), chunkstart, blockstart, sampleoffset
        cue += struct.pack('<IIIIII', i, sample, 0, 0, 0, sample)
    cue += b''.join(struct.pack('<I', 0) for _ in marks)   # 每个 cue 之后是 LABEL chunk id 0

    adtl = b'adtl'
    for i, t in enumerate(marks, 1):
        label = f'M{i}'.encode()
        body = struct.pack('<I', i) + label + b'\x00'
        adtl += b'labl' + struct.pack('<I', len(body)) + body
        if len(body) & 1:
            adtl += b'\x00'
    list_chunk = b'LIST' + struct.pack('<I', len(adtl)) + adtl

    out_chunks = [(b'fmt ', fmt), (b'cue ', cue), (b'LIST', adtl)]
    for c, b in chunks:
        if c in (b'fmt ', b'cue ', b'LIST'):
            continue
        out_chunks.append((c, b))

    body = b''
    for c, b in out_chunks:
        body += c + struct.pack('<I', len(b)) + b + (b'\x00' if len(b) & 1 else b'')
    with open(out, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body)
    return out


def convert(wav, fmt_list, cue_out=None, marks_path=None, prefix=None):
    if not os.path.isfile(wav):
        print(f'skip (no wav): {wav}')
        return
    mp = marks_path or (wav + '.marks')
    if not os.path.isfile(mp):
        print(f'skip (no marks): {mp}')
        return
    marks = read_marks(mp)
    if not marks:
        print(f'skip (empty marks): {mp}')
        return
    base = prefix or wav
    made = []
    for fmt in fmt_list:
        if fmt == 'audacity':
            made.append(write_audacity(wav, marks, base + '.labels.txt'))
        elif fmt == 'csv':
            made.append(write_csv(wav, marks, base + '.marks.csv'))
        elif fmt == 'reaper':
            made.append(write_reaper(wav, marks, base + '.rpp'))
    if cue_out:
        made.append(write_cue_wav(wav, marks, cue_out))
    print(f'{os.path.basename(wav)}: {len(marks)} 个标记 -> ' + ', '.join(os.path.basename(m) for m in made))


def main():
    ap = argparse.ArgumentParser(description='导出 .marks 到 DAW 可用格式')
    ap.add_argument('wav', nargs='?', help='录音文件（.marks 与它同名）')
    ap.add_argument('--dir', help='批量处理目录下所有 wav')
    ap.add_argument('--format', default='audacity,reaper,csv',
                    help='逗号分隔：audacity,reaper,csv（默认全出）')
    ap.add_argument('--cue', metavar='OUT.wav', help='另存一份带 cue 标记的 WAV')
    ap.add_argument('--marks', help='显式指定 .marks 路径')
    args = ap.parse_args()
    fmts = [f.strip() for f in args.format.split(',') if f.strip()]
    bad = [f for f in fmts if f not in ('audacity', 'reaper', 'csv')]
    if bad:
        raise SystemExit(f'未知格式: {bad}')

    if args.dir:
        for n in sorted(os.listdir(args.dir)):
            if n.lower().endswith('.wav'):
                convert(os.path.join(args.dir, n), fmts)
    elif args.wav:
        convert(args.wav, fmts, cue_out=args.cue, marks_path=args.marks)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
