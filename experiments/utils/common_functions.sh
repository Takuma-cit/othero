#!/bin/bash
################################################################################
# common_functions.sh - 実験スクリプト共通関数
#
# 全ての実験スクリプトから source されることを想定
################################################################################

# 安全な結果パース関数
# 使用法: parse_result_safe <log_file>
# 戻り値: result,nodes,time,nps,utilization,subtasks
parse_result_safe() {
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
    local total_workers=$(grep -E "Worker [0-9]+:" "$log_file" 2>/dev/null | wc -l)
    local active_workers=$(grep -E "Worker [0-9]+:" "$log_file" 2>/dev/null | awk '{print $4}' | awk '$1 > 0' | wc -l)
    local utilization=0
    if [ "$total_workers" -gt 0 ] 2>/dev/null; then
        utilization=$(echo "scale=2; $active_workers * 100 / $total_workers" | bc 2>/dev/null || echo "0")
    fi

    # タスク数を抽出
    local subtasks=$(grep "Subtasks spawned:" "$log_file" 2>/dev/null | head -1 | awk '{print $NF}' | tr -d ',')
    [ -z "$subtasks" ] && subtasks="0"

    echo "$result,$nodes,$time,$nps,$utilization,$subtasks"
}

# 安全な数値計算（空の値を0として扱う）
# 使用法: safe_calc "<bc式>"
safe_calc() {
    local expr="$1"
    local result=$(echo "$expr" | bc 2>/dev/null)
    [ -z "$result" ] && result="0"
    echo "$result"
}

# 安全な数値比較（空の値を0として扱う）
# 使用法: safe_compare <値1> <演算子> <値2>
safe_compare() {
    local val1="${1:-0}"
    local op="$2"
    local val2="${3:-0}"

    case "$op" in
        "-eq") [ "$val1" -eq "$val2" ] 2>/dev/null && return 0 || return 1 ;;
        "-ne") [ "$val1" -ne "$val2" ] 2>/dev/null && return 0 || return 1 ;;
        "-gt") [ "$val1" -gt "$val2" ] 2>/dev/null && return 0 || return 1 ;;
        "-ge") [ "$val1" -ge "$val2" ] 2>/dev/null && return 0 || return 1 ;;
        "-lt") [ "$val1" -lt "$val2" ] 2>/dev/null && return 0 || return 1 ;;
        "-le") [ "$val1" -le "$val2" ] 2>/dev/null && return 0 || return 1 ;;
        *) return 1 ;;
    esac
}

# ソルバー実行関数（numactl対応）
# 使用法: run_solver_safe <solver_name> <solver_bin> <position_file> <threads> <time_limit> <eval_file> <log_file>
run_solver_safe() {
    local solver_name="$1"
    local solver_bin="$2"
    local position_file="$3"
    local threads="$4"
    local time_limit="$5"
    local eval_file="$6"
    local log_file="$7"

    local timeout_sec=$((${time_limit%.*} + 60))

    if [[ "$solver_name" == "Sequential" ]]; then
        # 逐次版（Deep_Pns_benchmark）
        timeout "$timeout_sec" "./$solver_bin" "$position_file" "$time_limit" > "$log_file" 2>&1 || true
    else
        # 並列版（NUMA最適化付き）
        if command -v numactl &> /dev/null; then
            numactl --interleave=all \
                timeout "$timeout_sec" \
                "./$solver_bin" "$position_file" "$threads" "$time_limit" "$eval_file" -v > "$log_file" 2>&1 || true
        else
            timeout "$timeout_sec" \
                "./$solver_bin" "$position_file" "$threads" "$time_limit" "$eval_file" -v > "$log_file" 2>&1 || true
        fi
    fi

    return $?
}

# LocalHeap統計を抽出
# 使用法: parse_localheap_stats <log_file>
# 戻り値: local_pushes,local_pops,global_chunks_pushed,global_chunks_popped,exports,imports
parse_localheap_stats() {
    local log_file=$1

    local local_pushes=$(grep "LocalHeap:" "$log_file" 2>/dev/null | sed -n 's/.*LocalHeap: \([0-9]*\) pushes.*/\1/p')
    local local_pops=$(grep "LocalHeap:" "$log_file" 2>/dev/null | sed -n 's/.*pushes, \([0-9]*\) pops.*/\1/p')
    local global_pushed=$(grep "GlobalChunkQueue:" "$log_file" 2>/dev/null | sed -n 's/.*GlobalChunkQueue: \([0-9]*\) chunks pushed.*/\1/p')
    local global_popped=$(grep "GlobalChunkQueue:" "$log_file" 2>/dev/null | sed -n 's/.*pushed, \([0-9]*\) chunks popped.*/\1/p')
    local exports=$(grep "Export/Import:" "$log_file" 2>/dev/null | sed -n 's/.*Export\/Import: \([0-9]*\) exported.*/\1/p')
    local imports=$(grep "Export/Import:" "$log_file" 2>/dev/null | sed -n 's/.*exported, \([0-9]*\) imported.*/\1/p')

    [ -z "$local_pushes" ] && local_pushes="0"
    [ -z "$local_pops" ] && local_pops="0"
    [ -z "$global_pushed" ] && global_pushed="0"
    [ -z "$global_popped" ] && global_popped="0"
    [ -z "$exports" ] && exports="0"
    [ -z "$imports" ] && imports="0"

    echo "$local_pushes,$local_pops,$global_pushed,$global_popped,$exports,$imports"
}

# TT統計を抽出
# 使用法: parse_tt_stats <log_file>
# 戻り値: hits,stores,collisions,hit_rate
parse_tt_stats() {
    local log_file=$1

    local tt_line=$(grep "^TT:" "$log_file" 2>/dev/null | head -1)

    local hits="0"
    local stores="0"
    local collisions="0"
    local hit_rate="0"

    if [ -n "$tt_line" ]; then
        hits=$(echo "$tt_line" | sed -n 's/.*TT: \([0-9]*\) hits.*/\1/p')
        stores=$(echo "$tt_line" | sed -n 's/.*hits, \([0-9]*\) stores.*/\1/p')
        collisions=$(echo "$tt_line" | sed -n 's/.*stores, \([0-9]*\) collisions.*/\1/p')
        hit_rate=$(echo "$tt_line" | sed -n 's/.*(\([0-9.]*\)% hit rate).*/\1/p')
    fi

    [ -z "$hits" ] && hits="0"
    [ -z "$stores" ] && stores="0"
    [ -z "$collisions" ] && collisions="0"
    [ -z "$hit_rate" ] && hit_rate="0"

    echo "$hits,$stores,$collisions,$hit_rate"
}
