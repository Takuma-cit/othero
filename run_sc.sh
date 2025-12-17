#!/bin/bash
#
# 並列化スケーラビリティ測定ベンチマークスクリプト
# - 複数のスレッド数で同じテストポジションを実行
# - スピードアップ率、効率性、並列化オーバーヘッドを分析
#

# ========================================
# デフォルト設定
# ========================================
DEFAULT_THREAD_COUNTS="4 8 16 24 32 64 128 256 562 1536 "  # テストするスレッド数のリスト
TT_SIZE_MB=2048000                            # 8GB (全テストで共通)
EVAL_FILE="eval/eval.dat"
POS_DIR="test_positions"
TIME_LIMIT=${1:-3600.0}                     # 30分
TEST_MODE=${2:-quick}                      # quick/normal/full
SOLVER_BASE="./othello_solver_scalability"

# ========================================
# コマンドライン引数処理
# ========================================
show_usage() {
    echo "使用法: $0 [TIME_LIMIT] [TEST_MODE] [THREAD_COUNTS]"
    echo ""
    echo "引数:"
    echo "  TIME_LIMIT     : 各テストの制限時間（秒、デフォルト: 300.0）"
    echo "  TEST_MODE      : テストモード (quick/normal/full、デフォルト: quick)"
    echo "  THREAD_COUNTS  : スレッド数のリスト（空白区切り、デフォルト: '1 2 4 8 16 32 64'）"
    echo ""
    echo "例:"
    echo "  $0 300.0 quick '1 2 4 8 16'         # クイックテスト、5スレッド設定"
    echo "  $0 600.0 normal '1 8 64 128 256'    # ノーマルテスト、5スレッド設定"
    echo "  $0 1800.0 full '1 16 64 256 768'    # フルテスト、5スレッド設定"
    exit 0
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_usage
fi

if [ -n "$3" ]; then
    THREAD_COUNTS="$3"
else
    THREAD_COUNTS="$DEFAULT_THREAD_COUNTS"
fi

# ========================================
# 並列化パラメータ（全テスト共通）
# ========================================
G_PARAM=1                     # 最大世代（小さい値でOK: アイドル駆動で自動継続）
D_PARAM=6                     # スポーン開始深さ（適度な深さで効率的）
S_PARAM=3                     # スポーン制限（オーバーヘッド最小、最も効率的）

# HYBRID設定
CHUNK_SIZE=16
LOCAL_HEAP_SIZE=4096          # 各workerのヒープサイズ増
GLOBAL_QUEUE_SIZE=32768       # グローバルキュー大きめ
SHARED_ARRAY_SIZE=32768       # 共有配列サイズ増

# ========================================
# テスト対象の空きマス数
# ========================================
case "$TEST_MODE" in
    quick)
        # 少数のポジションで素早くスケーラビリティをテスト
        EMPTIES_START=14
        EMPTIES_END=18
        FILES_PER_EMPTIES=3
        ;;
    normal)
        # 中程度のポジションでバランス良くテスト
        EMPTIES_START=14
        EMPTIES_END=25
        FILES_PER_EMPTIES=5
        ;;
    full)
        # 幅広い範囲で詳細にテスト
        EMPTIES_START=10
        EMPTIES_END=26
        FILES_PER_EMPTIES=10
        ;;
    *)
        echo "不明なテストモード: $TEST_MODE"
        exit 1
        ;;
esac

# ========================================
# ソルバービルド
# ========================================
build_solver() {
    local max_threads=$1
    local solver_name="${SOLVER_BASE}_${max_threads}"

    echo "========================================"
    echo "ソルバービルド中..."
    echo "  最大スレッド数: $max_threads"
    echo "  TTサイズ: ${TT_SIZE_MB}MB"
    echo "  出力: $solver_name"
    echo "========================================"

    make clean > /dev/null 2>&1

    # ビルド（使用可能なCPU機能に基づいて最適化）
    gcc -O3 -march=native -mtune=native -pthread -Wall -Wextra \
        -DSTANDALONE_MAIN \
        -DMAX_THREADS=$max_threads \
        -DTT_SIZE_MB=$TT_SIZE_MB \
        -DCHUNK_SIZE=$CHUNK_SIZE \
        -DLOCAL_HEAP_SIZE=$LOCAL_HEAP_SIZE \
        -DGLOBAL_QUEUE_SIZE=$GLOBAL_QUEUE_SIZE \
        -DSHARED_ARRAY_SIZE=$SHARED_ARRAY_SIZE \
        -flto -ffast-math \
        -o "$solver_name" \
        othello_endgame_solver_hybrid_check_tthit_fixed.c \
        -lm -lpthread

    if [ $? -ne 0 ]; then
        echo "ビルド失敗: $solver_name"
        return 1
    fi

    echo "ビルド完了: $solver_name"
    echo ""
    return 0
}

