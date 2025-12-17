#!/bin/bash
################################################################################
# exp2_strong_scaling.sh - 実験2: 強スケーリング実験（最重要）
#
# 目的: コア数増加による性能向上を測定し、並列化効率を評価
#
# 対象環境:
#   CPU: AMD EPYC 9965 192-Core Processor × 2 (768論理コア)
#   RAM: 2.2TB
#   NUMA: 2ノード
#
# スレッド数: 1, 2, 4, 8, 16, 32, 64, 128, 192, 256, 384, 512, 768
#
# 測定項目:
#   - 実行時間 T(p)
#   - Speedup = T(1) / T(p)
#   - Efficiency = Speedup / p × 100%
#   - 並列オーバーヘッド
#   - Worker稼働率
#   - 各並列化機能の発動回数
#
# 5つの並列化機能:
#   [1] ROOT SPLIT:      ルートタスク即座分割
#   [2] MID-SEARCH:      探索中スポーン（50イテレーション毎）
#   [3] DYNAMIC PARAMS:  動的パラメータ調整（アイドル率ベース）
#   [4] EARLY SPAWN:     探索前早期スポーン
#   [5] LOCAL-HEAP-FILL: ローカルヒープ保持スポーン
#
# 出力:
#   - results/exp2_workstealing_scaling.csv
#   - results/exp2_hybrid_scaling.csv
#   - results/exp2_combined_summary.txt
#   - results/exp2_speedup_data.csv (グラフ作成用)
#
# 推定実行時間: 12-24時間
################################################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# 設定
RESULTS_DIR="experiments/results"
LOG_FILE="$RESULTS_DIR/logs/exp2_$(date +%Y%m%d_%H%M%S).log"
CSV_WS="$RESULTS_DIR/exp2_workstealing_scaling.csv"
CSV_HY="$RESULTS_DIR/exp2_hybrid_scaling.csv"
CSV_COMBINED="$RESULTS_DIR/exp2_speedup_data.csv"
SUMMARY_FILE="$RESULTS_DIR/exp2_combined_summary.txt"

mkdir -p "$RESULTS_DIR/logs"

# 環境検出
CORES=$(nproc 2>/dev/null || echo "8")
MEM_GB=$(free -g 2>/dev/null | grep "^Mem:" | awk '{print $2}' || echo "8")

# ログ関数
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

log_header() {
    echo "" | tee -a "$LOG_FILE"
    echo "========================================" | tee -a "$LOG_FILE"
    echo "$*" | tee -a "$LOG_FILE"
    echo "========================================" | tee -a "$LOG_FILE"
}

log_header "実験2: 強スケーリング実験（5機能版）"
log "開始時刻: $(date)"
log "検出環境: $CORES コア, ${MEM_GB}GB RAM"

# ビルド実行
log "ソルバーをビルド中..."
if [ -f "$SCRIPT_DIR/build_solvers.sh" ]; then
    bash "$SCRIPT_DIR/build_solvers.sh" 2>&1 | tee -a "$LOG_FILE"
fi

# 実験パラメータ
# 環境に応じてスレッド数リストを調整
if [ "$CORES" -ge 768 ]; then
    THREAD_COUNTS=(1 2 4 8 16 32 64 128 192 256 384 512 768)
elif [ "$CORES" -ge 64 ]; then
    THREAD_COUNTS=(1 2 4 8 16 32 64)
else
    THREAD_COUNTS=(1 2 4 8)
fi

TIME_LIMIT=600  # 大規模並列では問題が早く解けるので長めに設定
EVAL_FILE="eval/eval.dat"
TRIALS=3  # 各条件で3回実行して平均

# テスト局面（固定問題サイズ）
TEST_POSITION="experiments/test_positions/empties_12_id_000.pos"

if [ ! -f "$TEST_POSITION" ]; then
    TEST_POSITION="test_positions/empties_12_id_000.pos"
fi

if [ ! -f "$TEST_POSITION" ]; then
    log "エラー: テスト局面が見つかりません: $TEST_POSITION"
    exit 1
fi

log "テスト局面: $TEST_POSITION"
log "試行回数: $TRIALS"
log "スレッド数リスト: ${THREAD_COUNTS[*]}"

# CSV ヘッダー作成（5機能メトリクス追加）
cat > "$CSV_WS" <<EOF
Threads,Trial,Time_Sec,Nodes,NPS,Speedup,Efficiency_Percent,Parallel_Overhead,Worker_Util,Subtasks,RootSplits,MidSpawns,DynamicParams,EarlySpawns,LocalHeapFill
EOF

