#!/bin/bash
# 把 build_e1ui/freE1 部署到设备并重启 UI
#
#   用法: ./scripts/deploy-ui.sh [--keep-recording]
#
# 安全点：
#   - 只替换 /userdata/freE1/freE1，不碰 /userdata/system/SogouE1 的内容
#     （换之前会把启动脚本临时改成 stub，换完用本仓库的 e1ui-launch.sh 还原并校验 md5）
#   - 每步都用 md5 校验，避免旧版本"看起来部署成功"
#   - 默认会先杀掉正在跑的 arecord 采集（UI 启动时的 rec_recover 也会杀，提前杀更干净）
#     需要保住采集时加 --keep-recording，此时只重启 freE1 本体
set -euo pipefail

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
BIN="$(dirname "$0")/../build_e1ui/freE1"
LAUNCH="$(dirname "$0")/../scripts/e1ui-launch.sh"
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$HOST")

# WiFi 偶尔断一下就够毁掉一次部署：ssh 统一重试三次
rssh(){
	for _ in 1 2 3; do "${SSH[@]}" "$@" && return 0; sleep 2; done
	return 1
}

# 无论怎么退出，都要把启动脚本放回去。否则设备会停在 stub（= 原厂 App）
restore_launcher(){
	if [ "${STUBBED:-0}" = 1 ]; then
		rssh 'cat > /userdata/system/SogouE1.new' < "$LAUNCH" || true
		rssh 'mv -f /userdata/system/SogouE1.new /userdata/system/SogouE1; chmod 755 /userdata/system/SogouE1' || true
		echo "-- launcher restored on exit"
	fi
}
trap restore_launcher EXIT

KEEP_REC=0
[ "${1:-}" = "--keep-recording" ] && KEEP_REC=1

[ -x "$BIN" ] || { echo "缺少 $BIN"; exit 1; }
LOCAL_SUM=$(md5sum "$BIN" | cut -d' ' -f1)
echo "== local $BIN md5=$LOCAL_SUM"

# 0. 确认设备上的启动脚本就是本仓库这一份，否则不敢覆盖
DEV_SUM=$("${SSH[@]}" 'md5sum /userdata/system/SogouE1' | cut -d' ' -f1)
WANT_SUM=$(md5sum "$LAUNCH" | cut -d' ' -f1)
if [ "$DEV_SUM" != "$WANT_SUM" ]; then
	echo "!! 设备启动脚本 md5=$DEV_SUM 与本仓库 $WANT_SUM 不一致，先手动核对"; exit 2
fi

# 1. 停采集（可选）+ 停 UI + 上 stub
if [ "$KEEP_REC" = 0 ]; then
	"${SSH[@]}" 'for p in /proc/[0-9]*; do c=$(tr "\0" " " < $p/cmdline 2>/dev/null); case "$c" in arecord*"hw:2,0"*) kill -9 ${p#/proc/} 2>/dev/null;; esac; done'
fi
STUBBED=1
rssh 'printf "#!/bin/sh\nexec /usr/bin/SogouE1\n" > /userdata/system/SogouE1; chmod 755 /userdata/system/SogouE1; killall -9 freE1 2>/dev/null; sleep 2; echo ok'

# 2. 传新二进制（设备没有 scp，用 ssh 的 stdin）
rssh 'cat > /userdata/freE1/freE1.new' < "$BIN"
GOT=$(rssh 'md5sum /userdata/freE1/freE1.new' | cut -d' ' -f1)
[ "$GOT" = "$LOCAL_SUM" ] || { echo "!! 传输 md5 不符: $GOT"; exit 3; }

# 3. 就位 + 还原启动脚本 + 复位崩溃计数
rssh 'mv -f /userdata/freE1/freE1.new /userdata/freE1/freE1 && chmod 755 /userdata/freE1/freE1 && echo 0 > /userdata/freE1/cnt && md5sum /userdata/freE1/freE1' | grep -q "$LOCAL_SUM" \
	|| { echo "!! 就位后 md5 不符"; exit 4; }
rssh 'cat > /userdata/system/SogouE1.new' < "$LAUNCH"
rssh 'mv -f /userdata/system/SogouE1.new /userdata/system/SogouE1 && chmod 755 /userdata/system/SogouE1'
rssh 'md5sum /userdata/system/SogouE1' | grep -q "$WANT_SUM" || { echo "!! 启动脚本还原失败"; exit 5; }
STUBBED=0

# 4. 触发重启：inittab 的 respawn 条目会重新拉起 SogouLunchProbe.sh → SogouE1
rssh 'killall -9 SogouE1 2>/dev/null; exit 0'
for i in $(seq 1 15); do
	sleep 1
	PID=$(rssh 'ps | grep "/userdata/freE1/freE1" | grep -v grep' || true)
	[ -n "$PID" ] && break
done
if [ -z "${PID:-}" ]; then
	# respawn 没生效（比如本次开机这条路已经走完），手动拉起
	rssh 'setsid /userdata/system/SogouE1 >/dev/null 2>&1 & sleep 1; echo launched'
	sleep 3
fi
PID=$(rssh 'ps | grep "/userdata/freE1/freE1" | grep -v grep' || true)
[ -n "$PID" ] || { echo "!! UI 没起来，看 /mnt/sdcard/freE1/boot.log"; exit 6; }

echo "== device  $PID"
rssh 'tail -6 /mnt/sdcard/freE1/boot.log'
echo "== done"
