#!/bin/bash
# 10 h 长测结束后，设备端验收：长测核对 → 设备真实采集 → 中途 kill -9 恢复
# 用法: ./freE1/device-test.sh   （需要在 sogou_e1 目录下，设备 WiFi 在线）
set -u
# 设备地址与私钥来自本地 e1env（不入库）；没有就 cp e1env.example e1env 并填写
_e1_here=$(cd "$(dirname "$0")" && pwd)
_e1_root="$_e1_here"; case "$_e1_here" in */scripts) _e1_root=$(cd "$_e1_here/.." && pwd) ;; esac
for _e1_f in "$_e1_root/e1env" "$_e1_here/e1env"; do
    if [ -f "$_e1_f" ]; then . "$_e1_f"; break; fi
done
HOST=${E1_HOST:?未配置设备地址：请 cp e1env.example e1env 并填入 E1_HOST=root@<设备IP>}
if [ -z "${E1_KEY:-}" ]; then
    for _e1_k in "$_e1_root/../.e1work/ssh/id_ed25519" "$_e1_root/.e1work/ssh/id_ed25519" "$HOME/.ssh/id_ed25519"; do
        [ -f "$_e1_k" ] && E1_KEY="$_e1_k" && break
    done
fi
KEY=${E1_KEY:?找不到 ssh 私钥：在 e1env 里设置 E1_KEY=/path/to/id_ed25519}

SSH="ssh -i $KEY -o StrictHostKeyChecking=no -o ConnectTimeout=6 $HOST"
r(){ $SSH "$@" 2>/dev/null; }

echo "=== 1) 10 h 长测核对 ==="
r 'ls -l /mnt/sdcard/verify/t10h.wav; cat /mnt/sdcard/verify/t10h.rc 2>/dev/null || echo "(still running)"'
EXPECT=$(( 36000*48000*2*4 + 44 ))
echo "expect bytes: $EXPECT"
r "echo xruns=\$(grep -ciE 'overrun|xrun' /mnt/sdcard/verify/t10h.log 2>/dev/null); echo tail:; tail -3 /mnt/sdcard/verify/t10h.log 2>/dev/null"

echo "=== 2) 部署最新 UI 二进制 ==="
r 'cat > /userdata/freE1/freE1.new && chmod +x /userdata/freE1/freE1.new && mv /userdata/freE1/freE1.new /userdata/freE1/freE1' < freE1/build_e1ui/freE1
r 'ls -l /userdata/freE1/freE1'

echo "=== 3) 真实采集 60 s（8ch 入 / 2ch 落盘） ==="
r 'rm -rf /mnt/sdcard/freE1test; mkdir -p /mnt/sdcard/freE1test'
r '/userdata/freE1/freE1 record --dir /mnt/sdcard/freE1test --device --segment-sec 60 2>&1 | tail -4'
r 'ls -l /mnt/sdcard/freE1test/'
echo "expect: 60 s × 288000 B/s + 44 = $((60*288000+44)) bytes/段"

echo "=== 4) kill -9 恢复测试 ==="
r 'rm -rf /mnt/sdcard/freE1test2; mkdir -p /mnt/sdcard/freE1test2'
r 'nohup /userdata/freE1/freE1 record --dir /mnt/sdcard/freE1test2 --device --segment-sec 3600 >/dev/null 2>&1 & sleep 5; killall -9 freE1 2>/dev/null || kill -9 $(ps | awk "/[f]reE1 record/{print \$1}") 2>/dev/null; sleep 1; echo killed'
r 'ls -l /mnt/sdcard/freE1test2/'
r '/userdata/freE1/freE1 recover --dir /mnt/sdcard/freE1test2 2>&1 | tail -2'
r 'ls -l /mnt/sdcard/freE1test2/'

echo "=== 5) 状态 ==="
r 'cat /sys/class/power_supply/battery/capacity; for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type)=$(cat $z/temp)"; done'
