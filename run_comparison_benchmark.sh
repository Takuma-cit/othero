#!/bin/bash
#
# DeepPN (逐次版) vs Work-Stealing並列版 比較ベンチマークスクリプト
# 
# 目的: 論文用の性能比較データを取得
# 比較対象:
#   1. Deep_Pns.c (単一スレッド、DeepPN R=1)
#   2. othello_endgame_solver_hybrid_check_tthit_fixed.c (並列版)
#   3. (オプション) othello_endgame_solver_workstealing.c (Work-Stealing版)
#

# ========================================
# 設定
# ========================================
EVAL_FILE="eval/eval.dat"
POS_DIR="test_positions"

# タイムアウト設定（秒）
TIME_LIMIT_SEQUENTIAL=300    # 逐次版: 5分
TIME_LIMIT_PARALLEL=300      # 並列版: 5分

# 並列版のスレッド数（複数設定でスケーラビリティ測定）
THREAD_COUNTS="1 2 4 8 16"

# テスト対象の空きマス数
EMPTIES_LIST="8 10 12 14 16"

# 各空きマス数でテストするファイル数
FILES_PER_EMPTIES=3

# 出力ディレクトリ
LOG_DIR="comparison_benchmark_$(date +%Y%m%d_%H%M%S)"

# ソルバーバイナリ名
SOLVER_SEQUENTIAL="./deep_pns_solver"
SOLVER_PARALLEL="./othello_solver_parallel"
SOLVER_WORKSTEALING="./othello_solver_ws"

# ========================================
# コマンドライン引数処理
# ========================================
show_usage() {
    echo "使用方法: $0 [オプション]"
    echo ""
    echo "オプション:"
    echo "  -t <秒>       タイムアウト時間 (デフォルト: 300)"
    echo "  -e <リスト>   空きマス数リスト (デフォルト: \"8 10 12 14 16\")"
    echo "  -n <数>       各空きマスのテストファイル数 (デフォルト: 3)"
    echo "  -p <リスト>   スレッド数リスト (デフォルト: \"1 2 4 8 16\")"
    echo "  -q            クイックモード (空きマス10-14, 各2ファイル)"
    echo "  -f            フルモード (空きマス8-20, 各5ファイル)"
    echo "  -h            このヘルプを表示"
    echo ""
    echo "例:"
    echo "  $0 -q                    # クイックテスト"
    echo "  $0 -e \"10 12\" -n 5      # 空きマス10,12で各5ファイル"
    echo "  $0 -p \"1 4 8 16 32\"     # スレッド1,4,8,16,32で比較"
}

while getopts "t:e:n:p:qfh" opt; do
    case $opt in
        t) TIME_LIMIT_SEQUENTIAL=$OPTARG; TIME_LIMIT_PARALLEL=$OPTARG ;;
        e) EMPTIES_LIST="$OPTARG" ;;
        n) FILES_PER_EMPTIES=$OPTARG ;;
        p) THREAD_COUNTS="$OPTARG" ;;
        q) EMPTIES_LIST="10 12 14"; FILES_PER_EMPTIES=2 ;;
        f) EMPTIES_LIST="8 10 12 14 16 18 20"; FILES_PER_EMPTIES=5 ;;
        h) show_usage; exit 0 ;;
        *) show_usage; exit 1 ;;
    esac
done

