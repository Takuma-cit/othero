#!/bin/bash
################################################################################
# run_comprehensive_benchmark.sh
#
# WPNS TT-Parallel vs Hybrid Solver 包括的ベンチマーク
#
# 目的: 2つのソルバーを複数の観点から比較評価
#   - othello_endgame_solver_hybrid_check_tthit_fixed.c (Hybrid)
#   - wpns_tt_parallel.c (WPNS)
#
# 測定項目:
#   [1] 基本性能比較（解決時間、ノード数、NPS）
#   [2] 強スケーリング（スレッド数に対する性能）
#   [3] 空きマス別性能（問題サイズによる性能変化）
#   [4] FFO難問テスト（end40-end59）
#   [5] 結果一致率（両ソルバーの解の整合性）
#   [6] 難易度ばらつき分析（同一空きマスでのロバスト性評価）
#   [7] TTヒット率分析（置換表効率の比較）
#
# 実行環境:
#   --wsl    : WSL環境（スレッド1-4、短時間、少数局面）
#   --kemeko : 学内マシン（768コア、長時間、全局面）
#
# 使用方法:
#   ./run_comprehensive_benchmark.sh --wsl      # WSLでテスト
#   ./run_comprehensive_benchmark.sh --kemeko   # 学内マシンで本番実行
#   ./run_comprehensive_benchmark.sh --help     # ヘルプ表示
#
# 注意: 実行再開機能はありません（実験環境での対話操作不可のため）
################################################################################

set -e

# ========================================
# デフォルト設定
# ========================================
MODE="wsl"
TIME_LIMIT=60
TT_SIZE_MB=1024

# スレッド数リスト
THREAD_COUNTS_WSL="1 2 4"
THREAD_COUNTS_KEMEKO="1 2 4 8 16 32 64 128 256 384 512 768"

# 空きマス数リスト
EMPTIES_LIST_WSL="10 11 12"
EMPTIES_LIST_KEMEKO="10 12 14 16 18 20 22 24 26 28 30 32 34 36 38 40 42 44 46 48 50 52 54 56 58"

# 各空きマスのテストファイル数
FILES_PER_EMPTIES_WSL=3
FILES_PER_EMPTIES_KEMEKO=3

# ばらつき分析用ファイル数（ExpE）
VARIANCE_FILES_WSL=5
VARIANCE_FILES_KEMEKO=20

# 試行回数（スケーリング実験用）
TRIALS_WSL=1
TRIALS_KEMEKO=3

# FFOテスト
RUN_FFO=false

# ディレクトリ
POS_DIR="test_positions"
FFO_DIR="ffotest"
EVAL_FILE="eval/eval.dat"

# 出力ディレクトリ
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_DIR="benchmark_results_${TIMESTAMP}"

# ソルバーバイナリ
SOLVER_WPNS="./wpns_tt_parallel"
SOLVER_HYBRID="./othello_hybrid_solver"

# ========================================
# ヘルプ表示
# ========================================
show_help() {
    cat << 'EOF'
================================================================================
WPNS vs Hybrid 包括的ベンチマーク
================================================================================

使用方法:
  ./run_comprehensive_benchmark.sh [オプション]

環境設定:
  --wsl           WSL環境用設定（短時間テスト）
                  - スレッド: 1, 2, 4
                  - 空きマス: 10, 11, 12
                  - タイムアウト: 30秒
                  - 試行回数: 1回

  --kemeko        学内マシン用設定（本番実行）
                  - スレッド: 1, 2, 4, 8, 16, 32, 64, 128, 256, 384, 512, 768
                  - 空きマス: 10 12 14 16 18 20 22 24 26 28 30 32 34 36 38 40 42 44 46 48 50 52 54 56 58
                  - タイムアウト: 21600秒（6時間）
                  - TTサイズ: 1900GB
                  - 試行回数: 3回
                  - FFOテスト: 有効

カスタム設定:
  -t <秒>         タイムアウト時間（デフォルト: 60）
  -T <リスト>     スレッド数リスト（例: "1 2 4 8"）
  -e <リスト>     空きマス数リスト（例: "10 12 14"）
  -n <数>         各空きマスのテストファイル数
  -r <数>         試行回数（スケーリング用）
  -f              FFOテストも実行
  -m <MB>         TTサイズ（MB）

その他:
  -h, --help      このヘルプを表示

実行例:
  ./run_comprehensive_benchmark.sh --wsl                    # WSLでクイックテスト
  ./run_comprehensive_benchmark.sh --kemeko                 # 学内マシンでフル実行
  ./run_comprehensive_benchmark.sh -T "1 4 16" -e "12 14"   # カスタム設定

出力ファイル:
  benchmark_results_YYYYMMDD_HHMMSS/
    ├── 01_basic_comparison.csv      # 基本性能比較
    ├── 02_scaling_wpns.csv          # WPNSスケーリング
    ├── 02_scaling_hybrid.csv        # Hybridスケーリング
    ├── 03_empties_analysis.csv      # 空きマス別分析
    ├── 04_ffo_results.csv           # FFO難問結果
    ├── 05_agreement_check.csv       # 結果一致チェック
    ├── 06_variance_analysis.csv     # 難易度ばらつき分析
    ├── 06_robustness.csv            # ロバスト性統計
    ├── 07_tt_hit_rate.csv           # TTヒット率分析
    ├── summary.txt                  # 総合サマリー
    └── logs/                        # 詳細ログ

================================================================================
EOF
}

