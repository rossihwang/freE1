#!/bin/sh
# 获取构建所需的 LVGL（9.2.2）。仓库不包含 third_party/ 以保持体积。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/hwtest/third_party"
VER=v9.2.2
mkdir -p "$DEST"
if [ -d "$DEST/lvgl/.git" ]; then
    cd "$DEST/lvgl" && git fetch --tags --depth 1 origin "$VER" && git checkout FETCH_HEAD
else
    git clone --depth 1 --branch "$VER" https://github.com/lvgl/lvgl.git "$DEST/lvgl"
fi
echo "LVGL $VER -> $DEST/lvgl"