# ========================================
# ソルバービルド
# ========================================
build_solvers() {
    echo "========================================"
    echo "ソルバーのビルド"
    echo "========================================"
    
    # Deep_Pns.c（逐次版）のビルド
    echo "[1/3] Deep_Pns.c (逐次版DeepPN) をビルド中..."
    
    # Deep_Pns.cは盤面がハードコードされているため、
    # ファイル入力対応版にラッパーを作成
    cat > deep_pns_wrapper.c << 'WRAPPER_EOF'
// Deep_Pns.cをファイル入力対応にするラッパー
// コンパイル: gcc -O3 -o deep_pns_solver deep_pns_wrapper.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>

typedef uint64_t bitboard;

#define WIN 1
#define LOSE -1
#define DRAW 0
#define UNKNOWN -2
#define BLACK 1
#define WHITE -1
#define INF 10000000
#define UNDEFINED 0
#define R 1

const int directions[8] = {-1,1,-9,-7,7,9,-8,8};
#define rightleft_HIDE_BIT 0x7E7E7E7E7E7E7E7E
#define topbottom_HIDE_BIT 0x00FFFFFFFFFFFF00

typedef struct node {
    int num_node;
    bitboard black;
    bitboard white;
    int color;
    int depth;
    float deep;
    int proof;
    int disproof;
    float dpn;
    struct node *parent;
    struct node *child;
    struct node *next;
} node_t;

typedef struct hash {
    int num_node;
    bitboard black;
    bitboard white;
    int color;
    int proof;
    int disproof;
    float dpn;
    struct hash *next;
} hash_t;

#define HASH_SIZE 999983

hash_t *hash_table[HASH_SIZE];

int node_num = 0;
int search_node_num = 0;
int store_num = 0;
int use_hash_num = 0;

// 前方宣言
int pns(bitboard black, bitboard white, int color, int depth);
void init_hash();
int count_bit(bitboard b);

// ファイルからポジション読み込み
int load_position(const char *filename, bitboard *black, bitboard *white, int *color) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("ファイルオープンエラー");
        return -1;
    }
    
    char board_str[128];
    char turn_str[128];
    
    if (fgets(board_str, sizeof(board_str), f) == NULL) {
        fclose(f);
        return -1;
    }
    if (fgets(turn_str, sizeof(turn_str), f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    
    *black = 0;
    *white = 0;
    for (int i = 0; i < 64 && board_str[i] != '\0'; i++) {
        if (board_str[i] == 'X' || board_str[i] == 'x' || board_str[i] == '*') {
            *black |= (1ULL << i);
        } else if (board_str[i] == 'O' || board_str[i] == 'o') {
            *white |= (1ULL << i);
        }
    }
    
    *color = (turn_str[0] == 'B' || turn_str[0] == 'b') ? BLACK : WHITE;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "使用方法: %s <position_file> [time_limit]\n", argv[0]);
        return 1;
    }
    
    double time_limit = (argc > 2) ? atof(argv[2]) : 300.0;
    
    bitboard black, white;
    int color;
    
    if (load_position(argv[1], &black, &white, &color) != 0) {
        fprintf(stderr, "ポジション読み込みエラー\n");
        return 1;
    }
    
    init_hash();
    
    int depth = 64 - count_bit(black) - count_bit(white);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    int result = pns(black, white, color, depth);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) * 1e-9;
    
    // 結果出力（パース用フォーマット）
    printf("Result: %s\n", result == WIN ? "WIN" : (result == LOSE ? "LOSE" : (result == DRAW ? "DRAW" : "UNKNOWN")));
    printf("Time: %.6f\n", elapsed);
    printf("Nodes: %d\n", search_node_num);
    printf("NPS: %.0f\n", search_node_num / elapsed);
    printf("TT_stores: %d\n", store_num);
    printf("TT_hits: %d\n", use_hash_num);
    
    return 0;
}

// count_bit関数
int count_bit(bitboard b) {
    return __builtin_popcountll(b);
}

// init_hash関数
void init_hash() {
    for (int i = 0; i < HASH_SIZE; i++) {
        hash_table[i] = NULL;
    }
}
WRAPPER_EOF

    # Deep_Pns.cから必要な関数を抽出してビルド
    # （元のmain関数を除外）
    if [ -f "Deep_Pns.c" ]; then
        # Deep_Pns.cのmain以外の部分を抽出
        sed '/^int main/,/^}$/d' Deep_Pns.c > deep_pns_functions.c
        
        # ラッパーと結合してビルド
        gcc -O3 -march=native -o "$SOLVER_SEQUENTIAL" \
            deep_pns_wrapper.c deep_pns_functions.c -lm 2>/dev/null
        
        if [ $? -eq 0 ]; then
            echo "  ✓ $SOLVER_SEQUENTIAL ビルド成功"
        else
            echo "  ✗ $SOLVER_SEQUENTIAL ビルド失敗（Deep_Pns.cのビルドをスキップ）"
            SOLVER_SEQUENTIAL=""
        fi
        
        rm -f deep_pns_functions.c deep_pns_wrapper.c
    else
        echo "  ⚠ Deep_Pns.c が見つかりません（逐次版テストをスキップ）"
        SOLVER_SEQUENTIAL=""
    fi
    
    # 並列版（Hybrid）のビルド
    echo "[2/3] Hybrid並列版をビルド中..."
    if [ -f "othello_endgame_solver_hybrid_check_tthit_fixed.c" ]; then
        gcc -O3 -march=native -pthread \
            -DSTANDALONE_MAIN \
            -DMAX_THREADS=128 \
            -o "$SOLVER_PARALLEL" \
            othello_endgame_solver_hybrid_check_tthit_fixed.c \
            -lm -lpthread 2>/dev/null
        
        if [ $? -eq 0 ]; then
            echo "  ✓ $SOLVER_PARALLEL ビルド成功"
        else
            echo "  ✗ $SOLVER_PARALLEL ビルド失敗"
            SOLVER_PARALLEL=""
        fi
    else
        echo "  ⚠ othello_endgame_solver_hybrid_check_tthit_fixed.c が見つかりません"
        SOLVER_PARALLEL=""
    fi
    
    # Work-Stealing版のビルド
    echo "[3/3] Work-Stealing版をビルド中..."
    if [ -f "othello_endgame_solver_workstealing.c" ]; then
        gcc -O3 -march=native -pthread \
            -DSTANDALONE_MAIN \
            -DMAX_THREADS=128 \
            -o "$SOLVER_WORKSTEALING" \
            othello_endgame_solver_workstealing.c \
            -lm -lpthread 2>/dev/null
        
        if [ $? -eq 0 ]; then
            echo "  ✓ $SOLVER_WORKSTEALING ビルド成功"
        else
            echo "  ✗ $SOLVER_WORKSTEALING ビルド失敗"
            SOLVER_WORKSTEALING=""
        fi
    else
        echo "  ⚠ othello_endgame_solver_workstealing.c が見つかりません"
        SOLVER_WORKSTEALING=""
    fi
    
    echo ""
}