# ========================================
# 引数処理
# ========================================
while [[ $# -gt 0 ]]; do
    case $1 in
        --wsl)
            MODE="wsl"
            TIME_LIMIT=30
            TT_SIZE_MB=1024
            THREAD_COUNTS="$THREAD_COUNTS_WSL"
            EMPTIES_LIST="$EMPTIES_LIST_WSL"
            FILES_PER_EMPTIES=$FILES_PER_EMPTIES_WSL
            TRIALS=$TRIALS_WSL
            VARIANCE_FILES=$VARIANCE_FILES_WSL
            RUN_FFO=false
            shift
            ;;
        --kemeko)
            MODE="kemeko"
            TIME_LIMIT=21600
            TT_SIZE_MB=1900000
            THREAD_COUNTS="$THREAD_COUNTS_KEMEKO"
            EMPTIES_LIST="$EMPTIES_LIST_KEMEKO"
            FILES_PER_EMPTIES=$FILES_PER_EMPTIES_KEMEKO
            TRIALS=$TRIALS_KEMEKO
            VARIANCE_FILES=$VARIANCE_FILES_KEMEKO
            RUN_FFO=true
            shift
            ;;
        -t)
            TIME_LIMIT="$2"
            shift 2
            ;;
        -T)
            THREAD_COUNTS="$2"
            shift 2
            ;;
        -e)
            EMPTIES_LIST="$2"
            shift 2
            ;;
        -n)
            FILES_PER_EMPTIES="$2"
            shift 2
            ;;
        -r)
            TRIALS="$2"
            shift 2
            ;;
        -f)
            RUN_FFO=true
            shift
            ;;
        -m)
            TT_SIZE_MB="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "不明なオプション: $1"
            echo "./run_comprehensive_benchmark.sh --help でヘルプを表示"
            exit 1
            ;;
    esac
done

# デフォルト値の設定（引数で設定されなかった場合）
THREAD_COUNTS=${THREAD_COUNTS:-$THREAD_COUNTS_WSL}
EMPTIES_LIST=${EMPTIES_LIST:-$EMPTIES_LIST_WSL}
FILES_PER_EMPTIES=${FILES_PER_EMPTIES:-$FILES_PER_EMPTIES_WSL}
TRIALS=${TRIALS:-$TRIALS_WSL}
VARIANCE_FILES=${VARIANCE_FILES:-$VARIANCE_FILES_WSL}

# ========================================
# ユーティリティ関数
# ========================================
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

log_section() {
    echo "" | tee -a "$LOG_FILE"
    echo "########################################" | tee -a "$LOG_FILE"
    echo "# $*" | tee -a "$LOG_FILE"
    echo "########################################" | tee -a "$LOG_FILE"
}

# ========================================
# ソルバービルド
# ========================================
build_solvers() {
    log_section "ソルバービルド"

    # WPNS版
    log "WPNS TT-Parallel版をビルド中..."
    if [ -f "wpns_tt_parallel.c" ]; then
        gcc -O3 -march=native -pthread \
            -DTT_SIZE_MB=$TT_SIZE_MB \
            -DMAX_THREADS=1024 \
            -o "$SOLVER_WPNS" \
            wpns_tt_parallel.c \
            -lm -lpthread 2>&1 | tee -a "$LOG_FILE"
        if [ $? -eq 0 ]; then
            log "  [OK] $SOLVER_WPNS ビルド成功"
        else
            log "  [ERROR] $SOLVER_WPNS ビルド失敗"
            exit 1
        fi
    else
        log "  [ERROR] wpns_tt_parallel.c が見つかりません"
        exit 1
    fi

    # Hybrid版
    log "Hybrid版をビルド中..."
    if [ -f "othello_endgame_solver_hybrid_check_tthit_fixed.c" ]; then
        gcc -O3 -march=native -pthread \
            -DSTANDALONE_MAIN \
            -DMAX_THREADS=1024 \
            -o "$SOLVER_HYBRID" \
            othello_endgame_solver_hybrid_check_tthit_fixed.c \
            -lm -lpthread 2>&1 | tee -a "$LOG_FILE"
        if [ $? -eq 0 ]; then
            log "  [OK] $SOLVER_HYBRID ビルド成功"
        else
            log "  [WARN] $SOLVER_HYBRID ビルド失敗（Hybridテストをスキップ）"
            SOLVER_HYBRID=""
        fi
    else
        log "  [WARN] othello_endgame_solver_hybrid_check_tthit_fixed.c が見つかりません"
        SOLVER_HYBRID=""
    fi
}

# ========================================
# 結果パース関数
# ========================================
parse_wpns_result() {
    local output="$1"
    local result=$(echo "$output" | grep -E "^Result:" | awk '{print $2}')
    local total_line=$(echo "$output" | grep -E "^Total:")
    local time=$(echo "$total_line" | grep -oE '[0-9]+\.[0-9]+ sec' | grep -oE '[0-9]+\.[0-9]+')
    local nodes=$(echo "$total_line" | grep -oE '[0-9]+ nodes' | grep -oE '[0-9]+')
    local nps=$(echo "$total_line" | grep -oE '\([0-9]+ NPS\)' | grep -oE '[0-9]+')
    local tt_hits=$(echo "$output" | grep -E "^TT hits:" | grep -oE '[0-9]+' | head -1)

    result=${result:-UNKNOWN}
    time=${time:-0}
    nodes=${nodes:-0}
    nps=${nps:-0}
    tt_hits=${tt_hits:-0}

    echo "$result,$time,$nodes,$nps,$tt_hits"
}

parse_hybrid_result() {
    local output="$1"

    if echo "$output" | grep -q "SKIP"; then
        echo "SKIP,0,0,0,0"
        return
    fi

    local result=$(echo "$output" | grep -E "^Result:" | awk '{print $2}')
    local total_line=$(echo "$output" | grep -E "Total:.*nodes.*seconds")
    local time=$(echo "$total_line" | grep -oE '[0-9]+\.[0-9]+ seconds' | grep -oE '[0-9]+\.[0-9]+')
    local nodes=$(echo "$total_line" | grep -oE '[0-9]+ nodes' | grep -oE '[0-9]+')
    local nps=$(echo "$total_line" | grep -oE '\([0-9]+ NPS\)' | grep -oE '[0-9]+')
    local tt_hits=$(echo "$output" | grep -E "TT hit" | grep -oE '[0-9]+' | head -1)

    result=${result:-UNKNOWN}
    time=${time:-0}
    nodes=${nodes:-0}
    nps=${nps:-0}
    tt_hits=${tt_hits:-0}

    echo "$result,$time,$nodes,$nps,$tt_hits"
}

