#!/bin/bash
################################################################################
# run_test.sh
#
# Hybrid No-Eval vs WPNS TT-Parallel ベンチマーク
#
# 比較対象:
#   - othello_endgame_solver_hybrid_no_eval.c (Hybrid No-Eval)
#   - wpns_tt_parallel.c (WPNS)
#
# 測定項目:
#   [1] 基本性能比較（解決時間、ノード数、NPS）
#   [2] 強スケーリング（スレッド数に対する性能）
#   [3] 空きマス別性能（問題サイズによる性能変化）
#   [4] 結果一致チェック（両ソルバーの解の整合性）
#
# 実行環境:
#   --wsl    : WSL環境（スレッド1-4、短時間、少数局面）
#   --kemeko : 学内マシン（768コア、長時間、全局面）
#
# 使用方法:
#   ./run_test.sh --wsl      # WSLでテスト
#   ./run_test.sh --kemeko   # 学内マシンで本番実行
#   ./run_test.sh --help     # ヘルプ表示
#
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
EMPTIES_LIST_WSL="10 12 14"
EMPTIES_LIST_KEMEKO="10 12 14 16 18 20 22 24 26 28 30 32 34 36 38 40 42 44 46 48 50 52 54 56 58"

# 各空きマスのテストファイル数
FILES_PER_EMPTIES_WSL=3
FILES_PER_EMPTIES_KEMEKO=5

# 試行回数（スケーリング実験用）
TRIALS_WSL=1
TRIALS_KEMEKO=3

# スケーリングテスト用の空きマス数
SCALING_EMPTIES_WSL=12
SCALING_EMPTIES_KEMEKO=16

# ディレクトリ
POS_DIR="test_positions"

# 出力ディレクトリ
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_DIR="benchmark_results_${TIMESTAMP}"

# ソルバーバイナリ
SOLVER_WPNS="./wpns_tt_parallel"
SOLVER_HYBRID="./othello_hybrid_no_eval"

# ソースファイル
SRC_WPNS="wpns_tt_parallel.c"
SRC_HYBRID="othello_endgame_solver_hybrid_no_eval.c"

# ========================================
# ヘルプ表示
# ========================================
show_help() {
    cat << 'EOF'
================================================================================
Hybrid No-Eval vs WPNS TT-Parallel ベンチマーク
================================================================================

使用方法:
  ./run_test.sh [オプション]

環境設定:
  --wsl           WSL環境用設定（短時間テスト）
                  - スレッド: 1, 2, 4
                  - 空きマス: 10, 12, 14
                  - タイムアウト: 60秒
                  - 試行回数: 1回

  --kemeko        学内マシン用設定（本番実行）
                  - スレッド: 1, 2, 4, 8, 16, 32, 64, 128, 256, 384, 512, 768
                  - 空きマス: 10〜58（偶数）
                  - タイムアウト: 21600秒（6時間）
                  - TTサイズ: 2100GB (2.1TB)
                  - 試行回数: 3回

カスタム設定:
  -t <秒>         タイムアウト時間（デフォルト: 60）
  -T <リスト>     スレッド数リスト（例: "1 2 4 8"）
  -e <リスト>     空きマス数リスト（例: "10 12 14"）
  -n <数>         各空きマスのテストファイル数
  -r <数>         試行回数（スケーリング用）
  -m <MB>         TTサイズ（MB）
  -s <数>         スケーリングテスト用空きマス数

その他:
  -h, --help      このヘルプを表示

実行例:
  ./run_test.sh --wsl                    # WSLでクイックテスト
  ./run_test.sh --kemeko                 # 学内マシンでフル実行
  ./run_test.sh -T "1 4 16" -e "12 14"   # カスタム設定

出力ファイル:
  benchmark_results_YYYYMMDD_HHMMSS/
    ├── 01_basic_comparison.csv      # 基本性能比較
    ├── 02_scaling_wpns.csv          # WPNSスケーリング
    ├── 02_scaling_hybrid.csv        # Hybridスケーリング
    ├── 03_empties_analysis.csv      # 空きマス別分析
    ├── 04_agreement_check.csv       # 結果一致チェック
    ├── 05_resource_usage.csv        # メモリ・CPU使用量
    ├── 05_cpu_scaling.csv           # スレッド別CPU効率
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
            TIME_LIMIT=60
            TT_SIZE_MB=1024
            THREAD_COUNTS="$THREAD_COUNTS_WSL"
            EMPTIES_LIST="$EMPTIES_LIST_WSL"
            FILES_PER_EMPTIES=$FILES_PER_EMPTIES_WSL
            TRIALS=$TRIALS_WSL
            SCALING_EMPTIES=$SCALING_EMPTIES_WSL
            shift
            ;;
        --kemeko)
            MODE="kemeko"
            TIME_LIMIT=21600
            TT_SIZE_MB=2100000
            THREAD_COUNTS="$THREAD_COUNTS_KEMEKO"
            EMPTIES_LIST="$EMPTIES_LIST_KEMEKO"
            FILES_PER_EMPTIES=$FILES_PER_EMPTIES_KEMEKO
            TRIALS=$TRIALS_KEMEKO
            SCALING_EMPTIES=$SCALING_EMPTIES_KEMEKO
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
        -m)
            TT_SIZE_MB="$2"
            shift 2
            ;;
        -s)
            SCALING_EMPTIES="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "不明なオプション: $1"
            echo "./run_test.sh --help でヘルプを表示"
            exit 1
            ;;
    esac
