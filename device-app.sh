#!/bin/bash
# 在 E1 上切换 freE1 UI / 原厂 App
#   ./freE1/device-app.sh enable     # 安装 launcher + 拉起 freE1 UI
#   ./freE1/device-app.sh disable    # 移除 launcher，恢复原厂 App
#   ./freE1/device-app.sh status
set -u
HOST=root@192.168.3.89
KEY=.e1work/ssh/id_ed25519
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