# ========================================
# [実験1] 基本性能比較
# ========================================
run_basic_comparison() {
    log_section "実験1: 基本性能比較"

    local csv_file="$OUTPUT_DIR/01_basic_comparison.csv"
    echo "Empties,FileID,Position,Threads,WPNS_Result,WPNS_Time,WPNS_Nodes,WPNS_NPS,WPNS_TTHits,Hybrid_Result,Hybrid_Time,Hybrid_Nodes,Hybrid_NPS,Hybrid_TTHits,Match" > "$csv_file"

    # 代表的なスレッド数で比較
    local test_threads=$(echo $THREAD_COUNTS | awk '{print $NF}')  # 最大スレッド数を使用

    for empties in $EMPTIES_LIST; do
        local empties_padded=$(printf "%02d" $empties)

        for file_id in $(seq 0 $((FILES_PER_EMPTIES - 1))); do
            local file_id_padded=$(printf "%03d" $file_id)
            local pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"

            if [ ! -f "$pos_file" ]; then
                continue
            fi

            log "テスト: $(basename $pos_file) (スレッド: $test_threads)"

            # WPNS実行
            local wpns_output=$(timeout $TIME_LIMIT "$SOLVER_WPNS" "$pos_file" $test_threads $TIME_LIMIT 2>&1)
            local wpns_exit=$?
            local wpns_data
            if [ $wpns_exit -eq 124 ]; then
                wpns_data="TIMEOUT,$TIME_LIMIT,0,0,0"
            else
                wpns_data=$(parse_wpns_result "$wpns_output")
            fi

            # Hybrid実行
            local hybrid_data="SKIP,0,0,0,0"
            if [ -n "$SOLVER_HYBRID" ] && [ -x "$SOLVER_HYBRID" ]; then
                local hybrid_output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$pos_file" $test_threads $TIME_LIMIT "$EVAL_FILE" -v 2>&1)
                local hybrid_exit=$?
                if [ $hybrid_exit -eq 124 ]; then
                    hybrid_data="TIMEOUT,$TIME_LIMIT,0,0,0"
                else
                    hybrid_data=$(parse_hybrid_result "$hybrid_output")
                fi
            fi

            # 結果一致チェック
            local wpns_result=$(echo "$wpns_data" | cut -d',' -f1)
            local hybrid_result=$(echo "$hybrid_data" | cut -d',' -f1)
            local match="N/A"
            if [ "$wpns_result" != "TIMEOUT" ] && [ "$hybrid_result" != "TIMEOUT" ] && [ "$hybrid_result" != "SKIP" ]; then
                if [ "$wpns_result" = "$hybrid_result" ]; then
                    match="YES"
                else
                    match="NO"
                fi
            fi

            echo "$empties,$file_id_padded,$pos_file,$test_threads,$wpns_data,$hybrid_data,$match" >> "$csv_file"

            # ログ出力
            log "  WPNS: $(echo $wpns_data | cut -d',' -f1) ($(echo $wpns_data | cut -d',' -f2)s)"
            log "  Hybrid: $(echo $hybrid_data | cut -d',' -f1) ($(echo $hybrid_data | cut -d',' -f2)s)"
            [ "$match" = "NO" ] && log "  [WARN] 結果不一致!"
        done
    done

    log "基本性能比較完了: $csv_file"
}

# ========================================
# [実験2] 強スケーリング
# ========================================
run_scaling_test() {
    log_section "実験2: 強スケーリング実験"

    local csv_wpns="$OUTPUT_DIR/02_scaling_wpns.csv"
    local csv_hybrid="$OUTPUT_DIR/02_scaling_hybrid.csv"

    echo "Threads,Trial,Time_Sec,Nodes,NPS,Speedup,Efficiency" > "$csv_wpns"
    echo "Threads,Trial,Time_Sec,Nodes,NPS,Speedup,Efficiency" > "$csv_hybrid"

    # テスト局面（固定）
    local test_pos="$POS_DIR/empties_12_id_000.pos"
    if [ ! -f "$test_pos" ]; then
        test_pos=$(ls $POS_DIR/empties_12_*.pos 2>/dev/null | head -1)
    fi

    if [ -z "$test_pos" ] || [ ! -f "$test_pos" ]; then
        log "[WARN] スケーリングテスト用局面が見つかりません。スキップします。"
        return
    fi

    log "テスト局面: $(basename $test_pos)"

    # ベースライン時間を保存
    local baseline_wpns=0
    local baseline_hybrid=0

    for threads in $THREAD_COUNTS; do
        log "スレッド数: $threads"

        for trial in $(seq 1 $TRIALS); do
            log "  試行 $trial/$TRIALS"

            # WPNS
            local wpns_output=$(timeout $TIME_LIMIT "$SOLVER_WPNS" "$test_pos" $threads $TIME_LIMIT 2>&1)
            local wpns_data=$(parse_wpns_result "$wpns_output")
            local wpns_time=$(echo "$wpns_data" | cut -d',' -f2)
            local wpns_nodes=$(echo "$wpns_data" | cut -d',' -f3)
            local wpns_nps=$(echo "$wpns_data" | cut -d',' -f4)

            # ベースライン設定
            if [ "$threads" = "1" ] && [ "$trial" = "1" ]; then
                baseline_wpns=$wpns_time
            fi

            # スピードアップ計算
            local speedup_wpns=1
            local efficiency_wpns=100
            if [ $(echo "$wpns_time > 0" | bc 2>/dev/null || echo "0") -eq 1 ] && \
               [ $(echo "$baseline_wpns > 0" | bc 2>/dev/null || echo "0") -eq 1 ]; then
                speedup_wpns=$(echo "scale=3; $baseline_wpns / $wpns_time" | bc 2>/dev/null || echo "1")
                efficiency_wpns=$(echo "scale=2; ($speedup_wpns / $threads) * 100" | bc 2>/dev/null || echo "100")
            fi

            echo "$threads,$trial,$wpns_time,$wpns_nodes,$wpns_nps,$speedup_wpns,$efficiency_wpns" >> "$csv_wpns"

            # Hybrid
            if [ -n "$SOLVER_HYBRID" ] && [ -x "$SOLVER_HYBRID" ]; then
                local hybrid_output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$test_pos" $threads $TIME_LIMIT "$EVAL_FILE" -v 2>&1)
                local hybrid_data=$(parse_hybrid_result "$hybrid_output")
                local hybrid_time=$(echo "$hybrid_data" | cut -d',' -f2)
                local hybrid_nodes=$(echo "$hybrid_data" | cut -d',' -f3)
                local hybrid_nps=$(echo "$hybrid_data" | cut -d',' -f4)

                if [ "$threads" = "1" ] && [ "$trial" = "1" ]; then
                    baseline_hybrid=$hybrid_time
                fi

                local speedup_hybrid=1
                local efficiency_hybrid=100
                if [ $(echo "$hybrid_time > 0" | bc 2>/dev/null || echo "0") -eq 1 ] && \
                   [ $(echo "$baseline_hybrid > 0" | bc 2>/dev/null || echo "0") -eq 1 ]; then
                    speedup_hybrid=$(echo "scale=3; $baseline_hybrid / $hybrid_time" | bc 2>/dev/null || echo "1")
                    efficiency_hybrid=$(echo "scale=2; ($speedup_hybrid / $threads) * 100" | bc 2>/dev/null || echo "100")
                fi

                echo "$threads,$trial,$hybrid_time,$hybrid_nodes,$hybrid_nps,$speedup_hybrid,$efficiency_hybrid" >> "$csv_hybrid"
            fi
        done
    done

    log "スケーリング実験完了"
}