cat > "$CSV_HY" <<EOF
Threads,Trial,Time_Sec,Nodes,NPS,Speedup,Efficiency_Percent,Parallel_Overhead,Worker_Util,Subtasks,RootSplits,MidSpawns,DynamicParams,EarlySpawns,LocalHeapFill
EOF

# ベースライン時間（1スレッド）を保存する変数
BASELINE_TIME_WS=0
BASELINE_TIME_HY=0
BASELINE_NODES=0

# 実験実行関数
run_scaling_test() {
    local solver_name="$1"
    local solver_bin="$2"
    local csv_file="$3"
    local threads="$4"
    local trial="$5"

    log "  試行 $trial: $solver_name - $threads スレッド"

    local output_file="/tmp/exp2_${solver_name}_${threads}t_${trial}_$$.txt"

    # ソルバー実行
    local start_time=$(date +%s.%N)

    # numactlの有無をチェックして実行
    if command -v numactl &> /dev/null && [ "$threads" -ge 64 ]; then
        numactl --interleave=all \
            timeout $((TIME_LIMIT + 60)) "./$solver_bin" "$TEST_POSITION" "$threads" "$TIME_LIMIT" "$EVAL_FILE" -v > "$output_file" 2>&1 || true
    else
        timeout $((TIME_LIMIT + 60)) "./$solver_bin" "$TEST_POSITION" "$threads" "$TIME_LIMIT" "$EVAL_FILE" -v > "$output_file" 2>&1 || true
    fi

    local end_time=$(date +%s.%N)
    local elapsed=$(echo "$end_time - $start_time" | bc 2>/dev/null || echo "0")

    # 結果をパース（Total行から抽出）
    local total_line=$(grep "^Total:" "$output_file" 2>/dev/null | head -1)

    local time_sec="$elapsed"
    local nodes="0"
    local nps="0"

    if [ -n "$total_line" ]; then
        nodes=$(echo "$total_line" | awk '{print $2}')
        time_sec=$(echo "$total_line" | awk '{print $5}')
        nps=$(echo "$total_line" | sed -n 's/.*(\([0-9]*\) NPS).*/\1/p')
    fi

    # 空の場合はデフォルト値
    [ -z "$time_sec" ] && time_sec="$elapsed"
    [ -z "$nodes" ] && nodes="0"
    [ -z "$nps" ] && nps="0"

    # Worker稼働率
    local total_workers=$(grep -E "^.*Worker [0-9]+:" "$output_file" 2>/dev/null | wc -l)
    local active_workers=$(grep -E "^.*Worker [0-9]+:" "$output_file" 2>/dev/null | \
        awk '{gsub(/,/,"",$3); if($3+0 > 0) count++} END {print count+0}')
    local worker_util=0
    if [ "$total_workers" -gt 0 ] 2>/dev/null; then
        worker_util=$(echo "scale=1; $active_workers * 100 / $total_workers" | bc 2>/dev/null || echo "0")
    fi

    # サブタスク数
    local subtasks=$(grep "Subtasks spawned:" "$output_file" 2>/dev/null | head -1 | awk '{print $3}' | tr -d ',')
    [ -z "$subtasks" ] && subtasks="0"

    # 各機能の発動回数（5機能）
    local root_splits=$(grep -c "ROOT SPLIT.*spawned" "$output_file" 2>/dev/null || echo "0")
    local mid_spawns=$(grep -c "MID-SEARCH SPAWN\|periodic spawn" "$output_file" 2>/dev/null || echo "0")
    local dynamic_params=$(grep -c "DYNAMIC PARAMS" "$output_file" 2>/dev/null || echo "0")
    local early_spawns=$(grep -c "EARLY SPAWN" "$output_file" 2>/dev/null || echo "0")
    local local_heap_fill=$(grep -c "LOCAL-HEAP-FILL\|local_fill=YES" "$output_file" 2>/dev/null || echo "0")

    # ベースライン時間の保存（1スレッドの最初の試行）
    if [ "$threads" -eq 1 ] && [ "$trial" -eq 1 ]; then
        if [[ "$solver_name" == "WorkStealing" ]]; then
            BASELINE_TIME_WS=$time_sec
        else
            BASELINE_TIME_HY=$time_sec
        fi
        BASELINE_NODES=$nodes
    fi

    # スピードアップと効率の計算
    local baseline_time
    if [[ "$solver_name" == "WorkStealing" ]]; then
        baseline_time=$BASELINE_TIME_WS
    else
        baseline_time=$BASELINE_TIME_HY
    fi

    # デフォルト値を設定
    [ -z "$baseline_time" ] && baseline_time="0"

    local speedup=1
    local efficiency=100
    local overhead=0

    # 安全な数値計算
    local time_ok=$(echo "$time_sec > 0" | bc 2>/dev/null || echo "0")
    local baseline_ok=$(echo "$baseline_time > 0" | bc 2>/dev/null || echo "0")

    if [ "$time_ok" -eq 1 ] && [ "$baseline_ok" -eq 1 ]; then
        speedup=$(echo "scale=3; $baseline_time / $time_sec" | bc 2>/dev/null || echo "1")
        efficiency=$(echo "scale=2; ($speedup / $threads) * 100" | bc 2>/dev/null || echo "100")

        # 並列オーバーヘッド = (実ノード数 / ベースノード数) - 1
        if [ "$BASELINE_NODES" -gt 0 ] 2>/dev/null && [ "$nodes" -gt 0 ] 2>/dev/null; then
            overhead=$(echo "scale=3; ($nodes / $BASELINE_NODES) - 1" | bc 2>/dev/null || echo "0")
        fi
    fi

    # CSV に追記（5機能メトリクス含む）
    echo "$threads,$trial,$time_sec,$nodes,$nps,$speedup,$efficiency,$overhead,$worker_util,$subtasks,$root_splits,$mid_spawns,$dynamic_params,$early_spawns,$local_heap_fill" >> "$csv_file"

    log "    時間: ${time_sec}s, Speedup: ${speedup}x, 効率: ${efficiency}%, 稼働率: ${worker_util}%"
    log "    機能: ROOT=$root_splits, MID=$mid_spawns, DYN=$dynamic_params, EARLY=$early_spawns, FILL=$local_heap_fill"

    rm -f "$output_file"
}