done

# デフォルト値の設定（引数で設定されなかった場合）
THREAD_COUNTS=${THREAD_COUNTS:-$THREAD_COUNTS_WSL}
EMPTIES_LIST=${EMPTIES_LIST:-$EMPTIES_LIST_WSL}
FILES_PER_EMPTIES=${FILES_PER_EMPTIES:-$FILES_PER_EMPTIES_WSL}
TRIALS=${TRIALS:-$TRIALS_WSL}
SCALING_EMPTIES=${SCALING_EMPTIES:-$SCALING_EMPTIES_WSL}

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

    # コンパイルオプション設定（モード別）
    local COMMON_FLAGS="-O3 -pthread -DMAX_THREADS=1024"
    local ARCH_FLAGS="-march=native"

    if [ "$MODE" = "kemeko" ]; then
        # AMD EPYC 9965 (Zen4) 向け最適化
        log "kemeko環境向け最適化オプションを使用"
        ARCH_FLAGS="-march=znver4"
        COMMON_FLAGS="$COMMON_FLAGS -mavx512f -mavx512bw -mavx512vl -mavx512dq"
        COMMON_FLAGS="$COMMON_FLAGS -mbmi -mbmi2"
    fi

    # WPNS版
    log "WPNS TT-Parallel版をビルド中..."
    if [ -f "$SRC_WPNS" ]; then
        gcc $COMMON_FLAGS $ARCH_FLAGS \
            -DTT_SIZE_MB=$TT_SIZE_MB \
            -o "$SOLVER_WPNS" \
            "$SRC_WPNS" \
            -lm -lpthread 2>&1 | tee -a "$LOG_FILE"
        if [ $? -eq 0 ]; then
            log "  [OK] $SOLVER_WPNS ビルド成功"
        else
            log "  [ERROR] $SOLVER_WPNS ビルド失敗"
            exit 1
        fi
    else
        log "  [ERROR] $SRC_WPNS が見つかりません"
        exit 1
    fi

    # Hybrid No-Eval版
    log "Hybrid No-Eval版をビルド中..."
    if [ -f "$SRC_HYBRID" ]; then
        local HYBRID_FLAGS="-DSTANDALONE_MAIN -DTT_SIZE_MB=$TT_SIZE_MB -DMINIMAL_OUTPUT=1"

        if [ "$MODE" = "kemeko" ]; then
            # 大規模並列向け追加パラメータ（768コア最適化）
            HYBRID_FLAGS="$HYBRID_FLAGS -DLOCAL_HEAP_CAPACITY=32768"
            HYBRID_FLAGS="$HYBRID_FLAGS -DGLOBAL_QUEUE_CAPACITY=65536"
            HYBRID_FLAGS="$HYBRID_FLAGS -DCHUNK_SIZE=32"
            HYBRID_FLAGS="$HYBRID_FLAGS -DTT_LOCK_STRIPES=32768"
        fi

        log "  コンパイルオプション: $COMMON_FLAGS $ARCH_FLAGS $HYBRID_FLAGS"
        gcc $COMMON_FLAGS $ARCH_FLAGS $HYBRID_FLAGS \
            -o "$SOLVER_HYBRID" \
            "$SRC_HYBRID" \
            -lm -lpthread 2>&1 | tee -a "$LOG_FILE"
        if [ $? -eq 0 ]; then
            log "  [OK] $SOLVER_HYBRID ビルド成功"
        else
            log "  [ERROR] $SOLVER_HYBRID ビルド失敗"
            exit 1
        fi
    else
        log "  [ERROR] $SRC_HYBRID が見つかりません"
        exit 1
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

    result=${result:-UNKNOWN}
    time=${time:-0}
    nodes=${nodes:-0}
    nps=${nps:-0}

    echo "$result,$time,$nodes,$nps"
}