# ========================================
# [実験3] 空きマス別分析
# ========================================
run_empties_analysis() {
    log_section "実験3: 空きマス別性能分析"

    local csv_file="$OUTPUT_DIR/03_empties_analysis.csv"
    echo "Empties,Solver,Avg_Time,Avg_Nodes,Avg_NPS,Solved,Total,Solve_Rate" > "$csv_file"

    local test_threads=$(echo $THREAD_COUNTS | awk '{print $NF}')

    for empties in $EMPTIES_LIST; do
        local empties_padded=$(printf "%02d" $empties)
        log "空きマス: $empties"

        # WPNS統計
        local wpns_total_time=0
        local wpns_total_nodes=0
        local wpns_total_nps=0
        local wpns_solved=0
        local wpns_count=0

        # Hybrid統計
        local hybrid_total_time=0
        local hybrid_total_nodes=0
        local hybrid_total_nps=0
        local hybrid_solved=0
        local hybrid_count=0

        for file_id in $(seq 0 $((FILES_PER_EMPTIES - 1))); do
            local file_id_padded=$(printf "%03d" $file_id)
            local pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"

            if [ ! -f "$pos_file" ]; then
                continue
            fi

            # WPNS
            local wpns_output=$(timeout $TIME_LIMIT "$SOLVER_WPNS" "$pos_file" $test_threads $TIME_LIMIT 2>&1)
            local wpns_data=$(parse_wpns_result "$wpns_output")
            local wpns_result=$(echo "$wpns_data" | cut -d',' -f1)
            local wpns_time=$(echo "$wpns_data" | cut -d',' -f2)
            local wpns_nodes=$(echo "$wpns_data" | cut -d',' -f3)
            local wpns_nps=$(echo "$wpns_data" | cut -d',' -f4)

            wpns_count=$((wpns_count + 1))
            if [ "$wpns_result" = "WIN" ] || [ "$wpns_result" = "LOSE" ] || [ "$wpns_result" = "DRAW" ]; then
                wpns_solved=$((wpns_solved + 1))
                wpns_total_time=$(echo "$wpns_total_time + $wpns_time" | bc 2>/dev/null || echo "$wpns_total_time")
                wpns_total_nodes=$(echo "$wpns_total_nodes + $wpns_nodes" | bc 2>/dev/null || echo "$wpns_total_nodes")
                wpns_total_nps=$(echo "$wpns_total_nps + $wpns_nps" | bc 2>/dev/null || echo "$wpns_total_nps")
            fi

            # Hybrid
            if [ -n "$SOLVER_HYBRID" ] && [ -x "$SOLVER_HYBRID" ]; then
                local hybrid_output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$pos_file" $test_threads $TIME_LIMIT "$EVAL_FILE" -v 2>&1)
                local hybrid_data=$(parse_hybrid_result "$hybrid_output")
                local hybrid_result=$(echo "$hybrid_data" | cut -d',' -f1)
                local hybrid_time=$(echo "$hybrid_data" | cut -d',' -f2)
                local hybrid_nodes=$(echo "$hybrid_data" | cut -d',' -f3)
                local hybrid_nps=$(echo "$hybrid_data" | cut -d',' -f4)

                hybrid_count=$((hybrid_count + 1))
                if [ "$hybrid_result" = "WIN" ] || [ "$hybrid_result" = "LOSE" ] || [ "$hybrid_result" = "DRAW" ]; then
                    hybrid_solved=$((hybrid_solved + 1))
                    hybrid_total_time=$(echo "$hybrid_total_time + $hybrid_time" | bc 2>/dev/null || echo "$hybrid_total_time")
                    hybrid_total_nodes=$(echo "$hybrid_total_nodes + $hybrid_nodes" | bc 2>/dev/null || echo "$hybrid_total_nodes")
                    hybrid_total_nps=$(echo "$hybrid_total_nps + $hybrid_nps" | bc 2>/dev/null || echo "$hybrid_total_nps")
                fi
            fi
        done

        # WPNS平均
        if [ $wpns_solved -gt 0 ]; then
            local wpns_avg_time=$(echo "scale=3; $wpns_total_time / $wpns_solved" | bc 2>/dev/null || echo "0")
            local wpns_avg_nodes=$(echo "scale=0; $wpns_total_nodes / $wpns_solved" | bc 2>/dev/null || echo "0")
            local wpns_avg_nps=$(echo "scale=0; $wpns_total_nps / $wpns_solved" | bc 2>/dev/null || echo "0")
            local wpns_solve_rate=$(echo "scale=1; $wpns_solved * 100 / $wpns_count" | bc 2>/dev/null || echo "0")
            echo "$empties,WPNS,$wpns_avg_time,$wpns_avg_nodes,$wpns_avg_nps,$wpns_solved,$wpns_count,$wpns_solve_rate" >> "$csv_file"
        else
            echo "$empties,WPNS,0,0,0,0,$wpns_count,0" >> "$csv_file"
        fi

        # Hybrid平均
        if [ $hybrid_solved -gt 0 ]; then
            local hybrid_avg_time=$(echo "scale=3; $hybrid_total_time / $hybrid_solved" | bc 2>/dev/null || echo "0")
            local hybrid_avg_nodes=$(echo "scale=0; $hybrid_total_nodes / $hybrid_solved" | bc 2>/dev/null || echo "0")
            local hybrid_avg_nps=$(echo "scale=0; $hybrid_total_nps / $hybrid_solved" | bc 2>/dev/null || echo "0")
            local hybrid_solve_rate=$(echo "scale=1; $hybrid_solved * 100 / $hybrid_count" | bc 2>/dev/null || echo "0")
            echo "$empties,Hybrid,$hybrid_avg_time,$hybrid_avg_nodes,$hybrid_avg_nps,$hybrid_solved,$hybrid_count,$hybrid_solve_rate" >> "$csv_file"
        elif [ $hybrid_count -gt 0 ]; then
            echo "$empties,Hybrid,0,0,0,0,$hybrid_count,0" >> "$csv_file"
        fi
    done

    log "空きマス別分析完了: $csv_file"
}

