#!/usr/bin/env bash
# P30 串口老化日志抓取 — 持续读 + 时间戳 + 断线自动重连 + 存盘
# 专为整夜/多小时无人值守老化设计：用 cat 纯读串口，不触发设备复位，
# 设备中途重启/断电导致串口断开时自动重连，不丢监控连续性。
#
# 用法:
#   ./scripts/aging-monitor.sh                      # 默认 /dev/cu.usbmodem2101 115200
#   ./scripts/aging-monitor.sh /dev/cu.usbmodemXXXX # 指定串口
#
# 实时只盯内存趋势(另开一个终端):
#   tail -f ~/p30_aging_*.log | grep --line-buffered 长跑监测

PORT="${1:-/dev/cu.usbmodem2101}"
BAUD="${2:-115200}"
LOG="$HOME/p30_aging_$(date +%Y%m%d_%H%M).log"

echo "老化日志 → $LOG   (Ctrl-C 停止)"
echo "实时盯内存: 另开终端跑  tail -f $LOG | grep 长跑监测"
echo "----------------------------------------"

while true; do
  stty -f "$PORT" "$BAUD" 2>/dev/null
  # cat 纯读不拉 DTR/RTS，不会复位设备(老化中绝不打断)；串口断开则 cat 退出→重连
  cat "$PORT" 2>/dev/null | while IFS= read -r line; do
    printf '%s %s\n' "$(date '+%m-%d %H:%M:%S')" "$line"
  done | tee -a "$LOG"
  printf '%s [串口断开/设备重启? 2s 后重连]\n' "$(date '+%m-%d %H:%M:%S')" | tee -a "$LOG"
  sleep 2
done
