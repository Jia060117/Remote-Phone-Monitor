#!/system/bin/sh
# Magisk / KernelSU 模块安装脚本

set_perm $MODPATH/bin/remote-agent 0755 0 0
set_perm_recursive $MODPATH/config 0755 0 0

ui_print "- Remote Phone Monitor v3.0.0 已安装"
ui_print "- 配置: /data/adb/modules/remote-monitor/config/config.json"
ui_print "- 日志: /data/adb/modules/remote-monitor/logs/agent.log"
ui_print ""
ui_print " 请修改 config.json 中的 device_id 和 server.urls 后重启设备"
