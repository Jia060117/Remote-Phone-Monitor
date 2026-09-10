#!/system/bin/sh
# 测 screencap 子进程真实 CPU 时间（不含等待 SurfaceFlinger 的 wall time）
echo "=== screencap CPU time (toybox time) ==="
/system/bin/screencap -p /data/local/tmp/t.png >/dev/null 2>&1
echo "--- 预热完成, 开始计时 ---"
i=1
while [ $i -le 5 ]; do
  echo "run $i:"
  time /system/bin/screencap -p /data/local/tmp/t.png
  i=$((i+1))
done
echo "size=$(stat -c %s /data/local/tmp/t.png)"

echo "=== 当前网络类型 ==="
dumpsys connectivity 2>/dev/null | grep -iE "NetworkAgentInfo.*(WIFI|CELLULAR)|type: (WIFI|MOBILE)" | head -4
ip route show 2>/dev/null | head -3

echo "=== agent 全链路: 抓一次采集周期的 CPU 增量 ==="
P=$(pgrep -f remote-agent | head -1)
T0=$(awk '{print $14+$15}' /proc/$P/stat)
sleep 62
T1=$(awk '{print $14+$15}' /proc/$P/stat)
echo "pid=$P ticks_62s=$((T1-T0)) -> 单核占比约 $(( (T1-T0)*100/6200 ))% (含小数另算)"
echo "raw: delta=$((T1-T0))"
rm -f /data/local/tmp/t.png
echo "=== done ==="
