#!/bin/bash
#
# WPNS TT-Parallel vs Hybrid Solver 比較ベンチマークスクリプト
#
# 目的: wpns_tt_parallel.c と othello_endgame_solver_hybrid_check_tthit_fixed.c の性能比較
#
# 使用方法:
#   ./run_wpns_benchmark.sh [オプション]
#
# オプション:
#   -t <秒>         タイムアウト時間 (デフォルト: 60)
#   -T <スレッド>   スレッド数リスト (デフォルト: "1 2 4 8")
#   -e <リスト>     空きマス数リスト (デフォルト: "10 11 12 13 14")
#   -n <数>         各空きマスのテストファイル数 (デフォルト: 5)
#   -f              FFOテストも実行 (end40-end59)
#   -F              FFOテストのみ実行
#   --wsl           WSL環境用設定 (4スレッドまで)
#   --kemeko        学内マシン用設定 (768スレッドまで)
#   -q              クイックモード (空きマス10-12, 各3ファイル)
#   -h              ヘルプ表示
#

set -e

# ========================================
# デフォルト設定
# ========================================
TIME_LIMIT=60
THREAD_COUNTS="1 2 4 8"
EMPTIES_LIST="10 11 12 13 14"
FILES_PER_EMPTIES=5
RUN_FFO=false
FFO_ONLY=false
TT_SIZE_MB=4096

# ディレクトリ
POS_DIR="test_positions"
FFO_DIR="ffotest"
EVAL_FILE="eval/eval.dat"

# 出力ディレクトリ
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="wpns_benchmark_${TIMESTAMP}"

# ソルバーバイナリ
SOLVER_WPNS="./wpns_tt_parallel"
SOLVER_HYBRID="./othello_hybrid_solver"

# ========================================
# コマンドライン引数処理
# ========================================
show_usage() {
    echo "使用方法: $0 [オプション]"
    echo ""
    echo "オプション:"
    echo "  -t <秒>         タイムアウト時間 (デフォルト: 60)"
    echo "  -T <リスト>     スレッド数リスト (デフォルト: \"1 2 4 8\")"
    echo "  -e <リスト>     空きマス数リスト (デフォルト: \"10 11 12 13 14\")"
    echo "  -n <数>         各空きマスのテストファイル数 (デフォルト: 5)"
    echo "  -f              FFOテストも実行"
    echo "  -F              FFOテストのみ実行"
    echo "  --wsl           WSL環境用設定 (スレッド: 1 2 4, 空きマス: 10-12)"
    echo "  --kemeko        学内マシン用設定 (スレッド: 1 2 4 8 16 32 64 128 256 384 768)"
    echo "  -q              クイックモード"
    echo "  -h              このヘルプを表示"
    echo ""
    echo "例:"
    echo "  $0 --wsl                   # WSLでテスト実行"
    echo "  $0 --kemeko -f             # 学内マシンでフルテスト"
    echo "  $0 -T \"1 4 16\" -e \"12 14\" # カスタム設定"
}

# 引数処理
while [[ $# -gt 0 ]]; do
    case $1 in
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
        -f)
            RUN_FFO=true
            shift
            ;;
        -F)
            FFO_ONLY=true
            RUN_FFO=true
            shift
            ;;
        --wsl)
            THREAD_COUNTS="1 2 4"
            EMPTIES_LIST="10 11 12"
            FILES_PER_EMPTIES=3
            TIME_LIMIT=30
            TT_SIZE_MB=1024
            shift
            ;;
        --kemeko)
            THREAD_COUNTS="1 2 4 8 16 32 64 128 256 384 768"
            EMPTIES_LIST="10 12 14 16 18"
            FILES_PER_EMPTIES=10
            TIME_LIMIT=300
            TT_SIZE_MB=8192
            RUN_FFO=true
            shift
            ;;
        -q)
            EMPTIES_LIST="10 11 12"
            FILES_PER_EMPTIES=3
            TIME_LIMIT=30
            shift
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            echo "不明なオプション: $1"
            show_usage
            exit 1
            ;;
    esac
done