# ========================================
# [実験4] FFO難問テスト
# ========================================
run_ffo_test() {
    if [ "$RUN_FFO" != "true" ]; then
        log "FFOテストはスキップされました（-f オプションで有効化）"
        return
    fi

    log_section "実験4: FFO難問テスト (end40-end59)"

    local csv_file="$OUTPUT_DIR/04_ffo_results.csv"
    echo "FFO_ID,Empties,WPNS_Result,WPNS_Time,WPNS_Nodes,Hybrid_Result,Hybrid_Time,Hybrid_Nodes,Match" > "$csv_file"

    local test_threads=$(echo $THREAD_COUNTS | awk '{print $NF}')

    for ffo_file in $FFO_DIR/end*.pos; do
        if [ ! -f "$ffo_file" ]; then
            continue
        fi

        local ffo_id=$(basename "$ffo_file" .pos | sed 's/end//')
        local empties=$(head -1 "$ffo_file" | tr -cd '.-' | wc -c)

        log "FFO #$ffo_id (空きマス: $empties)"

        # WPNS
        local wpns_output=$(timeout $TIME_LIMIT "$SOLVER_WPNS" "$ffo_file" $test_threads $TIME_LIMIT 2>&1)
        local wpns_data=$(parse_wpns_result "$wpns_output")
        local wpns_result=$(echo "$wpns_data" | cut -d',' -f1)
        local wpns_time=$(echo "$wpns_data" | cut -d',' -f2)
        local wpns_nodes=$(echo "$wpns_data" | cut -d',' -f3)

        # Hybrid
        local hybrid_result="SKIP"
        local hybrid_time="0"
        local hybrid_nodes="0"
        if [ -n "$SOLVER_HYBRID" ] && [ -x "$SOLVER_HYBRID" ]; then
            local hybrid_output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$ffo_file" $test_threads $TIME_LIMIT "$EVAL_FILE" -v 2>&1)
            local hybrid_data=$(parse_hybrid_result "$hybrid_output")
            hybrid_result=$(echo "$hybrid_data" | cut -d',' -f1)
            hybrid_time=$(echo "$hybrid_data" | cut -d',' -f2)
            hybrid_nodes=$(echo "$hybrid_data" | cut -d',' -f3)
        fi

        # 一致チェック
        local match="N/A"
        if [ "$wpns_result" != "TIMEOUT" ] && [ "$hybrid_result" != "TIMEOUT" ] && [ "$hybrid_result" != "SKIP" ]; then
            if [ "$wpns_result" = "$hybrid_result" ]; then
                match="YES"
            else
                match="NO"
            fi
        fi

        echo "$ffo_id,$empties,$wpns_result,$wpns_time,$wpns_nodes,$hybrid_result,$hybrid_time,$hybrid_nodes,$match" >> "$csv_file"
        log "  WPNS: $wpns_result (${wpns_time}s), Hybrid: $hybrid_result (${hybrid_time}s)"
    done

    log "FFOテスト完了: $csv_file"
}

# ========================================
# [実験5] 結果一致チェック
# ========================================
run_agreement_check() {
    log_section "実験5: 結果一致チェック"

    local csv_file="$OUTPUT_DIR/05_agreement_check.csv"
    echo "Category,Total,Matched,Mismatched,Match_Rate" > "$csv_file"

    # 基本比較の一致率
    local basic_csv="$OUTPUT_DIR/01_basic_comparison.csv"
    if [ -f "$basic_csv" ]; then
        local basic_total=$(tail -n +2 "$basic_csv" | grep -v "TIMEOUT\|SKIP\|N/A" | wc -l)
        local basic_matched=$(tail -n +2 "$basic_csv" | grep ",YES$" | wc -l)
        local basic_mismatched=$((basic_total - basic_matched))
        local basic_rate=0
        if [ $basic_total -gt 0 ]; then
            basic_rate=$(echo "scale=1; $basic_matched * 100 / $basic_total" | bc 2>/dev/null || echo "0")
        fi
        echo "Basic_Comparison,$basic_total,$basic_matched,$basic_mismatched,$basic_rate" >> "$csv_file"
    fi

    # FFOの一致率
    local ffo_csv="$OUTPUT_DIR/04_ffo_results.csv"
    if [ -f "$ffo_csv" ]; then
        local ffo_total=$(tail -n +2 "$ffo_csv" | grep -v "TIMEOUT\|SKIP\|N/A" | wc -l)
        local ffo_matched=$(tail -n +2 "$ffo_csv" | grep ",YES$" | wc -l)
        local ffo_mismatched=$((ffo_total - ffo_matched))
        local ffo_rate=0
        if [ $ffo_total -gt 0 ]; then
            ffo_rate=$(echo "scale=1; $ffo_matched * 100 / $ffo_total" | bc 2>/dev/null || echo "0")
        fi
        echo "FFO_Test,$ffo_total,$ffo_matched,$ffo_mismatched,$ffo_rate" >> "$csv_file"
    fi

    log "結果一致チェック完了: $csv_file"
}

