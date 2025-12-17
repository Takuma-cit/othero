#!/bin/bash
################################################################################
# check_environment.sh - 実験環境チェックスクリプト
#
# 実行前に環境が正しく設定されているか確認
################################################################################

echo "========================================="
echo "実験環境チェック"
echo "========================================="
echo ""

# カラー出力
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

check_ok() {
    echo -e "${GREEN}[OK]${NC} $1"
}

check_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

check_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

ERRORS=0
WARNINGS=0

# 1. CPU情報確認
echo "1. CPU情報"
echo "----------"
if command -v lscpu &> /dev/null; then
    CPU_MODEL=$(lscpu | grep "Model name" | sed 's/Model name: *//')
    CPU_CORES=$(lscpu | grep "^CPU(s):" | awk '{print $2}')
    CPU_SOCKETS=$(lscpu | grep "Socket(s):" | awk '{print $2}')
    NUMA_NODES=$(lscpu | grep "NUMA node(s):" | awk '{print $3}')

    check_ok "CPU: $CPU_MODEL"
    check_ok "総コア数: $CPU_CORES"
    check_ok "ソケット数: $CPU_SOCKETS"
    check_ok "NUMAノード: $NUMA_NODES"

    if [ "$CPU_CORES" -ge 256 ]; then
        check_ok "大規模並列環境 ($CPU_CORES コア)"
    elif [ "$CPU_CORES" -ge 64 ]; then
        check_warn "中規模環境 ($CPU_CORES コア) - 一部実験はスケールしない可能性"
        WARNINGS=$((WARNINGS + 1))
    else
        check_error "小規模環境 ($CPU_CORES コア) - 実験には不十分"
        ERRORS=$((ERRORS + 1))
    fi
else
    check_error "lscpu コマンドが見つかりません"
    ERRORS=$((ERRORS + 1))
fi
echo ""

# 2. メモリ確認
echo "2. メモリ"
echo "----------"
if command -v free &> /dev/null; then
    TOTAL_MEM_GB=$(free -g | awk '/^Mem:/{print $2}')
    FREE_MEM_GB=$(free -g | awk '/^Mem:/{print $7}')

    check_ok "総メモリ: ${TOTAL_MEM_GB} GB"
    check_ok "利用可能: ${FREE_MEM_GB} GB"

    if [ "$TOTAL_MEM_GB" -ge 500 ]; then
        check_ok "メモリ十分 (${TOTAL_MEM_GB} GB)"
    elif [ "$TOTAL_MEM_GB" -ge 100 ]; then
        check_warn "メモリやや少ない (${TOTAL_MEM_GB} GB) - 大規模実験で不足の可能性"
        WARNINGS=$((WARNINGS + 1))
    else
        check_error "メモリ不足 (${TOTAL_MEM_GB} GB)"
        ERRORS=$((ERRORS + 1))
    fi
else
    check_error "free コマンドが見つかりません"
    ERRORS=$((ERRORS + 1))
fi
echo ""

# 3. tsp (Task Spooler) 確認
echo "3. Task Spooler (tsp)"
echo "----------"
if command -v tsp &> /dev/null; then
    TSP_VERSION=$(tsp -V 2>&1 || echo "不明")
    check_ok "tsp インストール済み ($TSP_VERSION)"

    # tspキューの状態確認
    TSP_RUNNING=$(tsp | tail -n +2 | grep -c "running" || echo "0")
    TSP_QUEUED=$(tsp | tail -n +2 | grep -c "queued" || echo "0")

    if [ "$TSP_RUNNING" -gt 0 ] || [ "$TSP_QUEUED" -gt 0 ]; then
        check_warn "tsp にジョブが実行中/待機中です (実行中:$TSP_RUNNING, 待機:$TSP_QUEUED)"
        WARNINGS=$((WARNINGS + 1))
    else
        check_ok "tsp キューは空です"
    fi
else
    check_error "tsp (Task Spooler) がインストールされていません"
    echo "        インストール方法: sudo apt-get install task-spooler"
    ERRORS=$((ERRORS + 1))
fi
echo ""

# 4. コンパイル済みバイナリ確認
echo "4. 実行ファイル"
echo "----------"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

BINARIES=(
    "Deep_Pns_benchmark"
    "othello_endgame_solver_workstealing"
    "othello_endgame_solver_hybrid"
)

for bin in "${BINARIES[@]}"; do
    if [ -f "$BASE_DIR/$bin" ]; then
        if [ -x "$BASE_DIR/$bin" ]; then
            check_ok "$bin (実行可能)"
        else
            check_warn "$bin (実行権限なし)"
            chmod +x "$BASE_DIR/$bin"
            check_ok "$bin に実行権限を付与しました"
        fi
    else
        check_error "$bin が見つかりません"
        echo "        コンパイル方法: cd $BASE_DIR && make"
        ERRORS=$((ERRORS + 1))
    fi