parse_hybrid_result() {
    local output="$1"

    # 出力形式に応じてパース
    local result=$(echo "$output" | grep -E "^Result:" | awk '{print $2}')
    if [ -z "$result" ]; then
        result=$(echo "$output" | grep -E "Result:" | head -1 | awk '{print $NF}')
    fi

    # 時間とノード数を取得
    local time=$(echo "$output" | grep -oE '[0-9]+\.[0-9]+ sec' | head -1 | grep -oE '[0-9]+\.[0-9]+')
    if [ -z "$time" ]; then
        time=$(echo "$output" | grep -oE 'Time: [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+')
    fi

    local nodes=$(echo "$output" | grep -oE '[0-9]+ nodes' | head -1 | grep -oE '[0-9]+')
    if [ -z "$nodes" ]; then
        nodes=$(echo "$output" | grep -oE 'Nodes: [0-9]+' | grep -oE '[0-9]+')
    fi

    local nps=$(echo "$output" | grep -oE '\([0-9]+ NPS\)' | head -1 | grep -oE '[0-9]+')
    if [ -z "$nps" ] && [ "$time" != "0" ] && [ -n "$nodes" ]; then
        nps=$(echo "scale=0; $nodes / $time" | bc 2>/dev/null || echo "0")
    fi

    result=${result:-UNKNOWN}
    time=${time:-0}
    nodes=${nodes:-0}
    nps=${nps:-0}

    echo "$result,$time,$nodes,$nps"
}

