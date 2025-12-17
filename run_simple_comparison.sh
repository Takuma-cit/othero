#!/bin/bash
#
# DeepPN (逐次) vs Work-Stealing並列版 比較ベンチマーク
# シンプル版 - 論文用性能比較データ取得
#

set -e

# ========================================
# 設定
# ========================================
EVAL_FILE="${EVAL_FILE:-eval/eval.dat}"
POS_DIR="${POS_DIR:-test_positions}"
TIME_LIMIT="${TIME_LIMIT:-300}"

# デフォルトテスト設定
EMPTIES_LIST="${EMPTIES_LIST:-10 12 14}"
FILES_PER_EMPTIES="${FILES_PER_EMPTIES:-3}"
THREAD_COUNTS="${THREAD_COUNTS:-1 2 4 8}"

# 出力
LOG_DIR="comparison_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_DIR"

# バイナリ
SOLVER_SEQ="./deep_pns_benchmark"
SOLVER_PAR="./othello_solver_hybrid"
SOLVER_WS="./othello_solver_ws"

# ========================================
# ヘルプ
# ========================================
if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "使用方法: $0 [オプション]"
    echo ""
    echo "環境変数で設定可能:"
    echo "  EMPTIES_LIST      空きマス数リスト (例: \"10 12 14\")"
    echo "  FILES_PER_EMPTIES 各空きマスのファイル数 (例: 3)"
    echo "  THREAD_COUNTS     スレッド数リスト (例: \"1 2 4 8\")"
    echo "  TIME_LIMIT        タイムアウト秒 (例: 300)"
    echo "  POS_DIR           テストポジションディレクトリ"
    echo "  EVAL_FILE         評価関数ファイル"
    echo ""
    echo "例:"
    echo "  EMPTIES_LIST=\"8 10 12\" THREAD_COUNTS=\"1 4 16\" $0"
    echo "  TIME_LIMIT=60 FILES_PER_EMPTIES=5 $0"
    exit 0
fi

# ========================================
# ビルド
# ========================================
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  ソルバービルド                                          ║"
echo "╚══════════════════════════════════════════════════════════╝"

# DeepPN逐次版
if [ -f "Deep_Pns_benchmark.c" ]; then
    echo "Building DeepPN sequential solver..."
    gcc -O3 -march=native -o "$SOLVER_SEQ" Deep_Pns_benchmark.c -lm
    echo "  ✓ $SOLVER_SEQ"
else
    echo "  ⚠ Deep_Pns_benchmark.c not found, skipping sequential"
    SOLVER_SEQ=""
fi

# Hybrid並列版
if [ -f "othello_endgame_solver_hybrid_check_tthit_fixed.c" ]; then
    echo "Building Hybrid parallel solver..."
    gcc -O3 -march=native -pthread -DSTANDALONE_MAIN -DMAX_THREADS=128 \
        -o "$SOLVER_PAR" othello_endgame_solver_hybrid_check_tthit_fixed.c -lm -lpthread
    echo "  ✓ $SOLVER_PAR"
else
    echo "  ⚠ Hybrid solver source not found"
    SOLVER_PAR=""
fi

# Work-Stealing版
if [ -f "othello_endgame_solver_workstealing.c" ]; then
    echo "Building Work-Stealing solver..."
    gcc -O3 -march=native -pthread -DSTANDALONE_MAIN -DMAX_THREADS=128 \
        -o "$SOLVER_WS" othello_endgame_solver_workstealing.c -lm -lpthread
    echo "  ✓ $SOLVER_WS"
else
    echo "  ⚠ Work-Stealing solver source not found"
    SOLVER_WS=""
fi

echo ""

# ========================================
# CSV初期化
# ========================================
CSV_FILE="$LOG_DIR/results.csv"
echo "Empties,FileID,Solver,Threads,Result,Time_sec,Nodes,NPS,Speedup" > "$CSV_FILE"

# 統計用
declare -A seq_times

