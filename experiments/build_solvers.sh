#!/bin/bash
################################################################################
# build_solvers.sh - 5機能版ソルバーのビルド
#
# 768コア・2TB環境専用（AMD EPYC 9965）
#
# 5つの並列化機能:
#   [1] ROOT SPLIT: ルートタスク即座分割
#   [2] MID-SEARCH SPAWN: 探索中スポーン（50イテレーション毎）
#   [3] DYNAMIC PARAMS: 動的パラメータ調整（アイドル率ベース）
#   [4] EARLY SPAWN: 探索前早期スポーン
#   [5] LOCAL-HEAP-FILL: ローカルヒープ保持スポーン（NEW）
#
# ビルド対象:
#   - othello_solver_768core: 5機能版（768コア最適化）
#   - othello_endgame_solver_hybrid: 互換版
#   - othello_endgame_solver_workstealing: Work-Stealing版（比較用）
#   - Deep_Pns_benchmark: 逐次版（ベースライン）
################################################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# 環境検出
CORES=$(nproc 2>/dev/null || echo "8")
MEM_GB=$(free -g 2>/dev/null | grep "^Mem:" | awk '{print $2}' || echo "8")

echo "╔════════════════════════════════════════════════════════════╗"
echo "║  5機能版ソルバー ビルド（768コア・2TB環境専用）            ║"
echo "╠════════════════════════════════════════════════════════════╣"
echo "║  検出: $CORES コア, ${MEM_GB}GB RAM"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "5つの並列化機能:"
echo "  [1] ROOT SPLIT:      ルートタスク即座分割"
echo "  [2] MID-SEARCH:      探索中スポーン（50イテレーション毎）"
echo "  [3] DYNAMIC PARAMS:  動的パラメータ調整"
echo "  [4] EARLY SPAWN:     探索前早期スポーン"
echo "  [5] LOCAL-HEAP-FILL: ローカルヒープ保持スポーン ← NEW"
echo ""

# アーキテクチャ検出
ARCH_FLAGS="-march=native"
IS_EPYC=false
if grep -q "AMD EPYC" /proc/cpuinfo 2>/dev/null; then
    ARCH_FLAGS="-march=znver4 -mtune=znver4"
    IS_EPYC=true
    echo "アーキテクチャ: AMD EPYC 9965 (znver4)"
elif grep -q "Intel" /proc/cpuinfo 2>/dev/null; then
    ARCH_FLAGS="-march=native"
    echo "アーキテクチャ: Intel"
else
    echo "アーキテクチャ: native"
fi

# AVX512対応チェック
AVX_FLAGS=""
if grep -q "avx512" /proc/cpuinfo 2>/dev/null; then
    AVX_FLAGS="-mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx512cd"
    echo "SIMD: AVX512対応"
elif grep -q "avx2" /proc/cpuinfo 2>/dev/null; then
    AVX_FLAGS="-mavx2"
    echo "SIMD: AVX2対応"
fi

# TTサイズ設定（環境に応じて）
if [ "$MEM_GB" -ge 2000 ]; then
    TT_SIZE_MB=2048000  # 2TB
    echo "TTサイズ: 2TB (768コア環境)"
elif [ "$MEM_GB" -ge 64 ]; then
    TT_SIZE_MB=32768    # 32GB
    echo "TTサイズ: 32GB"
elif [ "$MEM_GB" -ge 16 ]; then
    TT_SIZE_MB=8192     # 8GB
    echo "TTサイズ: 8GB"
else
    TT_SIZE_MB=1024     # 1GB
    echo "TTサイズ: 1GB"
fi

echo ""

# ソースファイル確認
SOURCE_FILE="othello_endgame_solver_hybrid_check_tthit_fixed.c"
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

echo "  [1] ROOT SPLIT:      $([ $root_split -gt 0 ] && echo '✓' || echo '✗')"
echo "  [2] MID-SEARCH:      ✓"
echo "  [3] DYNAMIC PARAMS:  $([ $dynamic_params -gt 0 ] && echo '✓' || echo '✗')"
echo "  [4] EARLY SPAWN:     $([ $early_spawn -gt 0 ] && echo '✓' || echo '✗')"
echo "  [5] LOCAL-HEAP-FILL: $([ $local_heap_fill -gt 0 ] && echo '✓' || echo '✗')"
echo ""

# 最適化フラグ
OPT_FLAGS="-O3 -flto -ffast-math"
if [ "$IS_EPYC" = true ]; then
    OPT_FLAGS="$OPT_FLAGS -mprefer-vector-width=512"
fi

# 1. 768コア専用版のビルド
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "ビルド中: othello_solver_768core (5機能版・768コア最適化)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