# ========================================
# [実験1] 基本性能比較
# ========================================
run_basic_comparison() {
    log_section "実験1: 基本性能比較"

    local csv_file="$OUTPUT_DIR/01_basic_comparison.csv"
    echo "Empties,FileID,Threads,WPNS_Result,WPNS_Time,WPNS_Nodes,WPNS_NPS,Hybrid_Result,Hybrid_Time,Hybrid_Nodes,Hybrid_NPS,Match,Speedup" > "$csv_file"

    # 最大スレッド数で比較
    local test_threads=$(echo $THREAD_COUNTS | awk '{print $NF}')

    for empties in $EMPTIES_LIST; do
        local empties_padded=$(printf "%02d" $empties)

        for file_id in $(seq 0 $((FILES_PER_EMPTIES - 1))); do
            local file_id_padded=$(printf "%03d" $file_id)
            local pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"

            if [ ! -f "$pos_file" ]; then
                continue
            fi

            log "テスト: empties=$empties, file=$file_id_padded (スレッド: $test_threads)"

            # WPNS実行
            local wpns_output=$(timeout $TIME_LIMIT "$SOLVER_WPNS" "$pos_file" $test_threads $TIME_LIMIT 2>&1)
            local wpns_exit=$?
            local wpns_data
            if [ $wpns_exit -eq 124 ]; then
                wpns_data="TIMEOUT,$TIME_LIMIT,0,0"
            else
                wpns_data=$(parse_wpns_result "$wpns_output")
            fi

            # Hybrid実行
            local hybrid_output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$pos_file" $test_threads $TIME_LIMIT 2>&1)
            local hybrid_exit=$?
            local hybrid_data
            if [ $hybrid_exit -eq 124 ]; then
                hybrid_data="TIMEOUT,$TIME_LIMIT,0,0"
            else
                hybrid_data=$(parse_hybrid_result "$hybrid_output")
            fi

            # 結果一致チェック
            local wpns_result=$(echo "$wpns_data" | cut -d',' -f1)
            local hybrid_result=$(echo "$hybrid_data" | cut -d',' -f1)
            local wpns_time=$(echo "$wpns_data" | cut -d',' -f2)
            local hybrid_time=$(echo "$hybrid_data" | cut -d',' -f2)

            local match="N/A"
            if [ "$wpns_result" != "TIMEOUT" ] && [ "$wpns_result" != "UNKNOWN" ] && \
               [ "$hybrid_result" != "TIMEOUT" ] && [ "$hybrid_result" != "UNKNOWN" ]; then
                if [ "$wpns_result" = "$hybrid_result" ]; then
                    match="YES"
                else
                    match="NO"
                fi
            fi

            # スピードアップ計算（Hybrid / WPNS）
            local speedup="N/A"
            if [ "$wpns_time" != "0" ] && [ "$hybrid_time" != "0" ] && \
               [ "$wpns_result" != "TIMEOUT" ] && [ "$hybrid_result" != "TIMEOUT" ]; then
                speedup=$(echo "scale=2; $wpns_time / $hybrid_time" | bc 2>/dev/null || echo "N/A")
            fi

            echo "$empties,$file_id_padded,$test_threads,$wpns_data,$hybrid_data,$match,$speedup" >> "$csv_file"

            # ログ出力
            log "  WPNS:   $wpns_result (${wpns_time}s)"
            log "  Hybrid: $hybrid_result (${hybrid_time}s)"
            [ "$match" = "NO" ] && log "  [WARN] 結果不一致!"
            [ "$speedup" != "N/A" ] && log "  Speedup: ${speedup}x"
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
    local empties_padded=$(printf "%02d" $SCALING_EMPTIES)
    local test_pos="$POS_DIR/empties_${empties_padded}_id_000.pos"

    if [ ! -f "$test_pos" ]; then
        test_pos=$(ls $POS_DIR/empties_${empties_padded}_*.pos 2>/dev/null | head -1)
    fi

    if [ -z "$test_pos" ] || [ ! -f "$test_pos" ]; then
        log "[WARN] スケーリングテスト用局面が見つかりません (empties=$SCALING_EMPTIES)"
        return
    fi

    log "テスト局面: $(basename $test_pos)"
    log "スレッド数: $THREAD_COUNTS"
    log "試行回数: $TRIALS"

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

            # ベースライン設定（1スレッドの最初の試行）
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
            local hybrid_output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$test_pos" $threads $TIME_LIMIT 2>&1)
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

            log "    WPNS:   ${wpns_time}s (Speedup: ${speedup_wpns}x, Eff: ${efficiency_wpns}%)"
            log "    Hybrid: ${hybrid_time}s (Speedup: ${speedup_hybrid}x, Eff: ${efficiency_hybrid}%)"
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
            local hybrid_output=$(timeout $TIME_LIMIT "$SOLVER_HYBRID" "$pos_file" $test_threads $TIME_LIMIT 2>&1)
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
        done

        # WPNS平均
        if [ $wpns_solved -gt 0 ]; then
            local wpns_avg_time=$(echo "scale=3; $wpns_total_time / $wpns_solved" | bc 2>/dev/null || echo "0")
            local wpns_avg_nodes=$(echo "scale=0; $wpns_total_nodes / $wpns_solved" | bc 2>/dev/null || echo "0")
            local wpns_avg_nps=$(echo "scale=0; $wpns_total_nps / $wpns_solved" | bc 2>/dev/null || echo "0")
            local wpns_solve_rate=$(echo "scale=1; $wpns_solved * 100 / $wpns_count" | bc 2>/dev/null || echo "0")
            echo "$empties,WPNS,$wpns_avg_time,$wpns_avg_nodes,$wpns_avg_nps,$wpns_solved,$wpns_count,$wpns_solve_rate" >> "$csv_file"
            log "  WPNS:   avg=${wpns_avg_time}s, solved=$wpns_solved/$wpns_count"
        else
            echo "$empties,WPNS,0,0,0,0,$wpns_count,0" >> "$csv_file"
            log "  WPNS:   no solutions"
        fi

        # Hybrid平均
        if [ $hybrid_solved -gt 0 ]; then
            local hybrid_avg_time=$(echo "scale=3; $hybrid_total_time / $hybrid_solved" | bc 2>/dev/null || echo "0")
            local hybrid_avg_nodes=$(echo "scale=0; $hybrid_total_nodes / $hybrid_solved" | bc 2>/dev/null || echo "0")
            local hybrid_avg_nps=$(echo "scale=0; $hybrid_total_nps / $hybrid_solved" | bc 2>/dev/null || echo "0")
            local hybrid_solve_rate=$(echo "scale=1; $hybrid_solved * 100 / $hybrid_count" | bc 2>/dev/null || echo "0")
            echo "$empties,Hybrid,$hybrid_avg_time,$hybrid_avg_nodes,$hybrid_avg_nps,$hybrid_solved,$hybrid_count,$hybrid_solve_rate" >> "$csv_file"
            log "  Hybrid: avg=${hybrid_avg_time}s, solved=$hybrid_solved/$hybrid_count"
        else
            echo "$empties,Hybrid,0,0,0,0,$hybrid_count,0" >> "$csv_file"
            log "  Hybrid: no solutions"
        fi
    done

    log "空きマス別分析完了: $csv_file"
}

