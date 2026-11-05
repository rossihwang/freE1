#!/bin/bash
# 拍手测试：设备上录 5 分钟 8ch，然后本地分析方向性 + 几何
# 用法: ./freE1/scripts/clap-test.sh
set -u
HOST=root@192.168.3.89
KEY=.e1work/ssh/id_ed25519
S="ssh -i $KEY -o StrictHostKeyChecking=no -o ConnectTimeout=6 $HOST"
OUT=verify/cap/clap_$(date +%H%M).wav

echo "开始录音 5 分钟，请在设备上按指示拍手..."
$S 'mkdir -p /mnt/sdcard/claptest; rm -f /mnt/sdcard/claptest/*.wav; pkill -x arecord 2>/dev/null;
    nohup arecord -D hw:2,0 -c 8 -r 48000 -f S32_LE -d 300 /mnt/sdcard/claptest/clap.wav >/dev/null 2>&1 &
    sleep 1; ls -l /mnt/sdcard/claptest/'
echo "录音中...（5 分钟）"
echo "请按顺序：① 顶端外 20cm 拍 5 下 → 停 10s → ② 左侧 → 停 10s → ③ 右侧 → 停 10s → ④ 按键端 → 停"
read -r -p "拍完按回车..." _
sleep 2
$S 'kill $(ps | grep "[a]record -D hw:2,0" | awk "{print \$1}") 2>/dev/null; ls -l /mnt/sdcard/claptest/'
ssh -i $KEY -o StrictHostKeyChecking=no $HOST 'cat /mnt/sdcard/claptest/clap.wav' > "$OUT" 2>/dev/null
echo "saved $OUT"
