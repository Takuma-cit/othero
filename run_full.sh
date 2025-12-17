#!/bin/bash
#
# オセロ終盤ソルバー ベンチマークスクリプト
# 768コア・2TB環境専用 ローカルヒープ保持スポーン効果測定版
#
# 対象環境:
#   CPU: AMD EPYC 9965 192-Core Processor × 2 (768論理コア)
#   RAM: 2.2TB
#   NUMA: 2ノード
#
# 主要機能:
#   [1] ROOT SPLIT: ルートタスク即座分割
#   [2] MID-SEARCH SPAWN: 探索中スポーン
#   [3] DYNAMIC PARAMS: 動的パラメータ調整
#   [4] EARLY SPAWN: 探索前早期スポーン
#   [5] LOCAL-HEAP-FILL: ローカルヒープ保持スポーン
#
# 使用法:
#   tsp ./run_full.sh [TIME_LIMIT] [TEST_MODE]
#
# テストモード:
#   quick  - 空きマス20-25, 5局面/空きマス, デフォルト1時間
#   full   - 空きマス1-60, 10局面/空きマス, デフォルト4時間
#   normal - 空きマス15-30, 10局面/空きマス, デフォルト1時間
#
# 例:
#   tsp ./run_full.sh "" full         # fullモード、デフォルト4時間
#   tsp ./run_full.sh 7200.0 full     # fullモード、2時間
#   tsp ./run_full.sh "" quick        # quickモード、デフォルト1時間

# ========================================
# 768コア・2TB環境固定設定
# ========================================
THREADS=768
TT_SIZE_MB=2048000                 # 2TB
EVAL_FILE="eval/eval.dat"
POS_DIR="test_positions"
SOURCE_FILE="othello_endgame_solver_hybrid_check_tthit_fixed.c"
SOLVER="./othello_solver_768core"

TEST_MODE=${2:-quick}

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
# 768コア最適化パラメータ
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
# LOCAL-HEAP-FILL: ローカルヒープ < 16 で全制限解除
# これにより768コア全てにタスクが行き渡る
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
G_PARAM=15                         # 最大世代（実質的にはLOCAL-HEAP-FILLで緩和）
D_PARAM=3                          # 最小深さ
S_PARAM=99999                      # スポーン制限（実質無制限）

# HYBRID設定（768コア用）
CHUNK_SIZE=16
SHARED_ARRAY_SIZE=262144           # 256K tasks
LOCAL_HEAP_SIZE=8192               # 8K tasks per worker
GLOBAL_QUEUE_SIZE=65536            # 64K chunks

# テスト範囲と制限時間（モード別）
case "$TEST_MODE" in
    quick)
        EMPTIES_START=${EMPTIES_START:-20}
        EMPTIES_END=${EMPTIES_END:-25}
        FILES_PER_EMPTIES=${FILES_PER_EMPTIES:-5}
        DEFAULT_TIME_LIMIT=3600.0      # 1時間
        ;;
    full)
        EMPTIES_START=${EMPTIES_START:-1}
        EMPTIES_END=${EMPTIES_END:-60}
        FILES_PER_EMPTIES=${FILES_PER_EMPTIES:-10}
        DEFAULT_TIME_LIMIT=14400.0     # 4時間
        ;;
    *)  # normal
        EMPTIES_START=${EMPTIES_START:-15}
        EMPTIES_END=${EMPTIES_END:-30}
        FILES_PER_EMPTIES=${FILES_PER_EMPTIES:-10}
        DEFAULT_TIME_LIMIT=3600.0      # 1時間
        ;;
esac

# 制限時間: コマンドライン引数 > モード別デフォルト
TIME_LIMIT=${1:-$DEFAULT_TIME_LIMIT}