# ========================================
# [実験4] 結果一致チェック
# ========================================
run_agreement_check() {
    log_section "実験4: 結果一致チェック"

    local csv_file="$OUTPUT_DIR/04_agreement_check.csv"
    echo "Category,Total,Matched,Mismatched,Match_Rate" > "$csv_file"

    # 基本比較の一致率
    local basic_csv="$OUTPUT_DIR/01_basic_comparison.csv"
    if [ -f "$basic_csv" ]; then
        local basic_total=$(tail -n +2 "$basic_csv" | grep -c -E ",YES,|,NO," || echo "0")
        local basic_matched=$(tail -n +2 "$basic_csv" | grep -c ",YES," || echo "0")
        local basic_mismatched=$((basic_total - basic_matched))
        local basic_rate=0
        if [ $basic_total -gt 0 ]; then
            basic_rate=$(echo "scale=1; $basic_matched * 100 / $basic_total" | bc 2>/dev/null || echo "0")
        fi
        echo "Basic_Comparison,$basic_total,$basic_matched,$basic_mismatched,$basic_rate" >> "$csv_file"

        log "結果一致率: $basic_matched / $basic_total (${basic_rate}%)"

        # 不一致があれば詳細を表示
        if [ $basic_mismatched -gt 0 ]; then
            log "[WARN] 不一致ケース:"
            tail -n +2 "$basic_csv" | grep ",NO," | while read line; do
                log "  $line"
            done
        fi
    fi

    log "結果一致チェック完了: $csv_file"
}

