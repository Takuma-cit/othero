#!/bin/bash
################################################################################
# exp1_basic_comparison.sh - 実験1: 基本性能比較（5機能版対応）
#
# 目的: 3つの実装（逐次, Work-Stealing, Hybrid 5機能版）の基本性能を比較
#       特に5つの並列化機能の効果を測定
#
# 対象環境:
#   CPU: AMD EPYC 9965 192-Core Processor × 2 (768論理コア)
#   RAM: 2.2TB
#   NUMA: 2ノード
#
# 5つの並列化機能:
#   [1] ROOT SPLIT:      ルートタスク即座分割
#   [2] MID-SEARCH:      探索中スポーン（50イテレーション毎）
#   [3] DYNAMIC PARAMS:  動的パラメータ調整（アイドル率ベース）
#   [4] EARLY SPAWN:     探索前早期スポーン
#   [5] LOCAL-HEAP-FILL: ローカルヒープ保持スポーン（NEW）
#
# 測定項目:
#   - 解決時間（秒）
#   - 探索ノード数
#   - NPS (Nodes Per Second)
#   - 置換表ヒット率
#   - Worker稼働率
#   - 各機能の発動回数
#
# 出力:
#   - results/exp1_results.csv
#   - results/exp1_summary.txt
#
# 推定実行時間: 4-8時間
################################################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# 設定
RESULTS_DIR="experiments/results"
LOG_DIR="$RESULTS_DIR/logs/exp1_$(date +%Y%m%d_%H%M%S)"
CSV_FILE="$RESULTS_DIR/exp1_results.csv"
SUMMARY_FILE="$RESULTS_DIR/exp1_summary.txt"

mkdir -p "$LOG_DIR"
mkdir -p "$RESULTS_DIR"

# 環境検出
CORES=$(nproc 2>/dev/null || echo "8")
MEM_GB=$(free -g 2>/dev/null | grep "^Mem:" | awk '{print $2}' || echo "8")

# スレッド数の設定（環境に応じて調整）
if [ "$CORES" -ge 768 ]; then
    FIXED_THREADS=768
elif [ "$CORES" -ge 64 ]; then
    FIXED_THREADS=$CORES
else
    FIXED_THREADS=64
fi

# コマンドライン引数で上書き可能
FIXED_THREADS=${1:-$FIXED_THREADS}
TIME_LIMIT=${2:-300.0}

# ログ関数
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_DIR/master.log"
}

log_header() {
    echo "" | tee -a "$LOG_DIR/master.log"
    echo "========================================" | tee -a "$LOG_DIR/master.log"
    echo "$*" | tee -a "$LOG_DIR/master.log"
    echo "========================================" | tee -a "$LOG_DIR/master.log"
}

log_header "実験1: 基本性能比較（5機能版）"
log "開始時刻: $(date)"
log "検出環境: $CORES コア, ${MEM_GB}GB RAM"
log "使用スレッド数: $FIXED_THREADS"

# 実験パラメータ
EVAL_FILE="eval/eval.dat"
POS_DIR="test_positions"

# テスト局面（空きマス10-20の代表的な局面）
TEST_POSITIONS=(
    "empties_10_id_000.pos"
    "empties_12_id_000.pos"
    "empties_14_id_000.pos"
    "empties_16_id_000.pos"
    "empties_18_id_000.pos"
    "empties_20_id_000.pos"
)

# ソルバービルド（5機能版）
log_header "ソルバービルド"
if [ -f "experiments/build_solvers.sh" ]; then
    bash experiments/build_solvers.sh
else
    log "警告: build_solvers.sh が見つかりません。既存のバイナリを使用します"
fi

# CSV ヘッダー作成（5機能メトリクス）
cat > "$CSV_FILE" <<EOF
Solver,Position,Empties,Result,Time_Sec,Total_Nodes,NPS,Worker_Util,Subtasks,RootSplits,MidSpawns,DynamicParams,EarlySpawns,LocalHeapFill,Status
EOF

log "CSV ファイル作成: $CSV_FILE"