# ========================================
# ソルバービルド（AMD EPYC 9965最適化）
# ========================================
build_solver() {
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║  ソルバービルド（768コア・2TB環境専用）                    ║"
    echo "╠════════════════════════════════════════════════════════════╣"
    echo "║  ソース: $SOURCE_FILE"
    echo "║  スレッド数: $THREADS"
    echo "║  TTサイズ: ${TT_SIZE_MB}MB (2TB)"
    echo "║  パラメータ: G=$G_PARAM, D=$D_PARAM, S=$S_PARAM"
    echo "║  アーキテクチャ: AMD EPYC 9965 (znver4, AVX512)"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""

    if [ ! -f "$SOURCE_FILE" ]; then
        echo "エラー: ソースファイルが見つかりません: $SOURCE_FILE"
        exit 1
    fi

    # 機能チェック
    echo "機能チェック:"
    local_heap_fill=$(grep -c "local_heap_needs_fill\|LOCAL-HEAP-FILL" "$SOURCE_FILE" 2>/dev/null || echo "0")
    early_spawn=$(grep -c "EARLY SPAWN" "$SOURCE_FILE" 2>/dev/null || echo "0")
    dynamic_params=$(grep -c "DYNAMIC PARAMS\|idle_rate" "$SOURCE_FILE" 2>/dev/null || echo "0")
    root_split=$(grep -c "ROOT SPLIT" "$SOURCE_FILE" 2>/dev/null || echo "0")

    echo "  [1] ROOT SPLIT:      $([ $root_split -gt 0 ] && echo '✓ 有効' || echo '✗ 無効')"
    echo "  [2] MID-SEARCH:      ✓ 有効"
    echo "  [3] DYNAMIC PARAMS:  $([ $dynamic_params -gt 0 ] && echo '✓ 有効' || echo '✗ 無効')"
    echo "  [4] EARLY SPAWN:     $([ $early_spawn -gt 0 ] && echo '✓ 有効' || echo '✗ 無効')"
    echo "  [5] LOCAL-HEAP-FILL: $([ $local_heap_fill -gt 0 ] && echo '✓ 有効' || echo '✗ 無効')"
    echo ""

    # AMD EPYC 9965向け最適化ビルド
    gcc -O3 -march=znver4 -mtune=znver4 \
        -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx512cd \
        -mprefer-vector-width=512 \
        -flto -ffast-math \
        -DSTANDALONE_MAIN \
        -DMAX_THREADS=1024 \
        -DTT_SIZE_MB=$TT_SIZE_MB \
        -DCHUNK_SIZE=$CHUNK_SIZE \
        -o "$SOLVER" "$SOURCE_FILE" \
        -lpthread -lm 2>&1

    if [ $? -ne 0 ]; then
        echo "ビルド失敗"
        exit 1
    fi

    echo "ビルド完了: $SOLVER ($(ls -lh $SOLVER | awk '{print $5}'))"
    echo ""
}

# ========================================
# 結果解析（効果測定用）
# ========================================
# 数値をサニタイズ（改行・非数値文字を除去）
sanitize_num() {
    echo "$1" | tr -d '\n\r' | sed 's/[^0-9.]//g' | head -c 20
}