# ========================================
# [実験5] CPU効率・メモリ使用量測定
# ========================================
run_resource_monitoring() {
    log_section "実験5: リソース使用量測定"

    local csv_file="$OUTPUT_DIR/05_resource_usage.csv"
    echo "Solver,Threads,Empties,Peak_Memory_KB,Avg_CPU_Percent,Time_Sec" > "$csv_file"

    # 最大スレッド数でテスト
    local test_threads=$(echo $THREAD_COUNTS | awk '{print $NF}')
    local empties_padded=$(printf "%02d" $SCALING_EMPTIES)
    local test_pos="$POS_DIR/empties_${empties_padded}_id_000.pos"

    if [ ! -f "$test_pos" ]; then
        log "[WARN] テスト局面が見つかりません: $test_pos"
        return
    fi

    log "リソース測定テスト (empties=$SCALING_EMPTIES, threads=$test_threads)"

    for solver_name in "WPNS" "Hybrid"; do
        if [ "$solver_name" = "WPNS" ]; then
            local solver_bin="$SOLVER_WPNS"
        else
            local solver_bin="$SOLVER_HYBRID"
        fi

        log "  $solver_name を測定中..."

        # /usr/bin/time を使用してメモリ・CPU測定
        local time_output=$(mktemp)
        local solver_output=$(mktemp)

        # GNU time形式で測定 (-v オプション)
        if command -v /usr/bin/time &> /dev/null; then
            /usr/bin/time -v timeout $TIME_LIMIT "$solver_bin" "$test_pos" $test_threads $TIME_LIMIT \
                > "$solver_output" 2> "$time_output"

            # メモリ使用量（Maximum resident set size）
            local peak_mem=$(grep "Maximum resident set size" "$time_output" | awk '{print $NF}')
            peak_mem=${peak_mem:-0}

            # CPU使用率（Percent of CPU this job got）
            local cpu_percent=$(grep "Percent of CPU" "$time_output" | awk '{print $NF}' | tr -d '%')
            cpu_percent=${cpu_percent:-0}

            # 実行時間
            local elapsed=$(grep "Elapsed" "$time_output" | awk '{print $NF}')
            # mm:ss.ss または h:mm:ss 形式を秒に変換
            if [[ "$elapsed" =~ ^([0-9]+):([0-9]+)\.([0-9]+)$ ]]; then
                local mins=${BASH_REMATCH[1]}
                local secs=${BASH_REMATCH[2]}
                local frac=${BASH_REMATCH[3]}
                elapsed=$(echo "$mins * 60 + $secs + 0.$frac" | bc)
            elif [[ "$elapsed" =~ ^([0-9]+):([0-9]+):([0-9]+)$ ]]; then
                local hours=${BASH_REMATCH[1]}
                local mins=${BASH_REMATCH[2]}
                local secs=${BASH_REMATCH[3]}
                elapsed=$(echo "$hours * 3600 + $mins * 60 + $secs" | bc)
            fi
            elapsed=${elapsed:-0}

            echo "$solver_name,$test_threads,$SCALING_EMPTIES,$peak_mem,$cpu_percent,$elapsed" >> "$csv_file"

            # メモリをMB単位で表示
            local mem_mb=$(echo "scale=1; $peak_mem / 1024" | bc 2>/dev/null || echo "0")
            log "    Peak Memory: ${mem_mb}MB, CPU: ${cpu_percent}%, Time: ${elapsed}s"
        else
            log "    [WARN] /usr/bin/time が見つかりません。スキップします。"

            # 代替: 単純に実行
            timeout $TIME_LIMIT "$solver_bin" "$test_pos" $test_threads $TIME_LIMIT > "$solver_output" 2>&1
            echo "$solver_name,$test_threads,$SCALING_EMPTIES,N/A,N/A,N/A" >> "$csv_file"
        fi

        rm -f "$time_output" "$solver_output"
    done

    # 複数スレッド数でのCPU効率比較
    log ""
    log "スレッド数別CPU効率測定..."

    local cpu_csv="$OUTPUT_DIR/05_cpu_scaling.csv"
    echo "Solver,Threads,CPU_Percent,Expected_CPU,Efficiency" > "$cpu_csv"

    for threads in $THREAD_COUNTS; do
        # 短めのタイムアウトで測定
        local short_timeout=30
        if [ "$MODE" = "kemeko" ]; then
            short_timeout=60
        fi

        for solver_name in "WPNS" "Hybrid"; do
            if [ "$solver_name" = "WPNS" ]; then
                local solver_bin="$SOLVER_WPNS"
            else
                local solver_bin="$SOLVER_HYBRID"
            fi

            local time_output=$(mktemp)

            if command -v /usr/bin/time &> /dev/null; then
                /usr/bin/time -v timeout $short_timeout "$solver_bin" "$test_pos" $threads $short_timeout \
                    > /dev/null 2> "$time_output"

                local cpu_percent=$(grep "Percent of CPU" "$time_output" | awk '{print $NF}' | tr -d '%')
                cpu_percent=${cpu_percent:-0}

                # 期待CPU使用率 = threads * 100
                local expected_cpu=$((threads * 100))

                # 効率 = 実際のCPU% / 期待CPU%
                local efficiency=0
                if [ "$expected_cpu" -gt 0 ]; then
                    efficiency=$(echo "scale=1; $cpu_percent * 100 / $expected_cpu" | bc 2>/dev/null || echo "0")
                fi

                echo "$solver_name,$threads,$cpu_percent,$expected_cpu,$efficiency" >> "$cpu_csv"
                log "  $solver_name (${threads}T): CPU=${cpu_percent}% (expected ${expected_cpu}%), Eff=${efficiency}%"
            fi

            rm -f "$time_output"
        done
    done

    log "リソース使用量測定完了"
}

