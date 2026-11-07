#!/bin/bash
# 拍手测试：设备上录 5 分钟 8ch，然后本地分析方向性 + 几何
# 用法: ./freE1/scripts/clap-test.sh
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

S="ssh -i $KEY -o StrictHostKeyChecking=no -o ConnectTimeout=6 $HOST"
OUT="$_e1_root/../verify/cap/clap_$(date +%H%M).wav"   # 放在仓库外（项目根的 verify/cap）
mkdir -p "$(dirname "$OUT")"

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