# ========================================
# システム情報表示
# ========================================
show_system_info() {
    echo "========================================"
    echo "システム情報"
    echo "========================================"
    echo "CPU:"
    lscpu | grep "Model name" | head -1 || echo "  情報取得不可"
    echo "論理コア数: $(nproc)"
    echo ""
    echo "メモリ:"
    free -h | grep "^Mem:" || echo "  情報取得不可"
    echo "========================================"
    echo ""
}

# ========================================
# 単一テスト実行
# ========================================
run_single_test() {
    local pos_file=$1
    local threads=$2
    local log_file=$3
    local solver="${SOLVER_BASE}_${threads}"

    # ソルバーが存在しない場合はビルド
    if [ ! -f "$solver" ]; then
        build_solver $threads
        if [ $? -ne 0 ]; then
            return 1
        fi
    fi

    # NUMA最適化が利用可能な場合は使用
    if command -v numactl &> /dev/null; then
        numactl --interleave=all \
            timeout $((${TIME_LIMIT%.*} + 60)) \
            "$solver" "$pos_file" $threads $TIME_LIMIT "$EVAL_FILE" \
            -G $G_PARAM -D $D_PARAM -S $S_PARAM -v 2>&1 | tee "$log_file"
    else
        timeout $((${TIME_LIMIT%.*} + 60)) \
            "$solver" "$pos_file" $threads $TIME_LIMIT "$EVAL_FILE" \
            -G $G_PARAM -D $D_PARAM -S $S_PARAM -v 2>&1 | tee "$log_file"
    fi

    return ${PIPESTATUS[0]}
}

# ========================================
# 結果解析
# ========================================
parse_result() {
    local log_file=$1

    # 基本結果
    local result=$(grep "^Result:" "$log_file" | awk '{print $2}')
    local nodes=$(grep "^Total:" "$log_file" | awk '{print $2}')
    local time=$(grep "^Total:" "$log_file" | awk '{print $5}')
    local nps=$(grep "^Total:" "$log_file" | sed 's/.*(\([0-9]*\) NPS).*/\1/')

    # Worker稼働率を抽出
    local total_workers=$(grep -E "Worker [0-9]+:" "$log_file" | wc -l)
    local active_workers=$(grep -E "Worker [0-9]+:" "$log_file" | awk '{print $4}' | awk '$1 > 0' | wc -l)
    local utilization=0
    if [ $total_workers -gt 0 ]; then
        utilization=$(echo "scale=2; $active_workers * 100 / $total_workers" | bc 2>/dev/null || echo "0")
    fi

    # サブタスク数
    local subtasks=$(grep "Subtasks spawned:" "$log_file" | awk '{print $3}' | tr -d ',')

    # TTヒット率
    local tt_hits=$(grep "TT hits:" "$log_file" | awk '{print $3}' | tr -d ',')
    local tt_stores=$(grep "TT stores:" "$log_file" | awk '{print $3}' | tr -d ',')
    local tt_hit_rate=0
    if [ -n "$tt_hits" ] && [ -n "$tt_stores" ] && [ $tt_stores -gt 0 ]; then
        tt_hit_rate=$(echo "scale=2; $tt_hits * 100 / ($tt_hits + $tt_stores)" | bc 2>/dev/null || echo "0")
    fi

    echo "$result,$nodes,$time,$nps,$utilization,$subtasks,$tt_hit_rate"
}