# ========================================
# サマリーレポート生成
# ========================================
generate_summary() {
    log_section "サマリーレポート生成"

    local summary_file="$OUTPUT_DIR/summary.txt"

    cat > "$summary_file" << EOF
================================================================================
Hybrid No-Eval vs WPNS TT-Parallel ベンチマーク結果
================================================================================
実行日時: $(date)
実行モード: $MODE
タイムアウト: ${TIME_LIMIT}秒
スレッド数: $THREAD_COUNTS
空きマス数: $EMPTIES_LIST
スケーリングテスト空きマス: $SCALING_EMPTIES
試行回数: $TRIALS
TTサイズ: ${TT_SIZE_MB}MB

比較対象:
  - WPNS:   $SRC_WPNS
  - Hybrid: $SRC_HYBRID

--------------------------------------------------------------------------------
[1] 基本性能比較
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/01_basic_comparison.csv" ]; then
        # 各ソルバーの平均時間
        local wpns_avg=$(awk -F',' 'NR>1 && $4!="TIMEOUT" && $4!="UNKNOWN" {sum+=$5; count++} END {if(count>0) printf "%.3f", sum/count; else print "N/A"}' "$OUTPUT_DIR/01_basic_comparison.csv")
        local hybrid_avg=$(awk -F',' 'NR>1 && $8!="TIMEOUT" && $8!="UNKNOWN" {sum+=$9; count++} END {if(count>0) printf "%.3f", sum/count; else print "N/A"}' "$OUTPUT_DIR/01_basic_comparison.csv")
        local avg_speedup=$(awk -F',' 'NR>1 && $13!="N/A" {sum+=$13; count++} END {if(count>0) printf "%.2f", sum/count; else print "N/A"}' "$OUTPUT_DIR/01_basic_comparison.csv")

        cat >> "$summary_file" << EOF
WPNS平均時間:     ${wpns_avg}秒
Hybrid平均時間:   ${hybrid_avg}秒
平均スピードアップ: ${avg_speedup}x (Hybrid vs WPNS)

詳細:
$(awk -F',' 'NR>1 {printf "  empties=%s file=%s: WPNS=%.3fs, Hybrid=%.3fs, Speedup=%s\n", $1, $2, $5, $9, $13}' "$OUTPUT_DIR/01_basic_comparison.csv")

EOF
    fi

    cat >> "$summary_file" << EOF
--------------------------------------------------------------------------------
[2] 強スケーリング (empties=$SCALING_EMPTIES)
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/02_scaling_wpns.csv" ] && [ -f "$OUTPUT_DIR/02_scaling_hybrid.csv" ]; then
        echo "スレッド | WPNS時間 | WPNS効率 | Hybrid時間 | Hybrid効率" >> "$summary_file"
        echo "---------|----------|----------|------------|------------" >> "$summary_file"

        for threads in $THREAD_COUNTS; do
            local wpns_line=$(awk -F',' -v t="$threads" '$1==t {print; exit}' "$OUTPUT_DIR/02_scaling_wpns.csv")
            local hybrid_line=$(awk -F',' -v t="$threads" '$1==t {print; exit}' "$OUTPUT_DIR/02_scaling_hybrid.csv")

            local wpns_time=$(echo "$wpns_line" | cut -d',' -f3)
            local wpns_eff=$(echo "$wpns_line" | cut -d',' -f7)
            local hybrid_time=$(echo "$hybrid_line" | cut -d',' -f3)
            local hybrid_eff=$(echo "$hybrid_line" | cut -d',' -f7)

            printf "%8s | %8ss | %7s%% | %10ss | %9s%%\n" "$threads" "$wpns_time" "$wpns_eff" "$hybrid_time" "$hybrid_eff" >> "$summary_file"
        done
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
[4] 結果一致チェック
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/04_agreement_check.csv" ]; then
        awk -F',' 'NR>1 {printf "%s: %d/%d 一致 (%.1f%%)\n", $1, $3, $2, $5}' "$OUTPUT_DIR/04_agreement_check.csv" >> "$summary_file"
    fi

    cat >> "$summary_file" << EOF

--------------------------------------------------------------------------------
[5] リソース使用量（メモリ・CPU効率）
--------------------------------------------------------------------------------
EOF

    if [ -f "$OUTPUT_DIR/05_resource_usage.csv" ]; then
        echo "ピークメモリ使用量:" >> "$summary_file"
        awk -F',' 'NR>1 {
            mem_mb = $4 / 1024;
            printf "  %s: %.1f MB (CPU: %s%%, Time: %ss)\n", $1, mem_mb, $5, $6
        }' "$OUTPUT_DIR/05_resource_usage.csv" >> "$summary_file"
        echo "" >> "$summary_file"
    fi

    if [ -f "$OUTPUT_DIR/05_cpu_scaling.csv" ]; then
        echo "CPU効率（スレッド別）:" >> "$summary_file"
        echo "スレッド | WPNS CPU% | WPNS効率 | Hybrid CPU% | Hybrid効率" >> "$summary_file"
        echo "---------|-----------|----------|-------------|------------" >> "$summary_file"

        for threads in $THREAD_COUNTS; do
            local wpns_line=$(awk -F',' -v t="$threads" '$1=="WPNS" && $2==t {print}' "$OUTPUT_DIR/05_cpu_scaling.csv")
            local hybrid_line=$(awk -F',' -v t="$threads" '$1=="Hybrid" && $2==t {print}' "$OUTPUT_DIR/05_cpu_scaling.csv")

            local wpns_cpu=$(echo "$wpns_line" | cut -d',' -f3)
            local wpns_eff=$(echo "$wpns_line" | cut -d',' -f5)
            local hybrid_cpu=$(echo "$hybrid_line" | cut -d',' -f3)
            local hybrid_eff=$(echo "$hybrid_line" | cut -d',' -f5)

            wpns_cpu=${wpns_cpu:-N/A}
            wpns_eff=${wpns_eff:-N/A}
            hybrid_cpu=${hybrid_cpu:-N/A}
            hybrid_eff=${hybrid_eff:-N/A}

            printf "%8s | %9s | %8s%% | %11s | %10s%%\n" "$threads" "$wpns_cpu" "$wpns_eff" "$hybrid_cpu" "$hybrid_eff" >> "$summary_file"
        done
        echo "" >> "$summary_file"
    fi

    cat >> "$summary_file" << EOF

