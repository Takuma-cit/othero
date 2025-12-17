#!/bin/bash
# test_cpu_usage.sh
# kemeko (768コア) でCPU使用率をテストするスクリプト
#
# 使用方法:
#   リモートマシンにアップロード後、実行:
#   ./test_cpu_usage.sh [スレッド数] [テスト時間(秒)] [空きマス数]
#
# 例:
#   ./test_cpu_usage.sh 384 60 18   # 384スレッドで60秒、18空きテスト
#   ./test_cpu_usage.sh 768 120 24  # 768スレッドで120秒、24空きテスト

THREADS=${1:-384}
TIMEOUT=${2:-120}
EMPTIES=${3:-20}
SOLVER="./othello_hybrid_no_eval"
TEST_POS="test_positions/empties_${EMPTIES}_id_000.pos"

echo "=============================================="
echo "CPU Usage Test for Othello Solver (No-Eval)"
echo "=============================================="
echo "Date: $(date)"
echo "Hostname: $(hostname)"
echo "Threads: $THREADS"
echo "Timeout: ${TIMEOUT}s"
echo "Empties: $EMPTIES"
echo ""

# コンパイル
echo "=== Compiling ==="
gcc -O3 -march=native -pthread -DSTANDALONE_MAIN \
    -o othello_hybrid_no_eval \
    othello_endgame_solver_hybrid_no_eval.c -lm

if [ $? -ne 0 ]; then
    echo "ERROR: Compilation failed!"
    exit 1
fi
echo "Compilation successful."
echo ""

# CPU情報表示
echo "=== CPU Information ==="
lscpu | grep -E "^CPU\(s\):|Model name:|Thread\(s\) per core:|Core\(s\) per socket:|Socket\(s\):"
echo ""

# テストポジションの存在確認
if [ ! -f "$TEST_POS" ]; then
    echo "ERROR: Test position not found: $TEST_POS"
    echo "Available positions:"
    ls test_positions/*.pos 2>/dev/null | head -10
    exit 1
fi

echo "=== Test Position ==="
echo "File: $TEST_POS"
echo ""

# バックグラウンドでCPU監視を開始
echo "=== Starting CPU Monitor (background) ==="
CPU_LOG="cpu_usage_${THREADS}threads.log"
(
    echo "Time,CPU_Usage(%)" > "$CPU_LOG"
    for i in $(seq 1 $((TIMEOUT + 10))); do
        usage=$(top -bn1 | grep "Cpu(s)" | awk '{print $2}' | cut -d'%' -f1)
        echo "$(date +%H:%M:%S),$usage" >> "$CPU_LOG"
        sleep 1
    done
) &
MONITOR_PID=$!
echo "Monitor PID: $MONITOR_PID"
echo ""

# メイン実行
echo "=== Running Solver ==="
echo "Command: timeout ${TIMEOUT}s $SOLVER $TEST_POS $THREADS ${TIMEOUT}.0"
echo ""
echo "--- Output ---"

START_TIME=$(date +%s.%N)
timeout ${TIMEOUT}s $SOLVER "$TEST_POS" $THREADS ${TIMEOUT}.0 2>&1
EXIT_CODE=$?
END_TIME=$(date +%s.%N)

ELAPSED=$(echo "$END_TIME - $START_TIME" | bc)
echo ""
echo "--- End Output ---"
echo ""

# 監視プロセス停止
kill $MONITOR_PID 2>/dev/null
wait $MONITOR_PID 2>/dev/null

echo "=== Results ==="
echo "Exit code: $EXIT_CODE"
echo "Elapsed time: ${ELAPSED}s"
echo ""

# CPU使用率の統計
if [ -f "$CPU_LOG" ]; then
    echo "=== CPU Usage Statistics ==="
    echo "Log file: $CPU_LOG"

    # ヘッダー行をスキップして統計計算
    AVG=$(tail -n +2 "$CPU_LOG" | awk -F',' '{sum+=$2; count++} END {if(count>0) printf "%.1f", sum/count; else print "N/A"}')
    MAX=$(tail -n +2 "$CPU_LOG" | awk -F',' 'BEGIN{max=0} {if($2>max) max=$2} END {printf "%.1f", max}')
    MIN=$(tail -n +2 "$CPU_LOG" | awk -F',' 'BEGIN{min=100} {if($2<min && $2>0) min=$2} END {printf "%.1f", min}')

    echo "Average CPU: ${AVG}%"
    echo "Max CPU: ${MAX}%"
    echo "Min CPU: ${MIN}%"
    echo ""

    # 最後の10秒のCPU使用率
    echo "Last 10 samples:"
    tail -10 "$CPU_LOG"
fi

echo ""
echo "=== Test Complete ==="
echo "Date: $(date)"