# ========================================
# ソルバービルド
# ========================================
build_solvers() {
    echo "========================================"
    echo "ソルバーのビルド"
    echo "========================================"

    # WPNS TT-Parallel版のビルド
    echo "[1/2] WPNS TT-Parallel版をビルド中..."
    if [ -f "wpns_tt_parallel.c" ]; then
        gcc -O3 -march=native -pthread \
            -DTT_SIZE_MB=$TT_SIZE_MB \
            -DMAX_THREADS=1024 \
            -o "$SOLVER_WPNS" \
            wpns_tt_parallel.c \
            -lm -lpthread 2>&1

        if [ $? -eq 0 ]; then
            echo "  [OK] $SOLVER_WPNS ビルド成功"
        else
            echo "  [ERROR] $SOLVER_WPNS ビルド失敗"
            exit 1
        fi
    else
        echo "  [ERROR] wpns_tt_parallel.c が見つかりません"
        exit 1
    fi

    # Hybrid版のビルド
    echo "[2/2] Hybrid版をビルド中..."
    if [ -f "othello_endgame_solver_hybrid_check_tthit_fixed.c" ]; then
        gcc -O3 -march=native -pthread \
            -DSTANDALONE_MAIN \
            -DMAX_THREADS=1024 \
            -o "$SOLVER_HYBRID" \
            othello_endgame_solver_hybrid_check_tthit_fixed.c \
            -lm -lpthread 2>&1

        if [ $? -eq 0 ]; then
            echo "  [OK] $SOLVER_HYBRID ビルド成功"
        else
            echo "  [WARN] $SOLVER_HYBRID ビルド失敗（Hybridテストをスキップ）"
            SOLVER_HYBRID=""
        fi
    else
        echo "  [WARN] othello_endgame_solver_hybrid_check_tthit_fixed.c が見つかりません"
        SOLVER_HYBRID=""
    fi

    echo ""
}

# ========================================
# テスト実行関数
# ========================================
run_wpns() {
    local pos_file=$1
    local threads=$2
    local timeout_sec=$3

    timeout $timeout_sec "$SOLVER_WPNS" "$pos_file" $threads $timeout_sec 2>&1
}

run_hybrid() {
    local pos_file=$1
    local threads=$2
    local timeout_sec=$3

    if [ -n "$SOLVER_HYBRID" ] && [ -x "$SOLVER_HYBRID" ]; then
        # -v フラグを追加してタイミング情報を出力
        timeout $timeout_sec "$SOLVER_HYBRID" "$pos_file" $threads $timeout_sec "$EVAL_FILE" -v 2>&1
    else
        echo "SKIP"
    fi
}

# ========================================
# 結果パース関数
# ========================================
parse_wpns_result() {
    local output="$1"

    local result=$(echo "$output" | grep -E "^Result:" | awk '{print $2}')
    local time=$(echo "$output" | grep -E "^Total:" | grep -oE '[0-9]+\.[0-9]+' | head -1)
    local nodes=$(echo "$output" | grep -E "^Total:" | grep -oE '[0-9]+ nodes' | grep -oE '[0-9]+')
    local nps=$(echo "$output" | grep -E "NPS" | grep -oE '[0-9]+' | head -1)
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

    # Result: WIN/LOSE を抽出
    local result=$(echo "$output" | grep -E "^Result:" | awk '{print $2}')

    # "Total: XXXX nodes in Y.YYY seconds (NNNN NPS)" 形式をパース
    # 注: -v出力時はタイムスタンプ付き "[HH:MM:SS.mmm] Total: ..."
    local total_line=$(echo "$output" | grep -E "Total:.*nodes.*seconds")
    local time=$(echo "$total_line" | grep -oE '[0-9]+\.[0-9]+ seconds' | grep -oE '[0-9]+\.[0-9]+')
    local nodes=$(echo "$total_line" | grep -oE '[0-9]+ nodes' | grep -oE '[0-9]+')
    local nps=$(echo "$total_line" | grep -oE '\([0-9]+ NPS\)' | grep -oE '[0-9]+')

    # TT hits (あれば)
    local tt_hits=$(echo "$output" | grep -E "TT hit" | grep -oE '[0-9]+' | head -1)

    result=${result:-UNKNOWN}
    time=${time:-0}
    nodes=${nodes:-0}
    nps=${nps:-0}
    tt_hits=${tt_hits:-0}

    echo "$result,$time,$nodes,$nps,$tt_hits"
}