================================================================================
結論
================================================================================
EOF

    # 結論を自動生成
    if [ -f "$OUTPUT_DIR/01_basic_comparison.csv" ]; then
        local avg_speedup=$(awk -F',' 'NR>1 && $13!="N/A" {sum+=$13; count++} END {if(count>0) printf "%.2f", sum/count; else print "1"}' "$OUTPUT_DIR/01_basic_comparison.csv")

        if [ $(echo "$avg_speedup > 1" | bc 2>/dev/null || echo "0") -eq 1 ]; then
            echo "Hybrid No-Eval は WPNS より平均 ${avg_speedup}x 高速です。" >> "$summary_file"
        else
            local inverse=$(echo "scale=2; 1 / $avg_speedup" | bc 2>/dev/null || echo "1")
            echo "WPNS は Hybrid No-Eval より平均 ${inverse}x 高速です。" >> "$summary_file"
        fi
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
    echo "Hybrid No-Eval vs WPNS ベンチマーク"
    echo "========================================"
    echo ""
    echo "設定:"
    echo "  モード: $MODE"
    echo "  タイムアウト: ${TIME_LIMIT}秒"
    echo "  スレッド数: $THREAD_COUNTS"
    echo "  空きマス数: $EMPTIES_LIST"
    echo "  スケーリングテスト空きマス: $SCALING_EMPTIES"
    echo "  試行回数: $TRIALS"
    echo "  TTサイズ: ${TT_SIZE_MB}MB"
    echo ""
    echo "ソースファイル:"
    echo "  WPNS:   $SRC_WPNS"
    echo "  Hybrid: $SRC_HYBRID"
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
    run_agreement_check
    run_resource_monitoring

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