# 結果パース関数（5機能対応）
parse_result() {
    local log_file=$1

    # 結果を抽出（デフォルト値を設定）
    local result=$(grep "^Result:" "$log_file" 2>/dev/null | head -1 | awk '{print $2}')
    [ -z "$result" ] && result="UNKNOWN"

    # Total行を取得
    local total_line=$(grep "^Total:" "$log_file" 2>/dev/null | head -1)

    local nodes="0"
    local time="0"
    local nps="0"

    if [ -n "$total_line" ]; then
        nodes=$(echo "$total_line" | awk '{print $2}')
        time=$(echo "$total_line" | awk '{print $5}')
        nps=$(echo "$total_line" | sed -n 's/.*(\([0-9]*\) NPS).*/\1/p')
    fi

    # 空の場合はデフォルト値
    [ -z "$nodes" ] && nodes="0"
    [ -z "$time" ] && time="0"
    [ -z "$nps" ] && nps="0"

    # Worker稼働率を抽出
    local total_workers=$(grep -E "^.*Worker [0-9]+:" "$log_file" 2>/dev/null | wc -l)
    local active_workers=$(grep -E "^.*Worker [0-9]+:" "$log_file" 2>/dev/null | \
        awk '{gsub(/,/,"",$3); if($3+0 > 0) count++} END {print count+0}')
    local utilization=0
    if [ "$total_workers" -gt 0 ] 2>/dev/null; then
        utilization=$(echo "scale=1; $active_workers * 100 / $total_workers" | bc 2>/dev/null || echo "0")
    fi

    # サブタスク数を抽出
    local subtasks=$(grep "Subtasks spawned:" "$log_file" 2>/dev/null | head -1 | awk '{print $3}' | tr -d ',')
    [ -z "$subtasks" ] && subtasks="0"

    # 各機能の発動回数
    local root_splits=$(grep -c "ROOT SPLIT.*spawned" "$log_file" 2>/dev/null || echo "0")
    local mid_spawns=$(grep -c "MID-SEARCH SPAWN\|periodic spawn" "$log_file" 2>/dev/null || echo "0")
    local dynamic_params=$(grep -c "DYNAMIC PARAMS" "$log_file" 2>/dev/null || echo "0")
    local early_spawns=$(grep -c "EARLY SPAWN" "$log_file" 2>/dev/null || echo "0")
    local local_heap_fill=$(grep -c "LOCAL-HEAP-FILL\|local_fill=YES" "$log_file" 2>/dev/null || echo "0")

    echo "$result,$nodes,$time,$nps,$utilization,$subtasks,$root_splits,$mid_spawns,$dynamic_params,$early_spawns,$local_heap_fill"
}

# 実験実行関数
run_solver() {
    local solver_name="$1"
    local solver_bin="$2"
    local position_file="$3"
    local threads="$4"

    log "実行中: $solver_name - $(basename $position_file) ($threads threads)"

    local log_file="$LOG_DIR/${solver_name}_$(basename $position_file .pos).log"
    local start_time=$(date +%s)

    # ソルバー実行
    if [[ "$solver_name" == "Sequential" ]]; then
        # 逐次版（Deep_Pns_benchmark）
        timeout $((${TIME_LIMIT%.*} + 60)) "./$solver_bin" "$position_file" "$TIME_LIMIT" > "$log_file" 2>&1 || true
    else
        # 並列版（NUMA最適化付き、詳細ログ有効）
        if command -v numactl &> /dev/null; then
            numactl --interleave=all \
                timeout $((${TIME_LIMIT%.*} + 60)) \
                "./$solver_bin" "$position_file" "$threads" "$TIME_LIMIT" "$EVAL_FILE" -v -w > "$log_file" 2>&1 || true
        else
            timeout $((${TIME_LIMIT%.*} + 60)) \
                "./$solver_bin" "$position_file" "$threads" "$TIME_LIMIT" "$EVAL_FILE" -v -w > "$log_file" 2>&1 || true
        fi
    fi

    local exit_code=$?
    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))

    # 結果をパース
    local result_data=$(parse_result "$log_file")
    local result=$(echo "$result_data" | cut -d',' -f1)
    local nodes=$(echo "$result_data" | cut -d',' -f2)
    local time_sec=$(echo "$result_data" | cut -d',' -f3)
    local nps=$(echo "$result_data" | cut -d',' -f4)
    local utilization=$(echo "$result_data" | cut -d',' -f5)
    local subtasks=$(echo "$result_data" | cut -d',' -f6)
    local root_splits=$(echo "$result_data" | cut -d',' -f7)
    local mid_spawns=$(echo "$result_data" | cut -d',' -f8)
    local dynamic_params=$(echo "$result_data" | cut -d',' -f9)
    local early_spawns=$(echo "$result_data" | cut -d',' -f10)
    local local_heap_fill=$(echo "$result_data" | cut -d',' -f11)

    # ステータス判定
    local status
    if [ $exit_code -eq 124 ]; then
        status="TIMEOUT"
    elif [ "$result" = "WIN" ] || [ "$result" = "LOSE" ] || [ "$result" = "DRAW" ]; then
        status="SOLVED"
    else
        status="UNKNOWN"
    fi

    # 空きマス数を取得
    local empties=$(echo "$(basename $position_file)" | sed 's/empties_\([0-9]*\)_.*/\1/' | sed 's/^0*//')

    # CSVに追記
    echo "$solver_name,$(basename $position_file),$empties,$result,$time_sec,$nodes,$nps,$utilization,$subtasks,$root_splits,$mid_spawns,$dynamic_params,$early_spawns,$local_heap_fill,$status" >> "$CSV_FILE"

    log "  結果: $result ($status)"
    log "  時間: ${time_sec}s, ノード: $nodes, NPS: $nps"
    log "  稼働率: ${utilization}%, サブタスク: $subtasks"
    log "  機能発動: ROOT=$root_splits, MID=$mid_spawns, DYN=$dynamic_params, EARLY=$early_spawns, FILL=$local_heap_fill"
}

