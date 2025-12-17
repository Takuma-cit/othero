#!/bin/bash
################################################################################
# quick_test.sh - 本番環境での簡易動作テスト
#
# 目的: 実験スクリプトが正常に動作するか最小限のテストで確認
#
# 実行時間: 5-10分
################################################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

echo "========================================"
echo "実験スクリプト 簡易動作テスト"
echo "========================================"
echo ""

# カラー出力
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

ERRORS=0
WARNINGS=0

# テスト1: 実行ファイルの確認
echo "テスト1: 実行ファイルの確認"
echo "----------------------------"

if [ -f "Deep_Pns_benchmark" ] && [ -x "Deep_Pns_benchmark" ]; then
    success "Deep_Pns_benchmark"
else
    error "Deep_Pns_benchmark が見つからないか実行権限がありません"
    ERRORS=$((ERRORS + 1))
fi

if [ -f "othello_endgame_solver_workstealing" ] && [ -x "othello_endgame_solver_workstealing" ]; then
    success "othello_endgame_solver_workstealing"
else
    error "othello_endgame_solver_workstealing が見つからないか実行権限がありません"
    ERRORS=$((ERRORS + 1))
fi

if [ -f "othello_endgame_solver_hybrid" ] && [ -x "othello_endgame_solver_hybrid" ]; then
    success "othello_endgame_solver_hybrid"
else
    error "othello_endgame_solver_hybrid が見つからないか実行権限がありません"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# テスト2: テスト局面の確認
echo "テスト2: テスト局面の確認"
echo "----------------------------"

TEST_POS="test_positions/empties_10_id_000.pos"
if [ -f "$TEST_POS" ]; then
    success "$TEST_POS が存在します"
else
    error "$TEST_POS が見つかりません"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# テスト3: 評価関数ファイルの確認
echo "テスト3: 評価関数ファイルの確認"
echo "----------------------------"

if [ -f "eval/eval.dat" ]; then
    success "eval/eval.dat が存在します"
else
    warn "eval/eval.dat が見つかりません（一部実験で必要）"
    WARNINGS=$((WARNINGS + 1))
fi

echo ""

# テスト4: 必要なコマンドの確認
echo "テスト4: 必要なコマンドの確認"
echo "----------------------------"

if command -v tsp &> /dev/null; then
    success "tsp (Task Spooler) インストール済み"
else
    error "tsp が見つかりません"
    echo "  インストール: sudo apt-get install task-spooler"
    ERRORS=$((ERRORS + 1))
fi

if command -v numactl &> /dev/null; then
    success "numactl インストール済み"
else
    warn "numactl が見つかりません（性能最適化に使用）"
    echo "  インストール: sudo apt-get install numactl"
    WARNINGS=$((WARNINGS + 1))
fi

if command -v bc &> /dev/null; then
    success "bc インストール済み"
else
    error "bc が見つかりません（スクリプトで計算に使用）"
    echo "  インストール: sudo apt-get install bc"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# エラーがある場合はここで終了
if [ $ERRORS -gt 0 ]; then
    echo "========================================"
    error "$ERRORS 個のエラーがあります"
    echo "上記のエラーを修正してから続行してください"
    echo "========================================"
    exit 1
fi

# テスト5: 単一実行テスト（最も重要）
echo "テスト5: Hybrid版の単一実行テスト"
echo "----------------------------"
echo "テスト局面: $TEST_POS"
echo "スレッド数: 4"
echo "制限時間: 30秒"
echo ""

TEST_OUTPUT="/tmp/quick_test_$$.log"

echo "実行中..."

if command -v numactl &> /dev/null; then
    timeout 60 numactl --interleave=all \
        ./othello_endgame_solver_hybrid \
        "$TEST_POS" 4 30.0 eval/eval.dat -v > "$TEST_OUTPUT" 2>&1 || true
else
    timeout 60 ./othello_endgame_solver_hybrid \
        "$TEST_POS" 4 30.0 eval/eval.dat > "$TEST_OUTPUT" 2>&1 || true
fi

# 結果の確認
if grep -q "^Result:" "$TEST_OUTPUT"; then
    RESULT=$(grep "^Result:" "$TEST_OUTPUT" | awk '{print $2}')
    success "実行完了: 結果 = $RESULT"

    if grep -q "^Total:" "$TEST_OUTPUT"; then
        NODES=$(grep "^Total:" "$TEST_OUTPUT" | awk '{print $2}')
        TIME=$(grep "^Total:" "$TEST_OUTPUT" | awk '{print $5}')
        success "  ノード数: $NODES, 時間: ${TIME}秒"
    fi

    if grep -q "Worker 0:" "$TEST_OUTPUT"; then
        WORKER_COUNT=$(grep -c "^Worker" "$TEST_OUTPUT")
        success "  Worker統計: ${WORKER_COUNT}個のWorkerが動作"
    fi
else
    error "実行結果が取得できませんでした"
    echo "ログの内容:"
    cat "$TEST_OUTPUT"
    ERRORS=$((ERRORS + 1))
fi

rm -f "$TEST_OUTPUT"

echo ""

# テスト6: 結果ディレクトリの作成テスト
echo "テスト6: 結果ディレクトリの作成テスト"
echo "----------------------------"

RESULTS_DIR="experiments/results"
mkdir -p "$RESULTS_DIR/logs"

if [ -d "$RESULTS_DIR" ]; then
    success "results ディレクトリ作成成功"
else
    error "results ディレクトリの作成に失敗"
    ERRORS=$((ERRORS + 1))
fi

# テストCSVファイルの作成
TEST_CSV="$RESULTS_DIR/test_write.csv"
echo "test,data" > "$TEST_CSV" 2>&1

if [ -f "$TEST_CSV" ]; then
    success "CSVファイルの書き込み成功"
    rm -f "$TEST_CSV"
else
    error "CSVファイルの書き込みに失敗"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# 最終結果
echo "========================================"
echo "テスト結果サマリー"
echo "========================================"

if [ $ERRORS -eq 0 ] && [ $WARNINGS -eq 0 ]; then
    success "全てのテストに合格しました"
    echo ""
    echo "次のステップ:"
    echo "  1. 最小テストの実行:"
    echo "     bash experiments/exp1_basic_comparison.sh"
    echo ""
    echo "  2. または全実験の投入:"
    echo "     bash experiments/run_all_experiments.sh --quick"
    echo ""
    exit 0
elif [ $ERRORS -eq 0 ]; then
    warn "$WARNINGS 個の警告がありますが、実験は可能です"
    echo ""
    echo "次のステップ:"
    echo "  bash experiments/run_all_experiments.sh --quick"
    echo ""
    exit 0
else
    error "$ERRORS 個のエラー、$WARNINGS 個の警告があります"
    echo "エラーを修正してから実験を開始してください"
    exit 1
fi