# ========================================
# テスト実行
# ========================================
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  ベンチマーク実行                                        ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo "空きマス: $EMPTIES_LIST"
echo "ファイル数/空きマス: $FILES_PER_EMPTIES"
echo "スレッド数: $THREAD_COUNTS"
echo "タイムアウト: ${TIME_LIMIT}秒"
echo ""

test_num=0

for empties in $EMPTIES_LIST; do
    emp_pad=$(printf "%02d" $empties)
    
    for fid in $(seq 0 $((FILES_PER_EMPTIES - 1))); do
        fid_pad=$(printf "%03d" $fid)
        pos_file="$POS_DIR/empties_${emp_pad}_id_${fid_pad}.pos"
        
        [ ! -f "$pos_file" ] && continue
        
        test_num=$((test_num + 1))
        key="${empties}_${fid}"
        
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "Test #$test_num: $pos_file (empties=$empties)"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        
        # ────────────────────────────────────────
        # 逐次版 (DeepPN)
        # ────────────────────────────────────────
        if [ -n "$SOLVER_SEQ" ] && [ -x "$SOLVER_SEQ" ]; then
            echo ""
            echo "▶ [Sequential] DeepPN R=1"
            log="$LOG_DIR/seq_${emp_pad}_${fid_pad}.log"
            
            timeout $TIME_LIMIT "$SOLVER_SEQ" "$pos_file" $TIME_LIMIT > "$log" 2>&1 || true
            
            result=$(grep "^Result:" "$log" 2>/dev/null | awk '{print $2}' || echo "TIMEOUT")
            time_s=$(grep "^Time:" "$log" 2>/dev/null | awk '{print $2}' || echo "0")
            nodes=$(grep "^Nodes:" "$log" 2>/dev/null | awk '{print $2}' || echo "0")
            nps=$(grep "^NPS:" "$log" 2>/dev/null | awk '{print $2}' || echo "0")
            
            [ -z "$result" ] && result="TIMEOUT"
            [ -z "$time_s" ] && time_s="$TIME_LIMIT"
            
            seq_times[$key]=$time_s
            
            echo "  Result: $result | Time: ${time_s}s | Nodes: $nodes | NPS: $nps"
            echo "$empties,$fid,Sequential,1,$result,$time_s,$nodes,$nps,1.00" >> "$CSV_FILE"
        fi
        
        # ────────────────────────────────────────
        # 並列版（各スレッド数）
        # ────────────────────────────────────────
        for threads in $THREAD_COUNTS; do
            # Work-Stealing版
            if [ -n "$SOLVER_WS" ] && [ -x "$SOLVER_WS" ]; then
                echo ""
                echo "▶ [WorkStealing] ${threads} threads"
                log="$LOG_DIR/ws_${emp_pad}_${fid_pad}_t${threads}.log"
                
                timeout $TIME_LIMIT "$SOLVER_WS" "$pos_file" $threads $TIME_LIMIT "$EVAL_FILE" -v > "$log" 2>&1 || true
                
                result=$(grep "^Result:" "$log" 2>/dev/null | awk '{print $2}' || echo "TIMEOUT")
                time_s=$(grep "^Total:" "$log" 2>/dev/null | sed 's/.*in \([0-9.]*\)s.*/\1/' || echo "$TIME_LIMIT")
                nodes=$(grep "^Total:" "$log" 2>/dev/null | awk '{print $2}' || echo "0")
                nps=$(grep "NPS" "$log" 2>/dev/null | grep -oE '[0-9]+' | tail -1 || echo "0")
                
                [ -z "$result" ] && result="TIMEOUT"
                [ -z "$time_s" ] && time_s="$TIME_LIMIT"
                
                # スピードアップ計算
                speedup="N/A"
                seq_t=${seq_times[$key]:-0}
                if [ "$seq_t" != "0" ] && [ "$time_s" != "0" ] && [ "$time_s" != "$TIME_LIMIT" ]; then
                    speedup=$(echo "scale=2; $seq_t / $time_s" | bc 2>/dev/null || echo "N/A")
                fi
                
                echo "  Result: $result | Time: ${time_s}s | NPS: $nps | Speedup: ${speedup}x"
                echo "$empties,$fid,WorkStealing,$threads,$result,$time_s,$nodes,$nps,$speedup" >> "$CSV_FILE"
            fi
            
            # Hybrid版
            if [ -n "$SOLVER_PAR" ] && [ -x "$SOLVER_PAR" ]; then
                echo ""
                echo "▶ [Hybrid] ${threads} threads"
                log="$LOG_DIR/hybrid_${emp_pad}_${fid_pad}_t${threads}.log"
                
                timeout $TIME_LIMIT "$SOLVER_PAR" "$pos_file" $threads $TIME_LIMIT "$EVAL_FILE" -v > "$log" 2>&1 || true
                
                result=$(grep "^Result:" "$log" 2>/dev/null | awk '{print $2}' || echo "TIMEOUT")
                time_s=$(grep "^Total:" "$log" 2>/dev/null | sed 's/.*in \([0-9.]*\)s.*/\1/' || echo "$TIME_LIMIT")
                nodes=$(grep "^Total:" "$log" 2>/dev/null | awk '{print $2}' || echo "0")
                nps=$(grep "NPS" "$log" 2>/dev/null | grep -oE '[0-9]+' | tail -1 || echo "0")
                
                [ -z "$result" ] && result="TIMEOUT"
                [ -z "$time_s" ] && time_s="$TIME_LIMIT"
                
                speedup="N/A"
                seq_t=${seq_times[$key]:-0}
                if [ "$seq_t" != "0" ] && [ "$time_s" != "0" ] && [ "$time_s" != "$TIME_LIMIT" ]; then
                    speedup=$(echo "scale=2; $seq_t / $time_s" | bc 2>/dev/null || echo "N/A")
                fi
                
                echo "  Result: $result | Time: ${time_s}s | NPS: $nps | Speedup: ${speedup}x"
                echo "$empties,$fid,Hybrid,$threads,$result,$time_s,$nodes,$nps,$speedup" >> "$CSV_FILE"
            fi
        done
        
        echo ""
    done