# ========================================
# [実験6] 難易度ばらつき分析 (ExpE)
# ========================================
run_variance_analysis() {
    log_section "実験6: 難易度ばらつき分析"

    local csv_variance="$OUTPUT_DIR/06_variance_analysis.csv"
    local csv_robustness="$OUTPUT_DIR/06_robustness.csv"

    echo "Empties,FileID,Solver,Time_Sec,Nodes,NPS,Result" > "$csv_variance"
    echo "Solver,Empties,Count,Avg_Time,StdDev_Time,CV,Min_Time,Max_Time,MaxMin_Ratio" > "$csv_robustness"

    local test_threads=$(echo $THREAD_COUNTS | awk '{print $NF}')

    # テスト対象の空きマス（代表的なもの）
    local variance_empties="12"
    if [ "$MODE" = "kemeko" ]; then
        variance_empties="12 14 16"
    fi

    for empties in $variance_empties; do
        local empties_padded=$(printf "%02d" $empties)
        log "空きマス: $empties (${VARIANCE_FILES}問で統計)"

        # 各ソルバーの結果を収集
        for solver in "WPNS" "Hybrid"; do
            local times_file=$(mktemp)

            for file_id in $(seq 0 $((VARIANCE_FILES - 1))); do
                local file_id_padded=$(printf "%03d" $file_id)
                local pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"

                if [ ! -f "$pos_file" ]; then
                    continue
                fi

                local output result time_sec nodes nps
                if [ "$solver" = "WPNS" ]; then
                    output=$(timeout $TIME_LIMIT "$SOLVER_WPNS" "$pos_file" $test_threads $TIME_LIMIT 2>&1)
                    local data=$(parse_wpns_result "$output")
                    result=$(echo "$data" | cut -d',' -f1)
                    time_sec=$(echo "$data" | cut -d',' -f2)
                    nodes=$(echo "$data" | cut -d',' -f3)
                    nps=$(echo "$data" | cut -d',' -f4)
                elif [ -n "$SOLVER_HYBRID" ] && [ -x "$SOLVER_HYBRID" ]; then
                    output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$pos_file" $test_threads $TIME_LIMIT "$EVAL_FILE" -v 2>&1)
                    local data=$(parse_hybrid_result "$output")
                    result=$(echo "$data" | cut -d',' -f1)
                    time_sec=$(echo "$data" | cut -d',' -f2)
                    nodes=$(echo "$data" | cut -d',' -f3)
                    nps=$(echo "$data" | cut -d',' -f4)
                else
                    continue
                fi

                echo "$empties,$file_id_padded,$solver,$time_sec,$nodes,$nps,$result" >> "$csv_variance"

                # 解決した場合のみ統計に含める
                if [ "$result" = "WIN" ] || [ "$result" = "LOSE" ] || [ "$result" = "DRAW" ]; then
                    echo "$time_sec" >> "$times_file"
                fi
            done

            # 統計計算
            if [ -s "$times_file" ]; then
                local stats=$(awk '
                    BEGIN { sum=0; sum_sq=0; min=999999; max=0; count=0 }
                    {
                        sum += $1
                        sum_sq += $1 * $1
                        if ($1 < min) min = $1
                        if ($1 > max) max = $1
                        count++
                    }
                    END {
                        if (count > 0) {
                            avg = sum / count
                            variance = (count > 1) ? (sum_sq - sum*sum/count) / (count-1) : 0
                            stddev = sqrt(variance > 0 ? variance : 0)
                            cv = (avg > 0) ? stddev / avg : 0
                            ratio = (min > 0) ? max / min : 0
                            printf "%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f", count, avg, stddev, cv, min, max, ratio
                        } else {
                            print "0,0,0,0,0,0,0"
                        }
                    }
                ' "$times_file")

                echo "$solver,$empties,$stats" >> "$csv_robustness"
                log "  $solver: $(echo $stats | cut -d',' -f1)問, CV=$(echo $stats | cut -d',' -f4)"
            fi

            rm -f "$times_file"
        done
    done

    log "難易度ばらつき分析完了: $csv_variance"
}

# ========================================
# [実験7] TTヒット率分析 (ExpF)
# ========================================
run_tt_hit_rate_analysis() {
    log_section "実験7: TTヒット率分析"

    local csv_file="$OUTPUT_DIR/07_tt_hit_rate.csv"
    echo "Empties,Solver,FileID,Nodes,Time_Sec,TT_Hits,Hits_Per_Node" > "$csv_file"

    local test_threads=$(echo $THREAD_COUNTS | awk '{print $NF}')

    # テスト対象（代表的な空きマス）
    local tt_empties="12"
    if [ "$MODE" = "kemeko" ]; then
        tt_empties="10 12 14 16"
    fi

    local tt_files=3
    if [ "$MODE" = "kemeko" ]; then
        tt_files=5
    fi

    for empties in $tt_empties; do
        local empties_padded=$(printf "%02d" $empties)
        log "空きマス: $empties - TTヒット測定"

        for file_id in $(seq 0 $((tt_files - 1))); do
            local file_id_padded=$(printf "%03d" $file_id)
            local pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"

            if [ ! -f "$pos_file" ]; then
                continue
            fi

            # WPNS
            local wpns_output=$(timeout $TIME_LIMIT "$SOLVER_WPNS" "$pos_file" $test_threads $TIME_LIMIT 2>&1)
            local wpns_data=$(parse_wpns_result "$wpns_output")
            local wpns_nodes=$(echo "$wpns_data" | cut -d',' -f3)
            local wpns_time=$(echo "$wpns_data" | cut -d',' -f2)
            local wpns_tt_hits=$(echo "$wpns_data" | cut -d',' -f5)

            # Hits per Node（TTの再利用効率）
            local wpns_hits_per_node=0
            if [ "$wpns_nodes" -gt 0 ] 2>/dev/null; then
                wpns_hits_per_node=$(echo "scale=2; $wpns_tt_hits / $wpns_nodes" | bc 2>/dev/null || echo "0")
            fi

            echo "$empties,WPNS,$file_id_padded,$wpns_nodes,$wpns_time,$wpns_tt_hits,$wpns_hits_per_node" >> "$csv_file"

            # Hybrid
            if [ -n "$SOLVER_HYBRID" ] && [ -x "$SOLVER_HYBRID" ]; then
                local hybrid_output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$pos_file" $test_threads $TIME_LIMIT "$EVAL_FILE" -v 2>&1)
                local hybrid_data=$(parse_hybrid_result "$hybrid_output")
                local hybrid_nodes=$(echo "$hybrid_data" | cut -d',' -f3)
                local hybrid_time=$(echo "$hybrid_data" | cut -d',' -f2)
                local hybrid_tt_hits=$(echo "$hybrid_data" | cut -d',' -f5)

                local hybrid_hits_per_node=0
                if [ "$hybrid_nodes" -gt 0 ] 2>/dev/null; then
                    hybrid_hits_per_node=$(echo "scale=2; $hybrid_tt_hits / $hybrid_nodes" | bc 2>/dev/null || echo "0")
                fi

                echo "$empties,Hybrid,$file_id_padded,$hybrid_nodes,$hybrid_time,$hybrid_tt_hits,$hybrid_hits_per_node" >> "$csv_file"
            fi
        done
    done

    log "TTヒット率分析完了: $csv_file"
}

# ========================================
# サマリーレポート生成
# ========================================
generate_summary() {
    log_section "サマリーレポート生成"

    local summary_file="$OUTPUT_DIR/summary.txt"

    cat > "$summary_file" << EOF
================================================================================
WPNS vs Hybrid 包括的ベンチマーク結果
================================================================================
実行日時: $(date)
実行モード: $MODE
タイムアウト: ${TIME_LIMIT}秒
スレッド数: $THREAD_COUNTS
空きマス数: $EMPTIES_LIST
試行回数: $TRIALS
FFOテスト: $RUN_FFO

--------------------------------------------------------------------------------
[1] 基本性能比較
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/01_basic_comparison.csv" ]; then
        local wpns_avg=$(awk -F',' 'NR>1 && $5!="TIMEOUT" {sum+=$6; count++} END {if(count>0) printf "%.3f", sum/count; else print "N/A"}' "$OUTPUT_DIR/01_basic_comparison.csv")
        local hybrid_avg=$(awk -F',' 'NR>1 && $10!="TIMEOUT" && $10!="SKIP" {sum+=$11; count++} END {if(count>0) printf "%.3f", sum/count; else print "N/A"}' "$OUTPUT_DIR/01_basic_comparison.csv")

        cat >> "$summary_file" << EOF
WPNS平均時間:   ${wpns_avg}秒
Hybrid平均時間: ${hybrid_avg}秒

EOF
    fi

    cat >> "$summary_file" << EOF
--------------------------------------------------------------------------------
[2] 強スケーリング
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/02_scaling_wpns.csv" ]; then
        echo "WPNS スケーリング結果:" >> "$summary_file"
        awk -F',' 'NR>1 {printf "  %3dスレッド: %.3fs (Speedup: %.2fx, 効率: %.1f%%)\n", $1, $3, $6, $7}' "$OUTPUT_DIR/02_scaling_wpns.csv" >> "$summary_file"
        echo "" >> "$summary_file"
    fi

    if [ -f "$OUTPUT_DIR/02_scaling_hybrid.csv" ]; then
        echo "Hybrid スケーリング結果:" >> "$summary_file"
        awk -F',' 'NR>1 {printf "  %3dスレッド: %.3fs (Speedup: %.2fx, 効率: %.1f%%)\n", $1, $3, $6, $7}' "$OUTPUT_DIR/02_scaling_hybrid.csv" >> "$summary_file"
        echo "" >> "$summary_file"
    fi

    cat >> "$summary_file" << EOF
--------------------------------------------------------------------------------
[3] 空きマス別性能
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/03_empties_analysis.csv" ]; then
        echo "空きマス | ソルバー | 平均時間 | 平均ノード | 解決率" >> "$summary_file"
        echo "---------|----------|----------|------------|--------" >> "$summary_file"
        awk -F',' 'NR>1 {printf "%8s | %-8s | %8.3fs | %10d | %5.1f%%\n", $1, $2, $3, $4, $8}' "$OUTPUT_DIR/03_empties_analysis.csv" >> "$summary_file"
        echo "" >> "$summary_file"
    fi

    cat >> "$summary_file" << EOF
--------------------------------------------------------------------------------
[4] FFO難問テスト結果
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/04_ffo_results.csv" ]; then
        local ffo_solved=$(awk -F',' 'NR>1 && ($3=="WIN" || $3=="LOSE") {count++} END {print count+0}' "$OUTPUT_DIR/04_ffo_results.csv")
        local ffo_total=$(tail -n +2 "$OUTPUT_DIR/04_ffo_results.csv" | wc -l)
        echo "WPNS解決: $ffo_solved / $ffo_total" >> "$summary_file"
        echo "" >> "$summary_file"
    else
        echo "FFOテストは実行されませんでした" >> "$summary_file"
    fi

    cat >> "$summary_file" << EOF
--------------------------------------------------------------------------------
[5] 結果一致率
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/05_agreement_check.csv" ]; then
        awk -F',' 'NR>1 {printf "%s: %d/%d (%.1f%%)\n", $1, $3, $2, $5}' "$OUTPUT_DIR/05_agreement_check.csv" >> "$summary_file"
    fi

    cat >> "$summary_file" << EOF

--------------------------------------------------------------------------------
[6] 難易度ばらつき分析 (ロバスト性)
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/06_robustness.csv" ]; then
        echo "変動係数（CV）: 小さいほど安定した性能" >> "$summary_file"
        echo "" >> "$summary_file"
        printf "%-10s %-10s %-10s %-12s %-12s %-10s\n" "Solver" "Empties" "Count" "Avg_Time" "CV" "Max/Min" >> "$summary_file"
        echo "----------------------------------------------------------------------" >> "$summary_file"
        awk -F',' 'NR>1 {printf "%-10s %-10s %-10s %-12.4fs %-12.4f %-10.2fx\n", $1, $2, $3, $4, $6, $9}' "$OUTPUT_DIR/06_robustness.csv" >> "$summary_file"
        echo "" >> "$summary_file"

        # 平均CVを計算
        local wpns_cv=$(awk -F',' '$1=="WPNS" {sum+=$6; count++} END {if(count>0) printf "%.4f", sum/count; else print "N/A"}' "$OUTPUT_DIR/06_robustness.csv")
        local hybrid_cv=$(awk -F',' '$1=="Hybrid" {sum+=$6; count++} END {if(count>0) printf "%.4f", sum/count; else print "N/A"}' "$OUTPUT_DIR/06_robustness.csv")
        echo "平均CV: WPNS=$wpns_cv, Hybrid=$hybrid_cv" >> "$summary_file"
        echo "(CVが小さいほど問題難易度に対するロバスト性が高い)" >> "$summary_file"
    else
        echo "難易度ばらつき分析は実行されませんでした" >> "$summary_file"
    fi

    cat >> "$summary_file" << EOF

--------------------------------------------------------------------------------
[7] TTヒット率分析
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/07_tt_hit_rate.csv" ]; then
        echo "空きマス別 TT統計（Hits/Node = TTの再利用効率）:" >> "$summary_file"
        echo "" >> "$summary_file"
        printf "%-10s %-15s %-15s %-15s %-15s\n" "Empties" "WPNS_Hits/N" "WPNS_TT_Hits" "Hybrid_Hits/N" "Hybrid_TT_Hits" >> "$summary_file"
        echo "------------------------------------------------------------------------" >> "$summary_file"

        # 空きマス別にTT統計を集計
        for empties in $(awk -F',' 'NR>1 {print $1}' "$OUTPUT_DIR/07_tt_hit_rate.csv" | sort -n | uniq); do
            local wpns_hpn=$(awk -F',' -v e="$empties" '$1==e && $2=="WPNS" {sum+=$7; count++} END {if(count>0) printf "%.2f", sum/count; else print "N/A"}' "$OUTPUT_DIR/07_tt_hit_rate.csv")
            local wpns_hits=$(awk -F',' -v e="$empties" '$1==e && $2=="WPNS" {sum+=$6; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}' "$OUTPUT_DIR/07_tt_hit_rate.csv")
            local hybrid_hpn=$(awk -F',' -v e="$empties" '$1==e && $2=="Hybrid" {sum+=$7; count++} END {if(count>0) printf "%.2f", sum/count; else print "N/A"}' "$OUTPUT_DIR/07_tt_hit_rate.csv")
            local hybrid_hits=$(awk -F',' -v e="$empties" '$1==e && $2=="Hybrid" {sum+=$6; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}' "$OUTPUT_DIR/07_tt_hit_rate.csv")
            printf "%-10s %-15s %-15s %-15s %-15s\n" "$empties" "$wpns_hpn" "$wpns_hits" "$hybrid_hpn" "$hybrid_hits" >> "$summary_file"
        done
        echo "" >> "$summary_file"
        echo "※ Hits/Node > 1.0: 各ノードで平均1回以上TTヒット（高い再利用効率）" >> "$summary_file"
    else
        echo "TTヒット率分析は実行されませんでした" >> "$summary_file"
    fi

    cat >> "$summary_file" << EOF

================================================================================
出力ファイル一覧
================================================================================
$(ls -la $OUTPUT_DIR/*.csv 2>/dev/null)

================================================================================
EOF

    cat "$summary_file" | tee -a "$LOG_FILE"
    log "サマリーレポート: $summary_file"
}

# ========================================
# メイン処理
# ========================================
main() {
    echo "========================================"
    echo "WPNS vs Hybrid 包括的ベンチマーク"
    echo "========================================"
    echo ""
    echo "設定:"
    echo "  モード: $MODE"
    echo "  タイムアウト: ${TIME_LIMIT}秒"
    echo "  スレッド数: $THREAD_COUNTS"
    echo "  空きマス数: $EMPTIES_LIST"
    echo "  試行回数: $TRIALS"
    echo "  FFOテスト: $RUN_FFO"
    echo "  TTサイズ: ${TT_SIZE_MB}MB"
    echo ""

    # 出力ディレクトリ作成
    mkdir -p "$OUTPUT_DIR/logs"
    LOG_FILE="$OUTPUT_DIR/logs/benchmark.log"
    echo "出力ディレクトリ: $OUTPUT_DIR"
    echo ""

    # ソルバービルド
    build_solvers

    # 各実験実行
    run_basic_comparison
    run_scaling_test
    run_empties_analysis
    run_ffo_test
    run_agreement_check
    run_variance_analysis
    run_tt_hit_rate_analysis

    # サマリー生成
    generate_summary

    echo ""
    echo "========================================"
    echo "ベンチマーク完了!"
    echo "========================================"
    echo "結果ディレクトリ: $OUTPUT_DIR"
    echo ""
}

# 実行
main "$@"
