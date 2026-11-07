#!/bin/bash
# 在 E1 上切换 freE1 UI / 原厂 App
#   ./freE1/device-app.sh enable     # 安装 launcher + 拉起 freE1 UI
#   ./freE1/device-app.sh disable    # 移除 launcher，恢复原厂 App
#   ./freE1/device-app.sh status
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
cmd=${1:-status}
case "$cmd" in
enable)
  $S 'mkdir -p /userdata/freE1 /mnt/sdcard/freE1' 2>/dev/null
  $S 'cat > /userdata/freE1/freE1.new && chmod +x /userdata/freE1/freE1.new && mv /userdata/freE1/freE1.new /userdata/freE1/freE1' < freE1/build_e1ui/freE1 2>/dev/null
  $S 'cat > /userdata/system/SogouE1 && chmod +x /userdata/system/SogouE1' < freE1/scripts/e1ui-launch.sh 2>/dev/null
  # 先杀掉正在跑的 freE1（init 会 respawn 并 exec 新二进制），再杀原厂 App
  $S 'killall freE1 2>/dev/null; killall SogouE1 2>/dev/null' 2>/dev/null
  sleep 4; $S 'ps | grep "[f]reE1 ui-dev" | head -1; ls -l /proc/$(ps | grep "[f]reE1 ui-dev" | awk "{print \$1}")/exe 2>/dev/null' 2>/dev/null
  ;;
disable)
  $S 'rm -f /userdata/system/SogouE1; killall freE1' 2>/dev/null
  sleep 4; $S 'ps | grep "[S]ogouE1" | head -1' 2>/dev/null
  ;;
status)
  $S 'ps | grep -E "[f]reE1 ui-dev|[S]ogouE1" | head -3; wc -c /userdata/system/SogouE1 2>/dev/null' 2>/dev/null
  ;;
*) echo "usage: $0 enable|disable|status"; exit 1;;
esac
