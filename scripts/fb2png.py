#!/usr/bin/env python3
"""把 dd if=/dev/fb0 得到的 320x320 XRGB8888 原始帧转成 PNG"""
import struct, zlib, sys
raw, out = sys.argv[1], sys.argv[2]
d = open(raw, 'rb').read(); W = H = 320; st = 1280
rgb = bytearray(W*H*3)
for y in range(H):
    row = d[y*st:(y+1)*st]
    for x in range(W):
        b, g, r, a = row[x*4:x*4+4]; o = (y*W+x)*3
        rgb[o] = r; rgb[o+1] = g; rgb[o+2] = b
def ch(t, x): return struct.pack('>I', len(x))+t+x+struct.pack('>I', zlib.crc32(t+x) & 0xffffffff)
data = b''.join(b'\x00'+bytes(rgb[y*W*3:(y+1)*W*3]) for y in range(H))
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' +
    ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
    ch(b'IDAT', zlib.compress(data, 6)) + ch(b'IEND', b''))
