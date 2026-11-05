#!/bin/sh
# 电池功耗记录：每分钟一行 CSV
OUT=${1:-/mnt/sdcard/freE1/batt.csv}
DUR=${2:-7200}
P=/sys/class/power_supply/battery
i=0
[ -f "$OUT" ] || echo "ts,uptime,phase,cap,volt_uV,curr_uA,status,temp_mC,free_kb" > "$OUT"
while [ $i -lt "$DUR" ]; do
    up=$(cut -d. -f1 /proc/uptime)
    phase=$(echo rec > /dev/null; if ps | grep -q "[a]record -q -D hw:2,0"; then echo rec; else echo idle; fi)
    cap=$(cat $P/capacity 2>/dev/null)
    volt=$(cat $P/voltage_now 2>/dev/null)
    curr=$(cat $P/current_now 2>/dev/null)
    st=$(cat $P/status 2>/dev/null)
    tmp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
    free=$(df -k /mnt/sdcard | tail -1 | awk '{print $4}')
    echo "$(date +%s),$up,$phase,$cap,$volt,$curr,$st,$tmp,$free" >> "$OUT"
    i=$((i+60)); sleep 60
done