# ========================================
# ベンチマーク実行
# ========================================
run_benchmark() {
    local test_type=$1  # "empties" or "ffo"
    local csv_file=$2

    if [ "$test_type" = "empties" ]; then
        echo ""
        echo "========================================"
        echo "空きマス数別テスト"
        echo "空きマス: $EMPTIES_LIST"
        echo "各空きマス: ${FILES_PER_EMPTIES}ファイル"
        echo "========================================"

        for empties in $EMPTIES_LIST; do
            empties_padded=$(printf "%02d" $empties)

            for file_id in $(seq 0 $((FILES_PER_EMPTIES - 1))); do
                file_id_padded=$(printf "%03d" $file_id)
                pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"

                if [ ! -f "$pos_file" ]; then
                    continue
                fi

                run_single_test "$pos_file" "$empties" "$file_id_padded" "$csv_file"
            done
        done
    elif [ "$test_type" = "ffo" ]; then
        echo ""
        echo "========================================"
        echo "FFO難問テスト (end40-end59)"
        echo "========================================"

        for ffo_file in $FFO_DIR/end*.pos; do
            if [ ! -f "$ffo_file" ]; then
                continue
            fi

            # ファイル名からIDを抽出 (end40.pos -> 40)
            ffo_id=$(basename "$ffo_file" .pos | sed 's/end//')
            empties=$(head -1 "$ffo_file" | tr -cd '.-' | wc -c)

            run_single_test "$ffo_file" "$empties" "ffo$ffo_id" "$csv_file"
        done
    fi
}

run_single_test() {
    local pos_file=$1
    local empties=$2
    local file_id=$3
    local csv_file=$4

    echo ""
    echo "----------------------------------------"
    echo "テスト: $pos_file (空きマス: $empties)"
    echo "----------------------------------------"

    for threads in $THREAD_COUNTS; do
        echo ""
        echo "  スレッド数: $threads"
        echo "  ----------------"

        # WPNS実行
        echo "    [WPNS] 実行中..."
        wpns_output=$(run_wpns "$pos_file" $threads $TIME_LIMIT)
        wpns_exit=$?

        if [ $wpns_exit -eq 124 ]; then
            wpns_data="TIMEOUT,$TIME_LIMIT,0,0,0"
            echo "    [WPNS] TIMEOUT"
        else
            wpns_data=$(parse_wpns_result "$wpns_output")
            wpns_result=$(echo "$wpns_data" | cut -d',' -f1)
            wpns_time=$(echo "$wpns_data" | cut -d',' -f2)
            wpns_nodes=$(echo "$wpns_data" | cut -d',' -f3)
            wpns_nps=$(echo "$wpns_data" | cut -d',' -f4)
            echo "    [WPNS] Result=$wpns_result Time=${wpns_time}s Nodes=$wpns_nodes NPS=$wpns_nps"
        fi

        # Hybrid実行
        if [ -n "$SOLVER_HYBRID" ] && [ -x "$SOLVER_HYBRID" ]; then
            echo "    [Hybrid] 実行中..."
            hybrid_output=$(run_hybrid "$pos_file" $threads $TIME_LIMIT)
            hybrid_exit=$?

            if [ $hybrid_exit -eq 124 ]; then
                hybrid_data="TIMEOUT,$TIME_LIMIT,0,0,0"
                echo "    [Hybrid] TIMEOUT"
            else
                hybrid_data=$(parse_hybrid_result "$hybrid_output")
                hybrid_result=$(echo "$hybrid_data" | cut -d',' -f1)
                hybrid_time=$(echo "$hybrid_data" | cut -d',' -f2)
                hybrid_nodes=$(echo "$hybrid_data" | cut -d',' -f3)
                hybrid_nps=$(echo "$hybrid_data" | cut -d',' -f4)
                echo "    [Hybrid] Result=$hybrid_result Time=${hybrid_time}s Nodes=$hybrid_nodes NPS=$hybrid_nps"
            fi
        else
            hybrid_data="SKIP,0,0,0,0"
        fi

        # 結果一致チェック
        wpns_result=$(echo "$wpns_data" | cut -d',' -f1)
        hybrid_result=$(echo "$hybrid_data" | cut -d',' -f1)
        if [ "$wpns_result" != "TIMEOUT" ] && [ "$hybrid_result" != "TIMEOUT" ] && [ "$hybrid_result" != "SKIP" ]; then
            if [ "$wpns_result" = "$hybrid_result" ]; then
                echo "    [CHECK] 結果一致: $wpns_result"
            else
                echo "    [WARN] 結果不一致! WPNS=$wpns_result Hybrid=$hybrid_result"
            fi
        fi

        # CSV出力
        echo "$empties,$file_id,$pos_file,$threads,$wpns_data,$hybrid_data" >> "$csv_file"

        # 詳細ログ保存
        echo "=== WPNS Output ===" > "$LOG_DIR/detail_${empties}_${file_id}_t${threads}.log"
        echo "$wpns_output" >> "$LOG_DIR/detail_${empties}_${file_id}_t${threads}.log"
        echo "" >> "$LOG_DIR/detail_${empties}_${file_id}_t${threads}.log"
        echo "=== Hybrid Output ===" >> "$LOG_DIR/detail_${empties}_${file_id}_t${threads}.log"
        echo "$hybrid_output" >> "$LOG_DIR/detail_${empties}_${file_id}_t${threads}.log"
    done
}