parse_result() {
    local log_file=$1

    # ファイルが存在しない、または空の場合
    if [ ! -s "$log_file" ]; then
        echo "CRASHED,0,0,0,0,0,0,0,0,0,0"
        return
    fi

    # 基本情報
    local result=$(grep "^Result:" "$log_file" 2>/dev/null | awk '{print $2}' | head -1 | tr -d '\n\r')
    local total_line=$(grep "^Total:" "$log_file" 2>/dev/null | head -1)
    local nodes=$(echo "$total_line" | awk '{print $2}' | tr -d '\n\r,')
    local time_sec=$(echo "$total_line" | awk '{print $5}' | tr -d '\n\r')
    local nps=$(echo "$total_line" | sed 's/.*(\([0-9]*\) NPS).*/\1/' 2>/dev/null | tr -d '\n\r')

    # Worker稼働率（0以外のノードを処理したワーカー数）
    local active_workers=$(grep -E "^.*Worker [0-9]+:" "$log_file" 2>/dev/null | \
        awk '{gsub(/,/,"",$3); if($3+0 > 0) count++} END {print count+0}')
    local utilization=0
    if [ "$active_workers" -gt 0 ] 2>/dev/null; then
        utilization=$(echo "scale=1; $active_workers * 100 / $THREADS" | bc 2>/dev/null || echo "0")
    fi

    # サブタスク数
    local subtasks=$(grep "Subtasks spawned:" "$log_file" 2>/dev/null | awk '{print $3}' | tr -d ',\n\r' | head -1)

    # 各機能の発動回数
    local root_splits=$(grep -c "ROOT SPLIT.*spawned" "$log_file" 2>/dev/null || echo "0")
    local mid_spawns=$(grep -c "MID-SEARCH\|periodic spawn" "$log_file" 2>/dev/null || echo "0")
    local dynamic_params=$(grep -c "DYNAMIC PARAMS" "$log_file" 2>/dev/null || echo "0")
    local early_spawns=$(grep -c "EARLY SPAWN" "$log_file" 2>/dev/null || echo "0")
    local local_heap_fill=$(grep -c "LOCAL-HEAP-FILL\|local_fill=YES" "$log_file" 2>/dev/null || echo "0")

    # デフォルト値設定とサニタイズ
    result=${result:-UNKNOWN}
    nodes=$(sanitize_num "${nodes:-0}")
    time_sec=$(sanitize_num "${time_sec:-0}")
    nps=$(sanitize_num "${nps:-0}")
    utilization=$(sanitize_num "${utilization:-0}")
    subtasks=$(sanitize_num "${subtasks:-0}")
    root_splits=$(sanitize_num "${root_splits:-0}")
    mid_spawns=$(sanitize_num "${mid_spawns:-0}")
    dynamic_params=$(sanitize_num "${dynamic_params:-0}")
    early_spawns=$(sanitize_num "${early_spawns:-0}")
    local_heap_fill=$(sanitize_num "${local_heap_fill:-0}")

    # 空文字列を0に
    [ -z "$nodes" ] && nodes=0
    [ -z "$time_sec" ] && time_sec=0
    [ -z "$nps" ] && nps=0
    [ -z "$utilization" ] && utilization=0
    [ -z "$subtasks" ] && subtasks=0
    [ -z "$root_splits" ] && root_splits=0
    [ -z "$mid_spawns" ] && mid_spawns=0
    [ -z "$dynamic_params" ] && dynamic_params=0
    [ -z "$early_spawns" ] && early_spawns=0
    [ -z "$local_heap_fill" ] && local_heap_fill=0

    echo "$result,$nodes,$time_sec,$nps,$utilization,$subtasks,$root_splits,$mid_spawns,$dynamic_params,$early_spawns,$local_heap_fill"
}