# メイン実験ループ
log_header "Work-Stealing版 スケーリング実験"

TOTAL_TESTS=$((${#THREAD_COUNTS[@]} * TRIALS * 2))  # 2つのソルバー
CURRENT_TEST=0

if [ -f "othello_endgame_solver_workstealing" ]; then
    for threads in "${THREAD_COUNTS[@]}"; do
        log "スレッド数: $threads"

        for trial in $(seq 1 $TRIALS); do
            CURRENT_TEST=$((CURRENT_TEST + 1))
            log "[$CURRENT_TEST/$TOTAL_TESTS] Work-Stealing版"
            run_scaling_test "WorkStealing" "othello_endgame_solver_workstealing" "$CSV_WS" "$threads" "$trial"
        done
    done
fi

log_header "Hybrid版（5機能版） スケーリング実験"

# Hybrid版バイナリを検索
HYBRID_BIN=""
if [ -f "othello_solver_768core" ]; then
    HYBRID_BIN="othello_solver_768core"
elif [ -f "othello_endgame_solver_hybrid" ]; then
    HYBRID_BIN="othello_endgame_solver_hybrid"
fi

if [ -n "$HYBRID_BIN" ]; then
    for threads in "${THREAD_COUNTS[@]}"; do
        log "スレッド数: $threads"

        for trial in $(seq 1 $TRIALS); do
            CURRENT_TEST=$((CURRENT_TEST + 1))
            log "[$CURRENT_TEST/$TOTAL_TESTS] Hybrid版（5機能版）"
            run_scaling_test "Hybrid" "$HYBRID_BIN" "$CSV_HY" "$threads" "$trial"
        done
    done
fi

# グラフ作成用データの生成
log_header "グラフ作成用データ生成"

cat > "$CSV_COMBINED" <<EOF
Threads,Solver,Avg_Time,Avg_Speedup,Avg_Efficiency,Avg_Worker_Util,Ideal_Speedup,Amdahl_Speedup,Avg_RootSplits,Avg_LocalHeapFill
EOF

# 各スレッド数での平均を計算
for threads in "${THREAD_COUNTS[@]}"; do
    # Work-Stealing版の平均
    ws_avg_time=$(awk -F',' -v t="$threads" '$1==t {sum+=$3; count++} END {if(count>0) printf "%.3f", sum/count; else print "0"}' "$CSV_WS")
    ws_avg_speedup=$(awk -F',' -v t="$threads" '$1==t {sum+=$6; count++} END {if(count>0) printf "%.3f", sum/count; else print "0"}' "$CSV_WS")
    ws_avg_efficiency=$(awk -F',' -v t="$threads" '$1==t {sum+=$7; count++} END {if(count>0) printf "%.2f", sum/count; else print "0"}' "$CSV_WS")
    ws_avg_util=$(awk -F',' -v t="$threads" '$1==t {sum+=$9; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}' "$CSV_WS")
    ws_avg_roots=$(awk -F',' -v t="$threads" '$1==t {sum+=$11; count++} END {if(count>0) printf "%.0f", sum/count; else print "0"}' "$CSV_WS")
    ws_avg_fill=$(awk -F',' -v t="$threads" '$1==t {sum+=$15; count++} END {if(count>0) printf "%.0f", sum/count; else print "0"}' "$CSV_WS")

    # Hybrid版の平均
    hy_avg_time=$(awk -F',' -v t="$threads" '$1==t {sum+=$3; count++} END {if(count>0) printf "%.3f", sum/count; else print "0"}' "$CSV_HY")
    hy_avg_speedup=$(awk -F',' -v t="$threads" '$1==t {sum+=$6; count++} END {if(count>0) printf "%.3f", sum/count; else print "0"}' "$CSV_HY")
    hy_avg_efficiency=$(awk -F',' -v t="$threads" '$1==t {sum+=$7; count++} END {if(count>0) printf "%.2f", sum/count; else print "0"}' "$CSV_HY")
    hy_avg_util=$(awk -F',' -v t="$threads" '$1==t {sum+=$9; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}' "$CSV_HY")
    hy_avg_roots=$(awk -F',' -v t="$threads" '$1==t {sum+=$11; count++} END {if(count>0) printf "%.0f", sum/count; else print "0"}' "$CSV_HY")
    hy_avg_fill=$(awk -F',' -v t="$threads" '$1==t {sum+=$15; count++} END {if(count>0) printf "%.0f", sum/count; else print "0"}' "$CSV_HY")

    # 理想的なスピードアップ（線形）
    ideal_speedup=$threads

    # Amdahlの法則によるスピードアップ（逐次部分10%と仮定）
    # S = 1 / (0.1 + 0.9/p)
    amdahl_speedup=$(echo "scale=3; 1 / (0.1 + 0.9/$threads)" | bc 2>/dev/null || echo "1")

    echo "$threads,WorkStealing,$ws_avg_time,$ws_avg_speedup,$ws_avg_efficiency,$ws_avg_util,$ideal_speedup,$amdahl_speedup,$ws_avg_roots,$ws_avg_fill" >> "$CSV_COMBINED"
    echo "$threads,Hybrid,$hy_avg_time,$hy_avg_speedup,$hy_avg_efficiency,$hy_avg_util,$ideal_speedup,$amdahl_speedup,$hy_avg_roots,$hy_avg_fill" >> "$CSV_COMBINED"
done

# サマリーレポート生成
log_header "サマリーレポート生成"

cat > "$SUMMARY_FILE" <<EOF
═══════════════════════════════════════════════════════════════
実験2: 強スケーリング - サマリーレポート
【768コア・2TB環境専用 5機能版】
═══════════════════════════════════════════════════════════════
実行日時: $(date)
環境: AMD EPYC 9965 × 2 (${CORES}コア), ${MEM_GB}GB RAM
テスト局面: $TEST_POSITION
試行回数: $TRIALS

───────────────────────────────────────────────────────────────
5つの並列化機能
───────────────────────────────────────────────────────────────
[1] ROOT SPLIT:      ルートタスク子ノードを即座にSharedArrayへ
[2] MID-SEARCH:      50イテレーション毎にアイドルチェック
[3] DYNAMIC PARAMS:  アイドル率でG/D/S自動緩和
[4] EARLY SPAWN:     expand直後にスポーン判定
[5] LOCAL-HEAP-FILL: ローカルヒープ<16で全制限解除 ← NEW

───────────────────────────────────────────────────────────────
スケーラビリティ結果
───────────────────────────────────────────────────────────────

スレッド数別 平均スピードアップ:

EOF

# スレッド数ごとの結果をテーブル形式で出力
printf "%-10s %-12s %-12s %-10s %-12s %-12s %-10s\n" "Threads" "WS_Speedup" "WS_Efficiency" "WS_Util" "HY_Speedup" "HY_Efficiency" "HY_Util" >> "$SUMMARY_FILE"
echo "─────────────────────────────────────────────────────────────────────────────────────" >> "$SUMMARY_FILE"

for threads in "${THREAD_COUNTS[@]}"; do
    ws_sp=$(awk -F',' -v t="$threads" '$1==t && $2=="WorkStealing" {print $4}' "$CSV_COMBINED" | head -1)
    ws_ef=$(awk -F',' -v t="$threads" '$1==t && $2=="WorkStealing" {print $5}' "$CSV_COMBINED" | head -1)
    ws_ut=$(awk -F',' -v t="$threads" '$1==t && $2=="WorkStealing" {print $6}' "$CSV_COMBINED" | head -1)
    hy_sp=$(awk -F',' -v t="$threads" '$1==t && $2=="Hybrid" {print $4}' "$CSV_COMBINED" | head -1)
    hy_ef=$(awk -F',' -v t="$threads" '$1==t && $2=="Hybrid" {print $5}' "$CSV_COMBINED" | head -1)
    hy_ut=$(awk -F',' -v t="$threads" '$1==t && $2=="Hybrid" {print $6}' "$CSV_COMBINED" | head -1)

    printf "%-10s %-12s %-12s %-10s %-12s %-12s %-10s\n" "$threads" "${ws_sp}x" "${ws_ef}%" "${ws_ut}%" "${hy_sp}x" "${hy_ef}%" "${hy_ut}%" >> "$SUMMARY_FILE"
done

# 768コアでの最終結果
ws_768=$(awk -F',' '$1==768 && $2=="WorkStealing" {print $4}' "$CSV_COMBINED")
hy_768=$(awk -F',' '$1==768 && $2=="Hybrid" {print $4}' "$CSV_COMBINED")
ws_ef_768=$(awk -F',' '$1==768 && $2=="WorkStealing" {print $5}' "$CSV_COMBINED")
hy_ef_768=$(awk -F',' '$1==768 && $2=="Hybrid" {print $5}' "$CSV_COMBINED")
ws_ut_768=$(awk -F',' '$1==768 && $2=="WorkStealing" {print $6}' "$CSV_COMBINED")
hy_ut_768=$(awk -F',' '$1==768 && $2=="Hybrid" {print $6}' "$CSV_COMBINED")

cat >> "$SUMMARY_FILE" <<EOF

───────────────────────────────────────────────────────────────
768コアでの到達性能
───────────────────────────────────────────────────────────────
Work-Stealing版（5機能なし）:
  スピードアップ: ${ws_768}x
  並列効率: ${ws_ef_768}%
  Worker稼働率: ${ws_ut_768}%

Hybrid版（5機能適用）:
  スピードアップ: ${hy_768}x
  並列効率: ${hy_ef_768}%
  ★ Worker稼働率: ${hy_ut_768}%
  （修正前想定: ~4%, 目標: >90%）

───────────────────────────────────────────────────────────────
LOCAL-HEAP-FILL機能の効果
───────────────────────────────────────────────────────────────
  修正前の問題:
    - 初期状態でタスクを持つワーカーは少数
    - タスクを持つワーカーのみがスポーン可能
    - 768コア中、数十コアしか稼働しない（~4%）

  LOCAL-HEAP-FILL による解決:
    - ローカルヒープ < CHUNK_SIZE(16) の場合
    - 全制限を解除: G=999, S=999, D=2
    - 768コア全てにタスクが行き渡る

───────────────────────────────────────────────────────────────
論文への記載例
───────────────────────────────────────────────────────────────
  図2に、コア数に対するスピードアップ曲線を示す。5機能を
  適用したHybrid版は768コアにおいて${hy_768}倍のスピードアップを
  達成し、並列効率${hy_ef_768}%を維持した。Worker稼働率も
  ${hy_ut_768}%に達し、修正前のWork-Stealing版（${ws_ut_768}%）
  から大幅に改善した。

  特にLOCAL-HEAP-FILL機能により:
  - 初期タスク分配の問題を解決
  - ローカルヒープがチャンク未満なら全制限解除
  - 768コア全てにタスクが行き渡る

  これらの改善により、大規模並列環境でも高いスケーラビリティを
  実現した。

───────────────────────────────────────────────────────────────
グラフ作成方法
───────────────────────────────────────────────────────────────
  1. LibreOffice Calc で $CSV_COMBINED を開く
  2. Threads列とSpeedup列を選択
  3. 挿入 → グラフ → XY散布図
  4. X軸を対数スケールに設定
  5. Ideal_Speedup列を追加して比較線を表示
  6. Avg_Worker_Util列で稼働率推移グラフも作成可能

═══════════════════════════════════════════════════════════════
EOF

log "サマリーレポート生成完了: $SUMMARY_FILE"

# サマリー表示
cat "$SUMMARY_FILE" | tee -a "$LOG_FILE"

log_header "実験2完了"
log "終了時刻: $(date)"
log "結果ファイル:"
log "  - Work-Stealing: $CSV_WS"
log "  - Hybrid: $CSV_HY"
log "  - グラフ用: $CSV_COMBINED"
log "  - サマリー: $SUMMARY_FILE"
log "  - ログ: $LOG_FILE"

log ""
log "次のステップ:"
log "  1. グラフ作成用データを確認: $CSV_COMBINED"
log "  2. 可視化スクリプト実行: python3 utils/plot_scaling.py"

exit 0