done

# ========================================
# サマリー生成
# ========================================
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  ベンチマーク完了                                        ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
echo "テスト数: $test_num"
echo "結果CSV: $CSV_FILE"
echo ""

# 統計サマリー
SUMMARY="$LOG_DIR/summary.txt"
cat > "$SUMMARY" << EOF
═══════════════════════════════════════════════════════════════
比較ベンチマーク結果サマリー
═══════════════════════════════════════════════════════════════
実行日時: $(date)
テスト数: $test_num
空きマス: $EMPTIES_LIST
スレッド: $THREAD_COUNTS

───────────────────────────────────────────────────────────────
ソルバー別統計
───────────────────────────────────────────────────────────────
EOF

# 各ソルバーの平均スピードアップを計算
for solver in Sequential WorkStealing Hybrid; do
    for threads in 1 $THREAD_COUNTS; do
        if [ "$solver" = "Sequential" ] && [ "$threads" != "1" ]; then
            continue
        fi
        
        count=$(grep ",$solver,$threads," "$CSV_FILE" 2>/dev/null | wc -l)
        if [ "$count" -gt 0 ]; then
            avg_speedup=$(grep ",$solver,$threads," "$CSV_FILE" 2>/dev/null | \
                         awk -F',' '{if($9 != "N/A" && $9 > 0) {sum+=$9; n++}} END {if(n>0) printf "%.2f", sum/n; else print "N/A"}')
            solved=$(grep ",$solver,$threads," "$CSV_FILE" 2>/dev/null | grep -cE ",WIN,|,LOSE,|,DRAW," || echo "0")
            echo "$solver (${threads}t): $solved/$count solved, avg speedup: ${avg_speedup}x" >> "$SUMMARY"
        fi
    done
done

echo "" >> "$SUMMARY"
echo "═══════════════════════════════════════════════════════════════" >> "$SUMMARY"

cat "$SUMMARY"
echo ""
echo "詳細: $SUMMARY"
echo "ログ: $LOG_DIR/"
