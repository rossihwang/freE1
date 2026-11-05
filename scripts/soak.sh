#!/bin/sh
# 内存/CPU 长时间记录：每分钟一行 CSV（配合 freE1 挂机测试）
OUT=/mnt/sdcard/freE1/soak.csv
[ -f "$OUT" ] || echo "ts,rss_kb,vsz_kb,cpu_ticks,free_kb" > "$OUT"
n=0
while [ "$n" -lt 240 ]; do
    P=$(ps | grep "[f]reE1 ui-dev" | awk '{print $1}' | head -1)
    if [ -n "$P" ]; then
        R=$(grep VmRSS /proc/$P/status 2>/dev/null | awk '{print $2}')
        V=$(grep VmSize /proc/$P/status 2>/dev/null | awk '{print $2}')
        C=$(awk '{print $14 + $15}' /proc/$P/stat 2>/dev/null)
        F=$(df -k /mnt/sdcard | tail -1 | awk '{print $4}')
        echo "$(date +%s),$R,$V,$C,$F" >> "$OUT"
    fi
    n=$((n + 1))
    sleep 60
done
