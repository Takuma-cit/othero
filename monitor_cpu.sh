#!/bin/bash
# monitor_cpu.sh
# リアルタイムでCPU使用率を監視するスクリプト
#
# 使用方法:
#   ./monitor_cpu.sh [間隔(秒)]
#
# 別のターミナルで実行しながらソルバーを起動すると
# CPU使用率をリアルタイムで確認できます

INTERVAL=${1:-1}

echo "CPU Usage Monitor (Press Ctrl+C to stop)"
echo "Interval: ${INTERVAL}s"
echo "----------------------------------------"
echo "Time       | CPU% | Load Avg"
echo "----------------------------------------"

while true; do
    TIME=$(date +%H:%M:%S)

    # CPU使用率取得
    CPU=$(top -bn1 | grep "Cpu(s)" | awk '{print $2}' | cut -d'%' -f1)

    # ロードアベレージ取得
    LOAD=$(uptime | awk -F'load average:' '{print $2}' | xargs)

    printf "%s | %5.1f | %s\n" "$TIME" "$CPU" "$LOAD"

    sleep $INTERVAL
done
