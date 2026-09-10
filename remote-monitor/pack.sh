#!/usr/bin/env bash
# 打包 Magisk/KernelSU 模块 zip
set -euo pipefail

cd "$(dirname "$0")"
OUT="remote-monitor.zip"

# 先删旧包
rm -f "$OUT"

# 打包（排除开发文件和旧包）
zip -r "$OUT" \
  module.prop \
  customize.sh \
  service.sh \
  uninstall.sh \
  META-INF/ \
  bin/ \
  config/ \
  -x "*.git*" "*.DS_Store"

echo "==> 已生成: $OUT ($(du -h "$OUT" | cut -f1))"
echo "    通过 Magisk/KernelSU 刷入即可"
