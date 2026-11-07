#!/bin/sh
# 拉取设备上的 libasound（回放用）与主机头文件，供交叉编译链接。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/cross/out/alsa"
# 设备地址来自本地 e1env（不入库）；没有就在仓库根目录 cp e1env.example e1env
_e1_here=$(cd "$(dirname "$0")" && pwd)
for _e1_f in "$_e1_here/e1env" "$_e1_here/../e1env" "$_e1_here/../../e1env"; do
    if [ -f "$_e1_f" ]; then . "$_e1_f"; break; fi
done
DEV=${1:-$HOST}
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