done
echo ""

# 5. 評価関数ファイル確認
echo "5. 評価関数ファイル"
echo "----------"
EVAL_FILE="$BASE_DIR/eval/eval.dat"
if [ -f "$EVAL_FILE" ]; then
    EVAL_SIZE=$(du -h "$EVAL_FILE" | awk '{print $1}')
    check_ok "eval.dat ($EVAL_SIZE)"
else
    check_warn "eval.dat が見つかりません - 一部実験で評価関数なしで実行"
    echo "        作成方法: 既存のEdax評価関数を配置、または空きマス数のみで評価"
    WARNINGS=$((WARNINGS + 1))
fi
echo ""

# 6. ディレクトリ構造確認
echo "6. ディレクトリ構造"
echo "----------"
REQUIRED_DIRS=(
    "experiments"
    "experiments/results"
    "experiments/logs"
    "experiments/utils"
    "experiments/test_positions"
)

for dir in "${REQUIRED_DIRS[@]}"; do
    FULL_DIR="$BASE_DIR/$dir"
    if [ -d "$FULL_DIR" ]; then
        check_ok "$dir/"
    else
        check_warn "$dir/ が見つかりません - 作成します"
        mkdir -p "$FULL_DIR"
        check_ok "$dir/ を作成しました"
    fi
done
echo ""

# 7. Python環境確認（グラフ作成用）
echo "7. Python環境"
echo "----------"
if command -v python3 &> /dev/null; then
    PYTHON_VER=$(python3 --version 2>&1)
    check_ok "$PYTHON_VER"

    # 必要なモジュール確認
    PYTHON_MODULES=("numpy" "pandas" "matplotlib")
    for module in "${PYTHON_MODULES[@]}"; do
        if python3 -c "import $module" 2>/dev/null; then
            check_ok "Python モジュール: $module"
        else
            check_warn "Python モジュール: $module がインストールされていません"
            echo "        インストール方法: pip3 install $module"
            WARNINGS=$((WARNINGS + 1))
        fi
    done
else
    check_warn "python3 が見つかりません - グラフ自動生成ができません"
    echo "        手動でグラフ作成してください"
    WARNINGS=$((WARNINGS + 1))
fi
echo ""

# 8. ディスク空き容量確認
echo "8. ディスク容量"
echo "----------"
AVAILABLE_GB=$(df -BG "$BASE_DIR" | tail -1 | awk '{print $4}' | sed 's/G//')
check_ok "利用可能: ${AVAILABLE_GB} GB"

if [ "$AVAILABLE_GB" -ge 100 ]; then
    check_ok "ディスク容量十分"
elif [ "$AVAILABLE_GB" -ge 20 ]; then
    check_warn "ディスク容量やや少ない (${AVAILABLE_GB} GB) - ログファイルに注意"
    WARNINGS=$((WARNINGS + 1))
else
    check_error "ディスク容量不足 (${AVAILABLE_GB} GB)"
    ERRORS=$((ERRORS + 1))
fi
echo ""

# 9. 推奨設定確認
echo "9. 推奨設定"
echo "----------"

# CPUガバナー確認
if [ -f "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor" ]; then
    GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
    if [ "$GOVERNOR" == "performance" ]; then
        check_ok "CPUガバナー: performance"
    else
        check_warn "CPUガバナー: $GOVERNOR (performanceが推奨)"
        echo "        設定方法: echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
        WARNINGS=$((WARNINGS + 1))
    fi
else
    check_warn "CPUガバナー設定を確認できません"
fi

# NUMAバランス確認
if [ -f "/proc/sys/kernel/numa_balancing" ]; then
    NUMA_BAL=$(cat /proc/sys/kernel/numa_balancing)
    if [ "$NUMA_BAL" == "1" ]; then
        check_ok "NUMA自動バランシング: 有効"
    else
        check_warn "NUMA自動バランシング: 無効"
    fi
fi
echo ""

# 結果サマリー
echo "========================================="
echo "チェック結果サマリー"
echo "========================================="

if [ "$ERRORS" -eq 0 ] && [ "$WARNINGS" -eq 0 ]; then
    check_ok "全てのチェックに合格しました"
    echo ""
    echo "実験を開始できます:"
    echo "  bash experiments/run_all_experiments.sh"
    exit 0
elif [ "$ERRORS" -eq 0 ]; then
    echo -e "${YELLOW}警告: $WARNINGS 件${NC}"
    echo "一部の警告がありますが、実験は可能です"
    echo ""
    echo "実験を開始できます:"
    echo "  bash experiments/run_all_experiments.sh"
    exit 0
else
    echo -e "${RED}エラー: $ERRORS 件、警告: $WARNINGS 件${NC}"
    echo "エラーを修正してから実験を開始してください"
    exit 1
fi
