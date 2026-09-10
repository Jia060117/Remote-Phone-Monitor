#!/usr/bin/env bash
# 把新编译的 agent 推到手机并重启。
#
# 注意：config.json 只在进程启动时读一次，改完必须重启才生效。
# 用法：
#   ./deploy.sh build-cmake/remote-agent-v5.bin
set -euo pipefail

BIN=${1:?用法: ./deploy.sh <本地二进制路径>}
ADB=${ADB:-"$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"}
REMOTE_DIR=/data/adb/modules/remote-monitor
REMOTE_BIN=$REMOTE_DIR/bin/remote-agent

[ -f "$BIN" ] || { echo "找不到二进制: $BIN"; exit 1; }

echo "==> 检查设备"
"$ADB" shell "echo ok" >/dev/null || { echo "没有设备连接（adb devices 为空）"; exit 1; }

echo "==> 停掉旧进程"
"$ADB" shell "su -c 'pkill -f remote-agent'" 2>/dev/null || true
sleep 1
# 确认真的死了，没死透会有两个进程互相抢截屏
echo "    残留进程："
"$ADB" shell "ps -ef | grep remote-agent | grep -v grep" || echo "    （已清空）"

echo "==> 推送 $BIN -> $REMOTE_BIN"
"$ADB" push "$BIN" /data/local/tmp/remote-agent-new
"$ADB" shell "su -c 'mkdir -p $REMOTE_DIR/bin && cp /data/local/tmp/remote-agent-new $REMOTE_BIN && chmod 755 $REMOTE_BIN && rm -f /data/local/tmp/remote-agent-new'"

echo "==> 启动"
"$ADB" shell "su -c 'nohup $REMOTE_BIN --config $REMOTE_DIR/config/config.json >> $REMOTE_DIR/logs/agent.log 2>&1 &'" || \
  "$ADB" shell "su -c 'nohup $REMOTE_BIN --config $REMOTE_DIR/config/config.json >/dev/null 2>&1 &'"

sleep 3
echo
echo "==> 进程状态（应当只有一行）"
"$ADB" shell "ps -ef | grep remote-agent | grep -v grep"
echo
echo "==> 实时日志（Ctrl+C 退出）"
echo "    正常应看到："
echo "      Config: initial sync from server completed"
echo "      Config: effective version=<N>, interval=<N> sec, poll=<N> sec"
echo
"$ADB" logcat -v color -s remote-agent:V