# ========================================
# メイン処理
# ========================================
main() {
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║  オセロ終盤ソルバー ベンチマーク                           ║"
    echo "║  【768コア・2TB環境専用】                                  ║"
    echo "║  ローカルヒープ保持スポーン効果測定版                      ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""
    echo "環境情報:"
    echo "  CPU: AMD EPYC 9965 192-Core × 2 (768論理コア)"
    echo "  RAM: 2.2TB"
    echo "  NUMA: 2ノード"
    echo ""
    echo "設定:"
    echo "  スレッド数: $THREADS"
    echo "  TTサイズ: ${TT_SIZE_MB}MB (2TB)"
    echo "  制限時間: ${TIME_LIMIT}秒"
    echo "  テストモード: $TEST_MODE"
    echo "  空きマス範囲: $EMPTIES_START - $EMPTIES_END"
    echo "  ファイル数/空きマス: $FILES_PER_EMPTIES"
    echo ""
    echo "並列化パラメータ:"
    echo "  G=$G_PARAM (最大世代)"
    echo "  D=$D_PARAM (最小深さ)"
    echo "  S=$S_PARAM (スポーン制限)"
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "【LOCAL-HEAP-FILL機能】"
    echo "  ローカルヒープ < CHUNK_SIZE(16) なら全制限解除"
    echo "  → G=999, S=999, D=2 で無制限スポーン"
    echo "  → 768コア全てにタスクが行き渡る"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""

    # メモリ確認
    AVAILABLE_MEM_GB=$(free -g 2>/dev/null | grep "^Mem:" | awk '{print $7}')
    echo "利用可能メモリ: ${AVAILABLE_MEM_GB}GB"
    if [ "$AVAILABLE_MEM_GB" -lt 2000 ] 2>/dev/null; then
        echo "⚠️  警告: メモリが2TB未満です。TTサイズを調整してください。"
    fi
    echo ""

    # ソルバービルド
    build_solver

    # ログディレクトリ作成
    LOG_DIR="log/benchmark_768core_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$LOG_DIR"
    echo "ログディレクトリ: $LOG_DIR"
    echo ""

    # CSVヘッダー
    SUMMARY_CSV="$LOG_DIR/benchmark_summary.csv"
    echo "Empties,FileID,PosFile,Result,Nodes,Time,NPS,WorkerUtilization,Subtasks,RootSplits,MidSpawns,DynamicParams,EarlySpawns,LocalHeapFill,Status" > "$SUMMARY_CSV"

    # 統計変数
    total_tests=0
    solved_tests=0
    timeout_tests=0
    crashed_tests=0
    total_nodes=0
    total_subtasks=0
    total_root_splits=0
    total_mid_spawns=0
    total_dynamic_params=0
    total_early_spawns=0
    total_local_heap_fill=0
    sum_utilization=0
    start_time=$(date +%s)

    # テスト実行
    for empties in $(seq $EMPTIES_START $EMPTIES_END); do
        empties_padded=$(printf "%02d" $empties)

        for file_id in $(seq 0 $((FILES_PER_EMPTIES - 1))); do
            file_id_padded=$(printf "%03d" $file_id)
            pos_file="$POS_DIR/empties_${empties_padded}_id_${file_id_padded}.pos"

            if [ ! -f "$pos_file" ]; then
                pos_file="experiments/test_positions/empties_${empties_padded}_id_${file_id_padded}.pos"
                if [ ! -f "$pos_file" ]; then
                    continue
                fi
            fi

            total_tests=$((total_tests + 1))

            echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
            echo "テスト $total_tests: 空きマス=$empties, ID=$file_id"
            echo "ファイル: $pos_file"
            echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

            log_file="$LOG_DIR/empties_${empties_padded}_id_${file_id_padded}.log"

            # NUMA最適化実行
            numactl --interleave=all \
                timeout ${TIME_LIMIT%.*} \
                "$SOLVER" "$pos_file" $THREADS $TIME_LIMIT "$EVAL_FILE" \
                -G $G_PARAM -D $D_PARAM -S $S_PARAM -v 2>&1 | tee "$log_file"

            exit_code=${PIPESTATUS[0]}

            # 結果解析
            result_data=$(parse_result "$log_file")
            result=$(echo "$result_data" | cut -d',' -f1)
            nodes=$(echo "$result_data" | cut -d',' -f2)
            time_sec=$(echo "$result_data" | cut -d',' -f3)
            nps=$(echo "$result_data" | cut -d',' -f4)
            utilization=$(echo "$result_data" | cut -d',' -f5)
            subtasks=$(echo "$result_data" | cut -d',' -f6)
            root_splits=$(echo "$result_data" | cut -d',' -f7)
            mid_spawns=$(echo "$result_data" | cut -d',' -f8)
            dynamic_params=$(echo "$result_data" | cut -d',' -f9)
            early_spawns=$(echo "$result_data" | cut -d',' -f10)
            local_heap_fill=$(echo "$result_data" | cut -d',' -f11)

            # ステータス判定
            if [ $exit_code -eq 124 ]; then
                status="TIMEOUT"
                timeout_tests=$((timeout_tests + 1))
            elif [ $exit_code -eq 134 ] || [ $exit_code -eq 139 ] || [ "$result" = "CRASHED" ]; then
                status="CRASHED"
                crashed_tests=$((crashed_tests + 1))
                # クラッシュ時はデフォルト値を使用
                nodes=0; time_sec=0; nps=0; utilization=0; subtasks=0
                root_splits=0; mid_spawns=0; dynamic_params=0; early_spawns=0; local_heap_fill=0
            elif [ "$result" = "WIN" ] || [ "$result" = "LOSE" ] || [ "$result" = "DRAW" ]; then
                status="SOLVED"
                solved_tests=$((solved_tests + 1))
            else
                status="UNKNOWN"
            fi

            # 数値を安全に取得
            nodes=${nodes:-0}; nodes=${nodes//[^0-9]/}; [ -z "$nodes" ] && nodes=0
            time_sec=${time_sec:-0}
            nps=${nps:-0}; nps=${nps//[^0-9]/}; [ -z "$nps" ] && nps=0
            utilization=${utilization:-0}
            subtasks=${subtasks:-0}; subtasks=${subtasks//[^0-9]/}; [ -z "$subtasks" ] && subtasks=0
            root_splits=${root_splits:-0}; root_splits=${root_splits//[^0-9]/}; [ -z "$root_splits" ] && root_splits=0
            mid_spawns=${mid_spawns:-0}; mid_spawns=${mid_spawns//[^0-9]/}; [ -z "$mid_spawns" ] && mid_spawns=0
            dynamic_params=${dynamic_params:-0}; dynamic_params=${dynamic_params//[^0-9]/}; [ -z "$dynamic_params" ] && dynamic_params=0
            early_spawns=${early_spawns:-0}; early_spawns=${early_spawns//[^0-9]/}; [ -z "$early_spawns" ] && early_spawns=0
            local_heap_fill=${local_heap_fill:-0}; local_heap_fill=${local_heap_fill//[^0-9]/}; [ -z "$local_heap_fill" ] && local_heap_fill=0

            # CSV出力
            echo "$empties,$file_id,$pos_file,$result,$nodes,$time_sec,$nps,$utilization,$subtasks,$root_splits,$mid_spawns,$dynamic_params,$early_spawns,$local_heap_fill,$status" >> "$SUMMARY_CSV"

            # 統計更新（安全な算術演算）
            total_nodes=$((total_nodes + nodes))
            total_subtasks=$((total_subtasks + subtasks))
            total_root_splits=$((total_root_splits + root_splits))
            total_mid_spawns=$((total_mid_spawns + mid_spawns))
            total_dynamic_params=$((total_dynamic_params + dynamic_params))
            total_early_spawns=$((total_early_spawns + early_spawns))
            total_local_heap_fill=$((total_local_heap_fill + local_heap_fill))
            sum_utilization=$(echo "$sum_utilization + ${utilization:-0}" | bc 2>/dev/null || echo "$sum_utilization")

            echo ""
            echo "【結果】$result ($status)"
            echo "  ノード: $nodes | 時間: ${time_sec}s | NPS: $nps"
            echo "  Worker稼働率: ${utilization}%"
            echo "  サブタスク: $subtasks"
            echo "  ─────────────────────────────────"
            echo "  機能発動回数:"
            echo "    [1] ROOT SPLIT:      $root_splits"
            echo "    [4] EARLY SPAWN:     $early_spawns"
            echo "    [5] LOCAL-HEAP-FILL: $local_heap_fill"
            echo ""
        done
    done

    # 最終統計
    end_time=$(date +%s)
    total_elapsed=$((end_time - start_time))
    avg_utilization=0
    if [ $total_tests -gt 0 ]; then
        avg_utilization=$(echo "scale=1; $sum_utilization / $total_tests" | bc 2>/dev/null || echo "0")
    fi

    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║                    ベンチマーク完了                        ║"
    echo "╠════════════════════════════════════════════════════════════╣"
    echo "║  【テスト結果】                                            ║"
    echo "║      総テスト数: $total_tests"
    echo "║      解決: $solved_tests"
    echo "║      タイムアウト: $timeout_tests"
    echo "║      クラッシュ: $crashed_tests"
    if [ $total_tests -gt 0 ]; then
    echo "║      解決率: $(echo "scale=1; $solved_tests * 100 / $total_tests" | bc 2>/dev/null || echo "N/A")%"
    fi
    echo "║                                                            ║"
    echo "╠════════════════════════════════════════════════════════════╣"
    echo "║  【パフォーマンス】                                        ║"
    echo "║      総ノード数: $total_nodes"
    echo "║      総実行時間: $total_elapsed 秒"
    if [ $total_elapsed -gt 0 ]; then
    echo "║      平均NPS: $(echo "scale=0; $total_nodes / $total_elapsed" | bc 2>/dev/null || echo "N/A")"
    fi
    echo "║      ★ 平均Worker稼働率: ${avg_utilization}%"
    echo "║        (修正前想定: ~4%, 目標: >90%)"
    echo "║                                                            ║"
    echo "╠════════════════════════════════════════════════════════════╣"
    echo "║  【機能効果測定】                                          ║"
    echo "║                                                            ║"
    echo "║  [1] ROOT SPLIT:       $total_root_splits 回"
    echo "║  [2] MID-SEARCH SPAWN: $total_mid_spawns 回"
    echo "║  [3] DYNAMIC PARAMS:   $total_dynamic_params 回"
    echo "║  [4] EARLY SPAWN:      $total_early_spawns 回"
    echo "║  [5] LOCAL-HEAP-FILL:  $total_local_heap_fill 回 ← NEW"
    echo "║                                                            ║"
    echo "╠════════════════════════════════════════════════════════════╣"
    echo "║  【タスク生成】                                            ║"
    echo "║      総サブタスク数: $total_subtasks"
    if [ $total_tests -gt 0 ]; then
    echo "║      平均: $(echo "scale=1; $total_subtasks / $total_tests" | bc 2>/dev/null || echo "N/A") タスク/テスト"
    fi
    echo "║                                                            ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""
    echo "結果ファイル: $SUMMARY_CSV"
    echo "ログディレクトリ: $LOG_DIR"

    # 最終サマリー保存
    cat > "$LOG_DIR/final_summary.txt" << EOF
═══════════════════════════════════════════════════════════════
オセロ終盤ソルバー ベンチマーク結果
【768コア・2TB環境専用 ローカルヒープ保持スポーン効果測定版】
═══════════════════════════════════════════════════════════════
実行日時: $(date)
環境: AMD EPYC 9965 × 2 (768コア), 2.2TB RAM
スレッド数: $THREADS
TTサイズ: ${TT_SIZE_MB}MB (2TB)
制限時間: ${TIME_LIMIT}秒
テストモード: $TEST_MODE
パラメータ: G=$G_PARAM, D=$D_PARAM, S=$S_PARAM

───────────────────────────────────────────────────────────────
機能説明
───────────────────────────────────────────────────────────────
[1] ROOT SPLIT: ルートタスク子ノードを即座にSharedArrayへ
[2] MID-SEARCH SPAWN: 50イテレーション毎にアイドルチェック
[3] DYNAMIC PARAMS: アイドル率でG/D/S自動緩和
[4] EARLY SPAWN: expand直後にスポーン判定
[5] LOCAL-HEAP-FILL: ローカルヒープ<16で全制限解除 ← NEW

───────────────────────────────────────────────────────────────
テスト結果
───────────────────────────────────────────────────────────────
総テスト数: $total_tests
解決: $solved_tests
タイムアウト: $timeout_tests
クラッシュ: $crashed_tests
解決率: $([ $total_tests -gt 0 ] && echo "scale=1; $solved_tests * 100 / $total_tests" | bc 2>/dev/null || echo "N/A")%

───────────────────────────────────────────────────────────────
パフォーマンス
───────────────────────────────────────────────────────────────
総ノード数: $total_nodes
総実行時間: $total_elapsed 秒
平均NPS: $([ $total_elapsed -gt 0 ] && echo "scale=0; $total_nodes / $total_elapsed" | bc 2>/dev/null || echo "N/A")

★ 平均Worker稼働率: ${avg_utilization}%
  (修正前想定: ~4%, 目標: >90%)

───────────────────────────────────────────────────────────────
機能効果測定
───────────────────────────────────────────────────────────────
[1] ROOT SPLIT:       $total_root_splits 回
[2] MID-SEARCH SPAWN: $total_mid_spawns 回
[3] DYNAMIC PARAMS:   $total_dynamic_params 回
[4] EARLY SPAWN:      $total_early_spawns 回
[5] LOCAL-HEAP-FILL:  $total_local_heap_fill 回 ← NEW

総サブタスク数: $total_subtasks
平均サブタスク: $([ $total_tests -gt 0 ] && echo "scale=1; $total_subtasks / $total_tests" | bc 2>/dev/null || echo "N/A") タスク/テスト

═══════════════════════════════════════════════════════════════
EOF

    echo ""
    echo "最終サマリー: $LOG_DIR/final_summary.txt"
}

# 実行
main "$@"