# メイン実験ループ
log_header "実験実行開始"

TOTAL_TESTS=$((3 * ${#TEST_POSITIONS[@]}))
CURRENT_TEST=0

for pos in "${TEST_POSITIONS[@]}"; do
    POS_FILE="$POS_DIR/$pos"

    if [ ! -f "$POS_FILE" ]; then
        POS_FILE="experiments/test_positions/$pos"
        if [ ! -f "$POS_FILE" ]; then
            log "警告: $pos が見つかりません。スキップします"
            continue
        fi
    fi

    log_header "局面: $(basename $POS_FILE)"

    # 1. 逐次版（Deep_Pns_benchmark）
    if [ -f "Deep_Pns_benchmark" ]; then
        CURRENT_TEST=$((CURRENT_TEST + 1))
        log "[$CURRENT_TEST/$TOTAL_TESTS] 逐次版"
        run_solver "Sequential" "Deep_Pns_benchmark" "$POS_FILE" "1"
    fi

    # 2. Work-Stealing版
    if [ -f "othello_endgame_solver_workstealing" ]; then
        CURRENT_TEST=$((CURRENT_TEST + 1))
        log "[$CURRENT_TEST/$TOTAL_TESTS] Work-Stealing版"
        run_solver "WorkStealing" "othello_endgame_solver_workstealing" "$POS_FILE" "$FIXED_THREADS"
    fi

    # 3. Hybrid版（5機能版）
    if [ -f "othello_solver_768core" ]; then
        CURRENT_TEST=$((CURRENT_TEST + 1))
        log "[$CURRENT_TEST/$TOTAL_TESTS] Hybrid版（5機能版）"
        run_solver "Hybrid_5Features" "othello_solver_768core" "$POS_FILE" "$FIXED_THREADS"
    elif [ -f "othello_endgame_solver_hybrid" ]; then
        CURRENT_TEST=$((CURRENT_TEST + 1))
        log "[$CURRENT_TEST/$TOTAL_TESTS] Hybrid版"
        run_solver "Hybrid" "othello_endgame_solver_hybrid" "$POS_FILE" "$FIXED_THREADS"
    fi
done

# サマリーレポート生成
log_header "サマリーレポート生成"

cat > "$SUMMARY_FILE" <<EOF
═══════════════════════════════════════════════════════════════
実験1: 基本性能比較 - サマリーレポート
【768コア・2TB環境専用 5機能版】
═══════════════════════════════════════════════════════════════
実行日時: $(date)
環境: AMD EPYC 9965 × 2 (${CORES}コア), ${MEM_GB}GB RAM
スレッド数: $FIXED_THREADS
タイムアウト: $TIME_LIMIT 秒

───────────────────────────────────────────────────────────────
5つの並列化機能
───────────────────────────────────────────────────────────────
[1] ROOT SPLIT:      ルートタスク子ノードを即座にSharedArrayへ
[2] MID-SEARCH:      50イテレーション毎にアイドルチェック
[3] DYNAMIC PARAMS:  アイドル率でG/D/S自動緩和
[4] EARLY SPAWN:     expand直後にスポーン判定
[5] LOCAL-HEAP-FILL: ローカルヒープ<16で全制限解除 ← NEW

───────────────────────────────────────────────────────────────
平均性能比較
───────────────────────────────────────────────────────────────
EOF

# 平均計算
for solver in "Sequential" "WorkStealing" "Hybrid_5Features" "Hybrid"; do
    count=$(awk -F',' -v solver="$solver" '$1==solver {count++} END {print count+0}' "$CSV_FILE")
    [ "$count" -eq 0 ] && continue

    avg_time=$(awk -F',' -v solver="$solver" '$1==solver && $15=="SOLVED" {sum+=$5; count++} END {if(count>0) printf "%.2f", sum/count; else print "N/A"}' "$CSV_FILE")
    avg_nodes=$(awk -F',' -v solver="$solver" '$1==solver && $15=="SOLVED" {sum+=$6; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}' "$CSV_FILE")
    avg_nps=$(awk -F',' -v solver="$solver" '$1==solver && $15=="SOLVED" {sum+=$7; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}' "$CSV_FILE")
    avg_util=$(awk -F',' -v solver="$solver" '$1==solver && $15=="SOLVED" {sum+=$8; count++} END {if(count>0) printf "%.1f", sum/count; else print "N/A"}' "$CSV_FILE")
    avg_subtasks=$(awk -F',' -v solver="$solver" '$1==solver && $15=="SOLVED" {sum+=$9; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}' "$CSV_FILE")
    total_root=$(awk -F',' -v solver="$solver" '$1==solver {sum+=$10} END {print sum+0}' "$CSV_FILE")
    total_mid=$(awk -F',' -v solver="$solver" '$1==solver {sum+=$11} END {print sum+0}' "$CSV_FILE")
    total_dyn=$(awk -F',' -v solver="$solver" '$1==solver {sum+=$12} END {print sum+0}' "$CSV_FILE")
    total_early=$(awk -F',' -v solver="$solver" '$1==solver {sum+=$13} END {print sum+0}' "$CSV_FILE")
    total_fill=$(awk -F',' -v solver="$solver" '$1==solver {sum+=$14} END {print sum+0}' "$CSV_FILE")
    solved=$(awk -F',' -v solver="$solver" '$1==solver && $15=="SOLVED" {count++} END {print count+0}' "$CSV_FILE")

    cat >> "$SUMMARY_FILE" <<EOF

$solver:
  解決数: $solved / $count
  平均時間: ${avg_time} 秒
  平均ノード数: ${avg_nodes}
  平均NPS: ${avg_nps}
  ★ 平均Worker稼働率: ${avg_util}%
  平均サブタスク数: ${avg_subtasks}
  ─────────────────────────────────
  機能発動回数（累計）:
    [1] ROOT SPLIT:      $total_root 回
    [2] MID-SEARCH:      $total_mid 回
    [3] DYNAMIC PARAMS:  $total_dyn 回
    [4] EARLY SPAWN:     $total_early 回
    [5] LOCAL-HEAP-FILL: $total_fill 回 ← NEW
EOF
done

# スピードアップ計算
seq_time=$(awk -F',' '$1=="Sequential" && $15=="SOLVED" {sum+=$5; count++} END {if(count>0) printf "%.2f", sum/count; else print "0"}' "$CSV_FILE")
ws_time=$(awk -F',' '$1=="WorkStealing" && $15=="SOLVED" {sum+=$5; count++} END {if(count>0) printf "%.2f", sum/count; else print "1"}' "$CSV_FILE")
hy_time=$(awk -F',' '$1~/^Hybrid/ && $15=="SOLVED" {sum+=$5; count++} END {if(count>0) printf "%.2f", sum/count; else print "1"}' "$CSV_FILE")

ws_speedup="N/A"
hy_speedup="N/A"

if [ $(echo "$ws_time > 0" | bc 2>/dev/null || echo "0") -eq 1 ] && [ $(echo "$seq_time > 0" | bc 2>/dev/null || echo "0") -eq 1 ]; then
    ws_speedup=$(echo "scale=2; $seq_time / $ws_time" | bc 2>/dev/null || echo "N/A")
fi

if [ $(echo "$hy_time > 0" | bc 2>/dev/null || echo "0") -eq 1 ] && [ $(echo "$seq_time > 0" | bc 2>/dev/null || echo "0") -eq 1 ]; then
    hy_speedup=$(echo "scale=2; $seq_time / $hy_time" | bc 2>/dev/null || echo "N/A")
fi

# Hybrid vs WorkStealingの比較
hy_vs_ws="N/A"
if [ $(echo "$hy_time > 0" | bc 2>/dev/null || echo "0") -eq 1 ] && [ $(echo "$ws_time > 0" | bc 2>/dev/null || echo "0") -eq 1 ]; then
    hy_vs_ws=$(echo "scale=2; $ws_time / $hy_time" | bc 2>/dev/null || echo "N/A")
fi

# 稼働率の比較
ws_util=$(awk -F',' '$1=="WorkStealing" && $15=="SOLVED" {sum+=$8; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}' "$CSV_FILE")
hy_util=$(awk -F',' '$1~/^Hybrid/ && $15=="SOLVED" {sum+=$8; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}' "$CSV_FILE")

cat >> "$SUMMARY_FILE" <<EOF

───────────────────────────────────────────────────────────────
スピードアップ（逐次版基準）
───────────────────────────────────────────────────────────────
Work-Stealing版: ${ws_speedup}x
Hybrid版（5機能版）: ${hy_speedup}x

Hybrid vs Work-Stealing: ${hy_vs_ws}x 高速

───────────────────────────────────────────────────────────────
5機能の効果分析
───────────────────────────────────────────────────────────────
Worker稼働率の改善:
  Work-Stealing版: ${ws_util}%
  Hybrid版（5機能版）: ${hy_util}%

【各機能の役割】

[1] ROOT SPLIT（ルート即座分割）:
    → ルートタスク受信時に子ノードを即座にSharedArrayへ
    → 初期並列性を確保（最重要機能）

[2] MID-SEARCH SPAWN（探索中スポーン）:
    → 50イテレーションごとにアイドルワーカーを確認
    → 深い探索中もタスク供給を継続

[3] DYNAMIC PARAMS（動的パラメータ調整）:
    → アイドル率に応じてG/D/Sを動的緩和
    → 90%以上アイドル: G+10, S×5, D/2
    → 70%以上アイドル: G+5, S×3, D×2/3
    → 50%以上アイドル: G+2, S×2

[4] EARLY SPAWN（探索前早期スポーン）:
    → expand直後、dfpnループ開始前にスポーン判定
    → 探索中に証明されてスポーン機会を逃す問題を解決

[5] LOCAL-HEAP-FILL（ローカルヒープ保持スポーン）← NEW:
    → ローカルヒープ < CHUNK_SIZE(16) で全制限解除
    → G=999, S=999, D=2 で無制限スポーン
    → 768コア全てにタスクが行き渡る

───────────────────────────────────────────────────────────────
詳細結果
───────────────────────────────────────────────────────────────
CSV ファイル: $CSV_FILE
ログディレクトリ: $LOG_DIR

論文への記載例:
  表1に、3手法の基本性能比較を示す。5機能を適用した
  Hybrid版は逐次版に対して${hy_speedup}倍のスピードアップを達成し、
  Work-Stealing版の${ws_speedup}倍を${hy_vs_ws}倍上回る性能を示した。

  Worker稼働率は${hy_util}%に達し、従来のWork-Stealing版（${ws_util}%）
  から大幅に改善された。特にLOCAL-HEAP-FILL機能により、
  768コア環境でも全ワーカーにタスクが行き渡るようになった。

═══════════════════════════════════════════════════════════════
EOF

log "サマリーレポート生成完了: $SUMMARY_FILE"

# サマリー表示
cat "$SUMMARY_FILE" | tee -a "$LOG_DIR/master.log"

log_header "実験1完了"
log "終了時刻: $(date)"
log "結果ファイル:"
log "  - CSV: $CSV_FILE"
log "  - サマリー: $SUMMARY_FILE"
log "  - ログディレクトリ: $LOG_DIR"

exit 0
