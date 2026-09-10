#!/system/bin/sh
# 卸载时清理
MODDIR=${0%/*}

# 1. 停止 agent
pkill -f remote-agent 2>/dev/null
sleep 1
pkill -9 -f remote-agent 2>/dev/null

# 2. 清理运行态文件
rm -rf "$MODDIR/logs"/* 2>/dev/null
rm -rf "$MODDIR/queue"/* 2>/dev/null

exit 0