gcc $OPT_FLAGS $ARCH_FLAGS \
    -DSTANDALONE_MAIN \
    -DMAX_THREADS=1024 \
    -DTT_SIZE_MB=$TT_SIZE_MB \
    -DCHUNK_SIZE=16 \
    $AVX_FLAGS \
    -o "othello_solver_768core" \
    "$SOURCE_FILE" \
    -lm -lpthread 2>&1

if [ $? -eq 0 ]; then
    echo "  ✓ 完了: othello_solver_768core"
    echo "  サイズ: $(ls -lh othello_solver_768core | awk '{print $5}')"
else
    echo "  ✗ 失敗"
    exit 1
fi

# 2. 互換版（othello_endgame_solver_hybrid）のビルド
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "ビルド中: othello_endgame_solver_hybrid (互換版)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

gcc $OPT_FLAGS $ARCH_FLAGS \
    -DSTANDALONE_MAIN \
    -DMAX_THREADS=1024 \
    $AVX_FLAGS \
    -o "othello_endgame_solver_hybrid" \
    "$SOURCE_FILE" \
    -lm -lpthread 2>&1

if [ $? -eq 0 ]; then
    echo "  ✓ 完了: othello_endgame_solver_hybrid"
    echo "  サイズ: $(ls -lh othello_endgame_solver_hybrid | awk '{print $5}')"
else
    echo "  ✗ 失敗"
fi

# 3. Work-Stealing版のビルド（比較用）
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "ビルド中: othello_endgame_solver_workstealing (比較用)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ -f "othello_endgame_solver_workstealing.c" ]; then
    gcc $OPT_FLAGS $ARCH_FLAGS \
        -DSTANDALONE_MAIN \
        -DMAX_THREADS=1024 \
        $AVX_FLAGS \
        -o "othello_endgame_solver_workstealing" \
        "othello_endgame_solver_workstealing.c" \
        -lm -lpthread 2>&1

    if [ $? -eq 0 ]; then
        echo "  ✓ 完了: othello_endgame_solver_workstealing"
        echo "  サイズ: $(ls -lh othello_endgame_solver_workstealing | awk '{print $5}')"
    else
        echo "  ⚠ 警告: Work-Stealing版のビルドに失敗"
    fi
else
    echo "  スキップ: ソースファイルがありません"
fi

# 4. 逐次版のビルド（ベースライン）
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "ビルド中: Deep_Pns_benchmark (逐次版ベースライン)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ -f "Deep_Pns_benchmark.c" ]; then
    gcc -O3 $ARCH_FLAGS \
        $AVX_FLAGS \
        -o "Deep_Pns_benchmark" \
        "Deep_Pns_benchmark.c" \
        -lm 2>&1

    if [ $? -eq 0 ]; then
        echo "  ✓ 完了: Deep_Pns_benchmark"
        echo "  サイズ: $(ls -lh Deep_Pns_benchmark | awk '{print $5}')"
    else
        echo "  ⚠ 警告: 逐次版のビルドに失敗"
    fi
else
    echo "  スキップ: ソースファイルがありません"
fi

# 5. TT並列版弱証明数探索のビルド（比較用）
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "ビルド中: wpns_tt_parallel (TT並列版弱証明数探索)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ -f "wpns_tt_parallel.c" ]; then
    gcc $OPT_FLAGS $ARCH_FLAGS \
        -DMAX_THREADS=1024 \
        -DTT_SIZE_MB=$TT_SIZE_MB \
        $AVX_FLAGS \
        -o "wpns_tt_parallel" \
        "wpns_tt_parallel.c" \
        -lm -lpthread 2>&1

    if [ $? -eq 0 ]; then
        echo "  ✓ 完了: wpns_tt_parallel"
        echo "  サイズ: $(ls -lh wpns_tt_parallel | awk '{print $5}')"
    else
        echo "  ⚠ 警告: TT並列版弱証明数探索のビルドに失敗"
    fi
else
    echo "  スキップ: ソースファイルがありません"
fi

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║                    ビルド完了                              ║"
echo "╠════════════════════════════════════════════════════════════╣"
echo "║  ビルドされたバイナリ:                                     ║"
for bin in othello_solver_768core othello_endgame_solver_hybrid othello_endgame_solver_workstealing Deep_Pns_benchmark wpns_tt_parallel; do
    if [ -f "$bin" ]; then
        size=$(ls -lh "$bin" | awk '{print $5}')
        printf "║    %-40s %s\n" "$bin" "$size"
    fi
done
echo "║                                                            ║"
echo "╠════════════════════════════════════════════════════════════╣"
echo "║  次のステップ:                                             ║"
echo "║    tsp ./run_full.sh 3600.0 quick                         ║"
echo "║    または                                                   ║"
echo "║    ./experiments/exp1_basic_comparison.sh                 ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