# ========================================
# サマリー生成
# ========================================
generate_summary() {
    local csv_file=$1
    local summary_file="$LOG_DIR/summary.txt"

    echo ""
    echo "========================================"
    echo "ベンチマーク結果サマリー"
    echo "========================================"

    cat > "$summary_file" << EOF
========================================
WPNS TT-Parallel vs Hybrid 比較ベンチマーク結果
========================================
実行日時: $(date)
タイムアウト: ${TIME_LIMIT}秒
スレッド数: $THREAD_COUNTS
空きマス数: $EMPTIES_LIST
FFOテスト: $RUN_FFO

----------------------------------------
スレッド数別 平均実行時間比較
----------------------------------------
EOF

    for threads in $THREAD_COUNTS; do
        wpns_avg=$(grep ",$threads," "$csv_file" 2>/dev/null | \
            awk -F',' '{if($6 != "TIMEOUT" && $6 > 0) {sum+=$6; count++}} END {if(count>0) printf "%.3f", sum/count; else print "N/A"}')
        hybrid_avg=$(grep ",$threads," "$csv_file" 2>/dev/null | \
            awk -F',' '{if($11 != "TIMEOUT" && $11 != "SKIP" && $11 > 0) {sum+=$11; count++}} END {if(count>0) printf "%.3f", sum/count; else print "N/A"}')

        echo "スレッド $threads:" >> "$summary_file"
        echo "  WPNS平均時間:   ${wpns_avg}秒" >> "$summary_file"
        echo "  Hybrid平均時間: ${hybrid_avg}秒" >> "$summary_file"
        echo "" >> "$summary_file"
    done

    echo "----------------------------------------" >> "$summary_file"
    echo "結果一致率" >> "$summary_file"
    echo "----------------------------------------" >> "$summary_file"

    total=$(grep -v "^Empties" "$csv_file" 2>/dev/null | grep -v "TIMEOUT" | grep -v "SKIP" | wc -l)
    match=$(grep -v "^Empties" "$csv_file" 2>/dev/null | \
        awk -F',' '{if($5 == $10 && $5 != "TIMEOUT" && $10 != "SKIP") print}' | wc -l)

    if [ "$total" -gt 0 ]; then
        rate=$(echo "scale=1; $match * 100 / $total" | bc)
        echo "一致: $match / $total (${rate}%)" >> "$summary_file"
    else
        echo "一致: 0 / 0" >> "$summary_file"
    fi

    echo "" >> "$summary_file"
    echo "========================================" >> "$summary_file"
    echo "結果ファイル:" >> "$summary_file"
    echo "  CSV: $csv_file" >> "$summary_file"
    echo "  詳細ログ: $LOG_DIR/" >> "$summary_file"
    echo "========================================" >> "$summary_file"

    cat "$summary_file"
}

# ========================================
# メイン処理
# ========================================
main() {
    echo "========================================"
    echo "WPNS TT-Parallel vs Hybrid 比較ベンチマーク"
    echo "========================================"
    echo ""
    echo "設定:"
    echo "  タイムアウト: ${TIME_LIMIT}秒"
    echo "  スレッド数: $THREAD_COUNTS"
    if [ "$FFO_ONLY" = false ]; then
        echo "  空きマス数: $EMPTIES_LIST"
        echo "  各空きマスファイル数: $FILES_PER_EMPTIES"
    fi
    echo "  FFOテスト: $RUN_FFO"
    echo "  TTサイズ: ${TT_SIZE_MB}MB"
    echo ""

    # ログディレクトリ作成
    mkdir -p "$LOG_DIR"
    echo "ログディレクトリ: $LOG_DIR"
    echo ""

    # ソルバービルド
    build_solvers

    # CSVヘッダー作成
    CSV_FILE="$LOG_DIR/results.csv"
    echo "Empties,FileID,PosFile,Threads,WPNS_Result,WPNS_Time,WPNS_Nodes,WPNS_NPS,WPNS_TTHits,Hybrid_Result,Hybrid_Time,Hybrid_Nodes,Hybrid_NPS,Hybrid_TTHits" > "$CSV_FILE"

    # 空きマス数別テスト実行
    if [ "$FFO_ONLY" = false ]; then
        run_benchmark "empties" "$CSV_FILE"
    fi

    # FFOテスト実行
    if [ "$RUN_FFO" = true ]; then
        run_benchmark "ffo" "$CSV_FILE"
    fi

    # サマリー生成
    generate_summary "$CSV_FILE"

    echo ""
    echo "ベンチマーク完了!"
    echo "結果: $CSV_FILE"
}

# 実行
main "$@"
