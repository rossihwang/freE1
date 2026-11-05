#!/bin/sh
# 一键：编译 + 生成演示录音（若没有）+ 启动 PC 预览（X11，HTTP 在 :8080）
set -e
cd "$(dirname "$0")"
make -f Makefile.pc -j"$(nproc)" >/dev/null
if [ ! -d demo ] || [ -z "$(ls -A demo 2>/dev/null)" ]; then
  echo "generating demo recordings..."
  mkdir -p demo
  ./build_pc/freE1 record --dir demo --segment-sec 3 >/dev/null 2>&1 &
  P=$!; sleep 11; kill -9 $P 2>/dev/null || true
  ./build_pc/freE1 recover --dir demo >/dev/null 2>&1 || true
fi
echo "UI on :0  —  drag left/right = pages, click REC/mode. HTTP: http://127.0.0.1:8080/"
exec ./build_pc/freE1 ui --dir demo --http-port 8080