# ========================================
# スケーラビリティ分析
# ========================================
analyze_scalability() {
    local csv_file=$1
    local output_file=$2

    echo "========================================"
    echo "スケーラビリティ分析"
    echo "========================================"

    # Pythonスクリプトで詳細分析
    python3 - "$csv_file" "$output_file" << 'PYTHON_SCRIPT'
import sys
import csv
from collections import defaultdict

csv_file = sys.argv[1]
output_file = sys.argv[2]

# データ読み込み
data = defaultdict(lambda: defaultdict(dict))
with open(csv_file, 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        pos_key = f"{row['Empties']}_{row['FileID']}"
        threads = int(row['Threads'])
        data[pos_key][threads] = {
            'time': float(row['Time']) if row['Time'] else 0,
            'nodes': int(row['Nodes']) if row['Nodes'] else 0,
            'nps': int(row['NPS']) if row['NPS'] else 0,
            'utilization': float(row['WorkerUtilization']) if row['WorkerUtilization'] else 0,
            'subtasks': int(row['Subtasks']) if row['Subtasks'] else 0,
            'result': row['Result']
        }

# 分析結果を出力
with open(output_file, 'w') as f:
    f.write("ポジション,スレッド数,時間(秒),スピードアップ,効率性(%),NPS,Worker稼働率(%),サブタスク数,ステータス\n")

    for pos_key in sorted(data.keys()):
        pos_data = data[pos_key]

        # 1スレッドの結果を基準とする
        if 1 in pos_data and pos_data[1]['time'] > 0:
            base_time = pos_data[1]['time']

            for threads in sorted(pos_data.keys()):
                result = pos_data[threads]
                time = result['time']

                if time > 0:
                    speedup = base_time / time
                    efficiency = (speedup / threads) * 100
                else:
                    speedup = 0
                    efficiency = 0

                status = "OK" if result['result'] in ['WIN', 'LOSE', 'DRAW'] else "FAIL"

                f.write(f"{pos_key},{threads},{time:.3f},{speedup:.2f},{efficiency:.1f},"
                       f"{result['nps']},{result['utilization']:.1f},{result['subtasks']},{status}\n")

print(f"分析完了: {output_file}")
PYTHON_SCRIPT

    if [ $? -eq 0 ]; then
        echo "分析ファイル生成: $output_file"

        # サマリー表示
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "スケーラビリティサマリー"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

        # 平均スピードアップと効率性を計算
        python3 - "$output_file" << 'SUMMARY_SCRIPT'
import sys
import csv
from collections import defaultdict

output_file = sys.argv[1]

stats = defaultdict(lambda: {'speedup': [], 'efficiency': [], 'utilization': []})

with open(output_file, 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        threads = int(row['スレッド数'])
        if row['ステータス'] == 'OK' and threads > 1:
            stats[threads]['speedup'].append(float(row['スピードアップ']))
            stats[threads]['efficiency'].append(float(row['効率性(%)']))
            stats[threads]['utilization'].append(float(row['Worker稼働率(%)']))

print(f"{'スレッド数':>10} {'平均スピードアップ':>18} {'平均効率性(%)':>15} {'平均稼働率(%)':>15}")
print("-" * 60)

for threads in sorted(stats.keys()):
    avg_speedup = sum(stats[threads]['speedup']) / len(stats[threads]['speedup'])
    avg_efficiency = sum(stats[threads]['efficiency']) / len(stats[threads]['efficiency'])
    avg_utilization = sum(stats[threads]['utilization']) / len(stats[threads]['utilization'])

    print(f"{threads:10d} {avg_speedup:18.2f} {avg_efficiency:15.1f} {avg_utilization:15.1f}")

print("━" * 60)
SUMMARY_SCRIPT
    else
        echo "分析失敗"
    fi

    echo ""
}

# ========================================
# グラフ生成用gnuplotスクリプト作成
# ========================================
generate_plot_script() {
    local analysis_file=$1
    local plot_dir=$2

    cat > "$plot_dir/plot_scalability.gnuplot" << 'GNUPLOT_SCRIPT'
set terminal pngcairo size 1600,1200 font "Arial,14"
set datafile separator ","

# スピードアップグラフ
set output 'scalability_speedup.png'
set title "並列化スピードアップ" font "Arial,18"
set xlabel "スレッド数" font "Arial,14"
set ylabel "スピードアップ（倍）" font "Arial,14"
set grid
set logscale x 2
set key left top

# 理想的なスピードアップ（線形）
plot x title "理想（線形）" with lines lw 2 dt 2 lc rgb "gray", \
     'scalability_analysis.csv' skip 1 using 2:4 title "実測" with linespoints lw 2 pt 7 ps 1.5 lc rgb "blue"

# 効率性グラフ
set output 'scalability_efficiency.png'
set title "並列化効率性" font "Arial,18"
set xlabel "スレッド数" font "Arial,14"
set ylabel "効率性（%）" font "Arial,14"
set yrange [0:110]

plot 100 title "理想（100%）" with lines lw 2 dt 2 lc rgb "gray", \
     'scalability_analysis.csv' skip 1 using 2:5 title "実測効率性" with linespoints lw 2 pt 7 ps 1.5 lc rgb "red"

# Worker稼働率グラフ
set output 'scalability_utilization.png'
set title "Worker稼働率" font "Arial,18"
set xlabel "スレッド数" font "Arial,14"
set ylabel "稼働率（%）" font "Arial,14"
set yrange [0:110]

plot 100 title "理想（100%）" with lines lw 2 dt 2 lc rgb "gray", \
     'scalability_analysis.csv' skip 1 using 2:7 title "Worker稼働率" with linespoints lw 2 pt 7 ps 1.5 lc rgb "green"

# 複合グラフ
set output 'scalability_combined.png'
set title "並列化性能の総合評価" font "Arial,18"
set xlabel "スレッド数" font "Arial,14"
set ylabel "スピードアップ（倍）" font "Arial,14"
set y2label "効率性・稼働率（%）" font "Arial,14"
set ytics nomirror
set y2tics
set y2range [0:110]
set key left top

plot x axes x1y1 title "理想スピードアップ" with lines lw 2 dt 2 lc rgb "gray", \
     'scalability_analysis.csv' skip 1 using 2:4 axes x1y1 title "スピードアップ" with linespoints lw 2 pt 7 ps 1.5 lc rgb "blue", \
     'scalability_analysis.csv' skip 1 using 2:5 axes x1y2 title "効率性" with linespoints lw 2 pt 9 ps 1.5 lc rgb "red", \
     'scalability_analysis.csv' skip 1 using 2:7 axes x1y2 title "稼働率" with linespoints lw 2 pt 11 ps 1.5 lc rgb "green"

print "グラフ生成完了"
GNUPLOT_SCRIPT

    echo "gnuplotスクリプト生成: $plot_dir/plot_scalability.gnuplot"
}

# ========================================
# メイン処理
# ========================================
main() {
    echo "========================================"
    echo "並列化スケーラビリティベンチマーク"
    echo "========================================"
    echo "設定:"
    echo "  スレッド数リスト: $THREAD_COUNTS"
    echo "  TTサイズ: ${TT_SIZE_MB}MB"
    echo "  制限時間: ${TIME_LIMIT}秒"
    echo "  テストモード: $TEST_MODE"
    echo "  空きマス範囲: $EMPTIES_START - $EMPTIES_END"
    echo "  ファイル数/空きマス: $FILES_PER_EMPTIES"
    echo ""
    echo "並列化パラメータ:"
    echo "  G=$G_PARAM, D=$D_PARAM, S=$S_PARAM"
    echo "========================================"
    echo ""

    # システム情報表示
    show_system_info

    # ログディレクトリ作成
    LOG_DIR="log/scalability_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$LOG_DIR"
    echo "ログディレクトリ: $LOG_DIR"
    echo ""

    # CSVヘッダー
    RESULTS_CSV="$LOG_DIR/scalability_results.csv"
    echo "Empties,FileID,PosFile,Threads,Result,Nodes,Time,NPS,WorkerUtilization,Subtasks,TTHitRate,Status" > "$RESULTS_CSV"

    # 全テストポジションをリストアップ
    test_positions=()
    for empties in $(seq $EMPTIES_START $EMPTIES_END); do
        empties_padded=$(printf "%02d" $empties)
        for file_id in $(seq 0 $((FILES_PER_EMPTIES - 1))); do
            file_id_padded=$(printf "%03d" $file_id)
            pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"
            if [ -f "$pos_file" ]; then
                test_positions+=("$empties:$file_id:$pos_file")
            fi
        done
    done

    total_positions=${#test_positions[@]}
    echo "テストポジション数: $total_positions"
    echo "スレッド設定数: $(echo $THREAD_COUNTS | wc -w)"
    echo "総テスト実行回数: $((total_positions * $(echo $THREAD_COUNTS | wc -w)))"
    echo ""

    # 進捗カウンター
    test_counter=0
    total_tests=$((total_positions * $(echo $THREAD_COUNTS | wc -w)))
    start_time=$(date +%s)

    # 各ポジションに対して、全スレッド数でテスト
    for pos_data in "${test_positions[@]}"; do
        empties=$(echo "$pos_data" | cut -d':' -f1)
        file_id=$(echo "$pos_data" | cut -d':' -f2)
        pos_file=$(echo "$pos_data" | cut -d':' -f3)

        echo "========================================"
        echo "ポジション: $(basename $pos_file)"
        echo "  空きマス: $empties, ファイルID: $file_id"
        echo "========================================"

        # 各スレッド数でテスト
        for threads in $THREAD_COUNTS; do
            test_counter=$((test_counter + 1))

            echo ""
            echo "▶ テスト $test_counter/$total_tests: ${threads}スレッド"

            empties_padded=$(printf "%02d" $empties)
            file_id_padded=$(printf "%03d" $file_id)
            log_file="$LOG_DIR/empties_${empties_padded}_id_${file_id_padded}_threads_${threads}.log"

            # テスト実行
            test_start=$(date +%s)
            run_single_test "$pos_file" "$threads" "$log_file"
            exit_code=$?
            test_end=$(date +%s)

            # 結果解析
            result_data=$(parse_result "$log_file")
            result=$(echo "$result_data" | cut -d',' -f1)
            nodes=$(echo "$result_data" | cut -d',' -f2)
            time_sec=$(echo "$result_data" | cut -d',' -f3)
            nps=$(echo "$result_data" | cut -d',' -f4)
            utilization=$(echo "$result_data" | cut -d',' -f5)
            subtasks=$(echo "$result_data" | cut -d',' -f6)
            tt_hit_rate=$(echo "$result_data" | cut -d',' -f7)

            # ステータス判定
            if [ $exit_code -eq 124 ]; then
                status="TIMEOUT"
            elif [ "$result" = "WIN" ] || [ "$result" = "LOSE" ] || [ "$result" = "DRAW" ]; then
                status="SOLVED"
            else
                status="UNKNOWN"
            fi

            # CSV出力
            echo "$empties,$file_id,$pos_file,$threads,$result,$nodes,$time_sec,$nps,$utilization,$subtasks,$tt_hit_rate,$status" >> "$RESULTS_CSV"

            # 結果表示
            echo "  結果: $result ($status)"
            echo "  時間: ${time_sec}秒 (実測: $((test_end - test_start))秒)"
            echo "  ノード数: $nodes"
            echo "  NPS: $nps"
            echo "  Worker稼働率: ${utilization}%"
            echo "  サブタスク数: $subtasks"
            echo "  TTヒット率: ${tt_hit_rate}%"

            # 進捗表示
            elapsed=$(($(date +%s) - start_time))
            avg_time_per_test=$((elapsed / test_counter))
            remaining_tests=$((total_tests - test_counter))
            eta=$((remaining_tests * avg_time_per_test))
            echo "  進捗: $test_counter/$total_tests ($(echo "scale=1; $test_counter * 100 / $total_tests" | bc)%) - 推定残り時間: $((eta / 60))分"
        done

        echo ""
    done

    # 最終処理
    end_time=$(date +%s)
    total_elapsed=$((end_time - start_time))

    echo "========================================"
    echo "全テスト完了"
    echo "========================================"
    echo "総実行時間: $total_elapsed 秒 ($(echo "scale=1; $total_elapsed/60" | bc)分)"
    echo ""

    # スケーラビリティ分析
    echo "スケーラビリティ分析を実行中..."
    ANALYSIS_FILE="$LOG_DIR/scalability_analysis.csv"
    analyze_scalability "$RESULTS_CSV" "$ANALYSIS_FILE"

    # グラフ生成用スクリプト作成
    generate_plot_script "$ANALYSIS_FILE" "$LOG_DIR"

    # グラフ生成（gnuplotが利用可能な場合）
    if command -v gnuplot &> /dev/null; then
        echo ""
        echo "グラフを生成中..."
        cd "$LOG_DIR"
        gnuplot plot_scalability.gnuplot 2>/dev/null
        if [ $? -eq 0 ]; then
            echo "✓ グラフ生成完了:"
            echo "  - scalability_speedup.png"
            echo "  - scalability_efficiency.png"
            echo "  - scalability_utilization.png"
            echo "  - scalability_combined.png"
        fi
        cd - > /dev/null
    else
        echo ""
        echo "gnuplotが見つかりません。グラフを生成するには:"
        echo "  cd $LOG_DIR && gnuplot plot_scalability.gnuplot"
    fi

    # 最終サマリー
    cat > "$LOG_DIR/summary.txt" << EOF
======================================
並列化スケーラビリティベンチマーク結果
======================================
実行日時: $(date)
スレッド数リスト: $THREAD_COUNTS
TTサイズ: ${TT_SIZE_MB}MB
制限時間: ${TIME_LIMIT}秒
テストモード: $TEST_MODE

テスト構成:
  ポジション数: $total_positions
  スレッド設定数: $(echo $THREAD_COUNTS | wc -w)
  総テスト実行回数: $total_tests
  総実行時間: $total_elapsed 秒

結果ファイル:
  生データ: $RESULTS_CSV
  分析結果: $ANALYSIS_FILE
  ログディレクトリ: $LOG_DIR

推奨される次のステップ:
  1. 分析結果を確認: cat $ANALYSIS_FILE
  2. グラフを表示: eog $LOG_DIR/*.png
  3. 詳細ログを確認: ls $LOG_DIR/*.log
======================================
EOF

    echo ""
    echo "結果ファイル:"
    echo "  生データ: $RESULTS_CSV"
    echo "  分析結果: $ANALYSIS_FILE"
    echo "  サマリー: $LOG_DIR/summary.txt"
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "ベンチマーク完了！"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

# 実行
main "$@"
