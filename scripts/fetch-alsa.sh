#!/bin/sh
# 拉取设备上的 libasound（回放用）与主机头文件，供交叉编译链接。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/cross/out/alsa"
DEV=${1:-root@192.168.3.89}
KEY="$ROOT/.e1work/ssh/id_ed25519"
mkdir -p "$OUT/include"
if [ ! -f "$OUT/libasound.so.2.0.0" ]; then
    echo "pull libasound from $DEV"
    ssh -i "$KEY" -o StrictHostKeyChecking=no "$DEV" 'cat /usr/lib/libasound.so.2.0.0' > "$OUT/libasound.so.2.0.0"
    ln -sf libasound.so.2.0.0 "$OUT/libasound.so"
fi
if [ ! -d "$OUT/include/alsa" ]; then
    echo "copy host alsa headers"
    cp -r /usr/include/alsa "$OUT/include/"
fi
echo "alsa ready: $OUT"
