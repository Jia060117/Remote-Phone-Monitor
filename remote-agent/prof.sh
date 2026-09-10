#!/system/bin/sh
# 资源占用实测脚本：agent 进程 CPU / 内存 + screencap 单次开销
P=$(pgrep -f remote-agent | head -1)
echo "=== agent pid=$P ==="
echo "--- /proc/$P/status (内存, kB) ---"
grep -E 'VmSize|VmPeak|VmRSS|VmHWM|Threads|VmSwap' /proc/$P/status

echo "--- CPU 采样 60s (utime+stime ticks, HZ=100) ---"
T0=$(awk '{print $14+$15}' /proc/$P/stat)
sleep 60
T1=$(awk '{print $14+$15}' /proc/$P/stat)
echo "ticks_delta=$((T1-T0))  window=60s  single_core_pct=$(( (T1-T0) / 60 ))"

echo "--- 采样后内存 ---"
grep -E 'VmRSS|VmHWM' /proc/$P/status

echo "=== screencap 单次耗时 (ms) ==="
for i in 1 2 3; do
  S=$(date +%s%N)
  /system/bin/screencap -p /data/local/tmp/pf_full.png
  E=$(date +%s%N)
  echo "full_res_$i: $(( (E-S)/1000000 )) ms  size=$(stat -c %s /data/local/tmp/pf_full.png)"
done
for i in 1 2 3; do
  S=$(date +%s%N)
  /system/bin/screencap -p -w 1080 -h 2400 /data/local/tmp/pf_small.png 2>/data/local/tmp/pf_err.txt
  RC=$?
  E=$(date +%s%N)
  echo "small_${i}: rc=$RC $(( (E-S)/1000000 )) ms  size=$(stat -c %s /data/local/tmp/pf_small.png 2>/dev/null) err=$(head -c 120 /data/local/tmp/pf_err.txt 2>/dev/null)"
done

echo "=== 进程线程数 / 打开的文件数 ==="
echo "threads=$(ls /proc/$P/task | wc -l)  fds=$(ls /proc/$P/fd 2>/dev/null | wc -l)"
rm -f /data/local/tmp/pf_full.png /data/local/tmp/pf_small.png /data/local/tmp/pf_err.txt
echo "=== done ==="