# ========================================
# 単一テスト実行
# ========================================
run_test_sequential() {
    local pos_file=$1
    local log_file=$2
    
    if [ -z "$SOLVER_SEQUENTIAL" ]; then
        echo "SKIP" > "$log_file"
        return 1
    fi
    
    timeout $TIME_LIMIT_SEQUENTIAL "$SOLVER_SEQUENTIAL" "$pos_file" $TIME_LIMIT_SEQUENTIAL 2>&1 > "$log_file"
    return $?
}

run_test_parallel() {
    local solver=$1
    local pos_file=$2
    local threads=$3
    local log_file=$4
    
    if [ -z "$solver" ] || [ ! -x "$solver" ]; then
        echo "SKIP" > "$log_file"
        return 1
    fi
    
    timeout $TIME_LIMIT_PARALLEL "$solver" "$pos_file" $threads $TIME_LIMIT_PARALLEL "$EVAL_FILE" -v 2>&1 > "$log_file"
    return $?
}

# ========================================
# 結果パース
# ========================================
parse_result() {
    local log_file=$1
    
    if grep -q "SKIP" "$log_file" 2>/dev/null; then
        echo "SKIP,0,0,0"
        return
    fi
    
    local result=$(grep -E "^Result:" "$log_file" 2>/dev/null | awk '{print $2}')
    local time=$(grep -E "^Time:|^Total:" "$log_file" 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
    local nodes=$(grep -E "^Nodes:|^Total:" "$log_file" 2>/dev/null | head -1 | grep -oE '[0-9]+' | head -1)
    local nps=$(grep -E "^NPS:|NPS" "$log_file" 2>/dev/null | grep -oE '[0-9]+' | head -1)
    
    # デフォルト値
    result=${result:-UNKNOWN}
    time=${time:-0}
    nodes=${nodes:-0}
    nps=${nps:-0}
    
    echo "$result,$time,$nodes,$nps"
}

# ========================================
# メイン処理
# ========================================
main() {
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  DeepPN vs Work-Stealing並列版 比較ベンチマーク              ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo ""
    echo "設定:"
    echo "  空きマス数: $EMPTIES_LIST"
    echo "  各空きマスのファイル数: $FILES_PER_EMPTIES"
    echo "  スレッド数: $THREAD_COUNTS"
    echo "  タイムアウト(逐次): ${TIME_LIMIT_SEQUENTIAL}秒"
    echo "  タイムアウト(並列): ${TIME_LIMIT_PARALLEL}秒"
    echo ""
    
    # ログディレクトリ作成
    mkdir -p "$LOG_DIR"
    echo "ログディレクトリ: $LOG_DIR"
    echo ""
    
    # ソルバービルド
    build_solvers
    
    # 利用可能なソルバー確認
    available_solvers=""
    if [ -n "$SOLVER_SEQUENTIAL" ] && [ -x "$SOLVER_SEQUENTIAL" ]; then
        available_solvers="$available_solvers Sequential"
    fi
    if [ -n "$SOLVER_PARALLEL" ] && [ -x "$SOLVER_PARALLEL" ]; then
        available_solvers="$available_solvers Hybrid"
    fi
    if [ -n "$SOLVER_WORKSTEALING" ] && [ -x "$SOLVER_WORKSTEALING" ]; then
        available_solvers="$available_solvers WorkStealing"
    fi
    
    if [ -z "$available_solvers" ]; then
        echo "エラー: 利用可能なソルバーがありません"
        exit 1
    fi
    
    echo "利用可能なソルバー:$available_solvers"
    echo ""
    
    # CSVヘッダー作成
    SUMMARY_CSV="$LOG_DIR/comparison_summary.csv"
    echo "Empties,FileID,PosFile,Solver,Threads,Result,Time_sec,Nodes,NPS,Speedup" > "$SUMMARY_CSV"
    
    # 詳細比較用CSV
    DETAIL_CSV="$LOG_DIR/detailed_comparison.csv"
    echo "Empties,FileID,Sequential_Time,Sequential_Nodes,Sequential_NPS" > "$DETAIL_CSV"
    for threads in $THREAD_COUNTS; do
        echo -n ",Parallel_${threads}t_Time,Parallel_${threads}t_Nodes,Parallel_${threads}t_NPS,Parallel_${threads}t_Speedup" >> "$DETAIL_CSV"
    done
    echo "" >> "$DETAIL_CSV"
    
    # テスト実行
    test_count=0
    
    for empties in $EMPTIES_LIST; do
        empties_padded=$(printf "%02d" $empties)
        
        for file_id in $(seq 0 $((FILES_PER_EMPTIES - 1))); do
            file_id_padded=$(printf "%03d" $file_id)
            pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"
            
            if [ ! -f "$pos_file" ]; then
                # 代替パス試行
                pos_file="test_positions/empties_${empties_padded}_id_${file_id_padded}.pos"
                if [ ! -f "$pos_file" ]; then
                    continue
                fi
            fi
            
            test_count=$((test_count + 1))
            
            echo "════════════════════════════════════════════════════════════════"
            echo "テスト #$test_count: $pos_file (空きマス: $empties)"
            echo "════════════════════════════════════════════════════════════════"
            
            # 詳細CSV用の行データ
            detail_row="$empties,$file_id"
            sequential_time=0
            
            # ──────────────────────────────────────────────────
            # 逐次版テスト
            # ──────────────────────────────────────────────────
            if [ -n "$SOLVER_SEQUENTIAL" ] && [ -x "$SOLVER_SEQUENTIAL" ]; then
                echo ""
                echo "▶ [Sequential] DeepPN (R=1) 実行中..."
                log_seq="$LOG_DIR/seq_${empties_padded}_${file_id_padded}.log"
                
                run_test_sequential "$pos_file" "$log_seq"
                exit_code=$?
                
                result_data=$(parse_result "$log_seq")
                seq_result=$(echo "$result_data" | cut -d',' -f1)
                seq_time=$(echo "$result_data" | cut -d',' -f2)
                seq_nodes=$(echo "$result_data" | cut -d',' -f3)
                seq_nps=$(echo "$result_data" | cut -d',' -f4)
                
                sequential_time=$seq_time
                
                if [ $exit_code -eq 124 ]; then
                    echo "  結果: TIMEOUT"
                    seq_result="TIMEOUT"
                else
                    echo "  結果: $seq_result"
                    echo "  時間: ${seq_time}秒"
                    echo "  ノード数: $seq_nodes"
                    echo "  NPS: $seq_nps"
                fi
                
                echo "$empties,$file_id,$pos_file,Sequential,1,$seq_result,$seq_time,$seq_nodes,$seq_nps,1.0" >> "$SUMMARY_CSV"
                detail_row="$detail_row,$seq_time,$seq_nodes,$seq_nps"
            else
                detail_row="$detail_row,,,,"
            fi
            
            # ──────────────────────────────────────────────────
            # 並列版テスト（各スレッド数で）
            # ──────────────────────────────────────────────────
            for threads in $THREAD_COUNTS; do
                # Hybrid版
                if [ -n "$SOLVER_PARALLEL" ] && [ -x "$SOLVER_PARALLEL" ]; then
                    echo ""
                    echo "▶ [Hybrid] ${threads}スレッド 実行中..."
                    log_par="$LOG_DIR/hybrid_${empties_padded}_${file_id_padded}_t${threads}.log"
                    
                    run_test_parallel "$SOLVER_PARALLEL" "$pos_file" $threads "$log_par"
                    exit_code=$?
                    
                    result_data=$(parse_result "$log_par")
                    par_result=$(echo "$result_data" | cut -d',' -f1)
                    par_time=$(echo "$result_data" | cut -d',' -f2)
                    par_nodes=$(echo "$result_data" | cut -d',' -f3)
                    par_nps=$(echo "$result_data" | cut -d',' -f4)
                    
                    # スピードアップ計算
                    speedup="N/A"
                    if [ "$sequential_time" != "0" ] && [ "$par_time" != "0" ]; then
                        speedup=$(echo "scale=2; $sequential_time / $par_time" | bc 2>/dev/null || echo "N/A")
                    fi
                    
                    if [ $exit_code -eq 124 ]; then
                        echo "  結果: TIMEOUT"
                        par_result="TIMEOUT"
                        speedup="N/A"
                    else
                        echo "  結果: $par_result"
                        echo "  時間: ${par_time}秒"
                        echo "  ノード数: $par_nodes"
                        echo "  NPS: $par_nps"
                        echo "  スピードアップ: ${speedup}x"
                    fi
                    
                    echo "$empties,$file_id,$pos_file,Hybrid,$threads,$par_result,$par_time,$par_nodes,$par_nps,$speedup" >> "$SUMMARY_CSV"
                    detail_row="$detail_row,$par_time,$par_nodes,$par_nps,$speedup"
                else
                    detail_row="$detail_row,,,,"
                fi
            done
            
            echo "$detail_row" >> "$DETAIL_CSV"
            echo ""
        done
    done
    
    # ========================================
    # サマリー生成
    # ========================================
    echo ""
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  ベンチマーク完了                                            ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo ""
    echo "総テスト数: $test_count"
    echo ""
    echo "結果ファイル:"
    echo "  詳細CSV: $SUMMARY_CSV"
    echo "  比較CSV: $DETAIL_CSV"
    echo "  ログ: $LOG_DIR/"
    echo ""
    
    # 統計サマリー生成
    STATS_FILE="$LOG_DIR/statistics_summary.txt"
    cat > "$STATS_FILE" << EOF
═══════════════════════════════════════════════════════════════════════
DeepPN vs Work-Stealing並列版 比較ベンチマーク結果
═══════════════════════════════════════════════════════════════════════
実行日時: $(date)
テスト数: $test_count
空きマス数: $EMPTIES_LIST
スレッド数: $THREAD_COUNTS

───────────────────────────────────────────────────────────────────────
結果集計
───────────────────────────────────────────────────────────────────────
EOF

    # 各ソルバーの統計
    if [ -f "$SUMMARY_CSV" ]; then
        echo "" >> "$STATS_FILE"
        echo "ソルバー別統計:" >> "$STATS_FILE"
        
        # Sequential
        seq_count=$(grep -c ",Sequential," "$SUMMARY_CSV" 2>/dev/null || echo "0")
        seq_solved=$(grep ",Sequential," "$SUMMARY_CSV" 2>/dev/null | grep -cE ",WIN,|,LOSE,|,DRAW," || echo "0")
        echo "  Sequential: $seq_solved / $seq_count 解決" >> "$STATS_FILE"
        
        # 各スレッド数での並列版
        for threads in $THREAD_COUNTS; do
            par_count=$(grep ",Hybrid,$threads," "$SUMMARY_CSV" 2>/dev/null | wc -l || echo "0")
            par_solved=$(grep ",Hybrid,$threads," "$SUMMARY_CSV" 2>/dev/null | grep -cE ",WIN,|,LOSE,|,DRAW," || echo "0")
            
            # 平均スピードアップ
            avg_speedup=$(grep ",Hybrid,$threads," "$SUMMARY_CSV" 2>/dev/null | \
                         awk -F',' '{if($10 != "N/A" && $10 > 0) {sum+=$10; count++}} END {if(count>0) printf "%.2f", sum/count; else print "N/A"}')
            
            echo "  Hybrid (${threads}t): $par_solved / $par_count 解決, 平均スピードアップ: ${avg_speedup}x" >> "$STATS_FILE"
        done
    fi
    
    echo "" >> "$STATS_FILE"
    echo "═══════════════════════════════════════════════════════════════════════" >> "$STATS_FILE"
    
    cat "$STATS_FILE"
    
    echo ""
    echo "詳細統計: $STATS_FILE"
}

# 実行
main "$@"
