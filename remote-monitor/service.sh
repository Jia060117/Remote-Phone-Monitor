#!/system/bin/sh
MODDIR=${0%/*}
AGENT="$MODDIR/bin/remote-agent"
CONFIG="$MODDIR/config/config.json"
LOG="$MODDIR/logs/agent.log"

# 等待开机完成
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 2
done

# 给图形栈/系统服务一点缓冲
sleep 5

mkdir -p "$MODDIR/logs" "$MODDIR/queue"
chmod 755 "$AGENT"
[ -f "$CONFIG" ] || echo "[$(date)] WARN: missing config.json" >> "$LOG"

# Release 模板不能直接运行。初始化程序会同时替换服务地址和设备令牌。
if [ ! -f "$CONFIG" ] || grep -q "CHANGE_ME_DEVICE_TOKEN\|monitor.example.com" "$CONFIG"; then
    echo "[$(date)] ERROR: module is not initialized; run initialize.py from the release bundle" >> "$LOG"
    exit 1
fi

# 守护循环：agent 崩溃/退出后 10 秒自动重启
while true; do
    "$AGENT" --config "$CONFIG" >> "$LOG" 2>&1
    RC=$?
    echo "[$(date)] agent exited with code $RC, restarting in 10s" >> "$LOG"
    sleep 10
done
