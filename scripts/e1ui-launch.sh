#!/bin/sh
# freE1 系统启动：不依赖原厂 App（SogouE1），自己拉起全部必要服务
#   由 /userdata/system/SogouE1 调用（原厂 OTA 槽位），也兼容 /oem/SogouLunch.sh
# 顺序：环境 → 音频通路 → 频点 → WiFi/DHCP → 时间 → SSH → 守卫 → UI

LOG=/mnt/sdcard/freE1/boot.log
mkdir -p /mnt/sdcard/freE1 /userdata/freE1 2>/dev/null
exec 2>&1
[ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 131072 ] && mv "$LOG" "$LOG.1"
echo "=== freE1 boot $(date '+%F %T') ===" >> "$LOG"

# ---------- 1. 环境（原厂 RkEnv.sh 的等价物） ----------
export HOME=/userdata
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/userdata/bin:/data/bin
export LD_LIBRARY_PATH=/data/usrlibs:/usr/local/lib64:/data

# ---------- 2. 音频：原厂 RkLunch.sh + 录音所需的偏置/增益 ----------
# 2.1 输出通路（否则耳机/扬声器无声）。控制名两种命名都试，兼容不同固件
amixer -c 0 sset 'Master' 98% >/dev/null 2>&1 || amixer -c 0 cset name='Master Playback Volume' 98% >/dev/null 2>&1
for ctl in 'DAC LINEOUT Right' 'DAC LINEOUT Right Volume'; do
    amixer -c 0 sset "$ctl" 1 >/dev/null 2>&1 && break; done
for ctl in 'DAC HPMIX Left' 'DAC HPMIX Left Volume'; do
    amixer -c 0 sset "$ctl" 1 >/dev/null 2>&1 && break; done
for ctl in 'DAC HPMIX Right' 'DAC HPMIX Right Volume'; do
    amixer -c 0 sset "$ctl" 1 >/dev/null 2>&1 && break; done
for ctl in 'DAC HPOUT Left' 'DAC HPOUT Left Volume'; do
    amixer -c 0 sset "$ctl" 24 >/dev/null 2>&1 && break; done
for ctl in 'DAC HPOUT Right' 'DAC HPOUT Right Volume'; do
    amixer -c 0 sset "$ctl" 24 >/dev/null 2>&1 && break; done
# 2.2 采集偏置（PDM 阵列依赖模拟 codec 的偏置与增益）
amixer -c 0 sset 'ADC Main MICBIAS' On >/dev/null 2>&1
amixer -c 0 sset 'ADC MICBIAS1' on >/dev/null 2>&1
amixer -c 0 sset 'ADC MICBIAS2' on >/dev/null 2>&1
for n in 1 2 3 4 5 6 7 8; do amixer -c 0 cset numid=$n 3 >/dev/null 2>&1; done
for n in 33 34 35 36 37 38 39 40; do amixer -c 0 cset numid=$n 0 >/dev/null 2>&1; done
echo "audio: mixer applied" >> "$LOG"

# ---------- 3. CPU 频点：默认 816（与原厂一致），设置里可切 Performance ----------
echo 816000 > /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq 2>/dev/null
echo interactive > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null

# ---------- 3.5 清掉上次可能残留的振动/LED 状态 ----------
echo 0 > /proc/motor_vibr 2>/dev/null
echo 0 > /proc/leds_gpio 2>/dev/null
echo 0 > /proc/awled_enable 2>/dev/null

# ---------- 4. WiFi + DHCP ----------
if ! /usr/sbin/wpa_cli -i wlan0 ping >/dev/null 2>&1; then
    /usr/sbin/wpa_supplicant -B -i wlan0 -c /data/cfg/wpa_supplicant.conf >/dev/null 2>&1
    echo "wifi: wpa_supplicant started" >> "$LOG"
fi
if ! ifconfig wlan0 2>/dev/null | grep -q "inet addr"; then
    /sbin/udhcpc -i wlan0 -b -t 8 >/dev/null 2>&1 &
    echo "wifi: dhcp requested" >> "$LOG"
fi

# ---------- 5. 时间：有网就校时（RTC 停 7 年） ----------
(/usr/sbin/ntpd -n -p pool.ntp.org -p ntp.aliyun.com >/dev/null 2>&1 &)

# ---------- 6. SSH 兜底通道 ----------
if [ -x /userdata/bin/dropbear ]; then
    chmod 755 /userdata/root 2>/dev/null
    grep -q " /root " /proc/mounts 2>/dev/null || mount --bind /userdata/root /root 2>/dev/null
    netstat -ltn 2>/dev/null | grep -q ":22 " || /userdata/bin/dropbear -p 22 -r /userdata/dropbear/host_key >/dev/null 2>&1
fi

# ---------- 7. 关机 / 崩溃守卫 ----------
FLAG=/userdata/freE1/poweroff
up=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 99)
[ "$up" -lt 30 ] && rm -f "$FLAG"
if [ -f "$FLAG" ]; then
    echo "guard: poweroff flag, staying on factory app" >> "$LOG"
    exec /usr/bin/SogouE1
fi
CNT=/userdata/freE1/cnt
BIDF=/userdata/freE1/bootid
BID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
OLD=$(cat "$BIDF" 2>/dev/null)
if [ "$BID" != "$OLD" ]; then
    # 新的一次开机：计数清零（正常关机/重启不算崩溃）
    echo 0 > "$CNT"
    echo "$BID" > "$BIDF"
fi
# 运行较久后重启（例如跑完硬件测试返回）不算崩溃：开机 >120 s 就清零
up2=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)
[ "$up2" -gt 120 ] && echo 0 > "$CNT"
n=$(cat "$CNT" 2>/dev/null || echo 0)
if [ "$n" -ge 3 ]; then
    echo "guard: 3 crashes in one boot, falling back" >> "$LOG"
    exec /usr/bin/SogouE1
fi
echo $((n+1)) > "$CNT"

# ---------- 8. 启动 UI ----------
echo "starting freE1 ui-dev (attempt $((n+1)))" >> "$LOG"
# HTTP 服务默认关闭（设置页可手动打开）
exec /userdata/freE1/freE1 ui-dev --dir /mnt/sdcard/freE1 >> "$LOG" 2>&1
