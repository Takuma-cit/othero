#!/bin/bash
################################################################################
# run_all_experiments.sh - 全実験の一括実行スクリプト
#
# 使用方法:
#   bash run_all_experiments.sh [オプション]
#
# オプション:
#   --quick       : 最優先実験のみ実行（実験1, 2, A）
#   --standard    : 主要実験を実行（実験1, 2, 3, 5, A, B）
#   --full        : 全実験を実行（デフォルト）
#   --skip-slow   : 長時間実験をスキップ（実験3, 4をスキップ）
#
# 推定実行時間:
#   --quick    : 20-40時間
#   --standard : 50-80時間
#   --full     : 80-150時間
################################################################################

set -e  # エラーで即座に終了

# スクリプトのディレクトリを取得
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 結果ディレクトリの作成
mkdir -p results
mkdir -p results/logs

# ログファイル
MASTER_LOG="results/logs/master_$(date +%Y%m%d_%H%M%S).log"

# ログ関数
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$MASTER_LOG"
}

log_header() {
    echo "" | tee -a "$MASTER_LOG"
    echo "========================================" | tee -a "$MASTER_LOG"
    echo "$*" | tee -a "$MASTER_LOG"
    echo "========================================" | tee -a "$MASTER_LOG"
}

# 実行モードの決定
MODE="full"
if [[ "$1" == "--quick" ]]; then
    MODE="quick"
elif [[ "$1" == "--standard" ]]; then
    MODE="standard"
elif [[ "$1" == "--skip-slow" ]]; then
    MODE="skip-slow"
fi

log_header "全実験実行スクリプト開始"
log "実行モード: $MODE"
log "開始時刻: $(date)"
log "実行ディレクトリ: $SCRIPT_DIR"

# 環境チェック
log_header "環境チェック"
if [ -f "utils/check_environment.sh" ]; then
    bash utils/check_environment.sh | tee -a "$MASTER_LOG"
else
    log "警告: check_environment.sh が見つかりません"
fi

# tspの確認
if ! command -v tsp &> /dev/null; then
    log "エラー: tsp (Task Spooler) がインストールされていません"
    log "インストール方法: sudo apt-get install task-spooler"
    exit 1
fi

# コンパイル済みバイナリの確認
log_header "実行ファイルの確認"
REQUIRED_FILES=(
    "../Deep_Pns_benchmark"
    "../othello_endgame_solver_workstealing"
    "../othello_endgame_solver_hybrid"
)

for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$file" ]; then
        log "エラー: $file が見つかりません"
        log "まずプログラムをコンパイルしてください"
        exit 1
    else
        log "OK: $file"
    fi
done

# 実験スクリプトの実行関数
run_experiment() {
    local exp_name="$1"
    local exp_script="$2"
    local priority="$3"

    log_header "実験投入: $exp_name (優先度: $priority)"

    if [ ! -f "$exp_script" ]; then
        log "エラー: $exp_script が見つかりません"
        return 1
    fi

    # tspでジョブ投入
    tsp -L "$exp_name" bash "$exp_script"
    local job_id=$?

    log "$exp_name を tsp キューに投入しました (ID: $job_id)"
    log "進捗確認: tsp"
    log "ログ確認: tsp -c $job_id"
}

# 実験の実行
log_header "実験スケジューリング開始"

# Phase 1: 必須実験（全モードで実行）
log "Phase 1: 必須実験"
run_experiment "実験1: 基本性能比較" "exp1_basic_comparison.sh" "★★★★★"
run_experiment "実験2: スケーラビリティ" "exp2_strong_scaling.sh" "★★★★★"
run_experiment "実験A: ロック比較" "expA_lock_comparison.sh" "★★★★★"

if [[ "$MODE" == "quick" ]]; then
    log "クイックモード: Phase 1のみ実行"
    log_header "実験投入完了"
    log "投入された実験数: 3"
    log "tsp で進捗確認してください"
    exit 0
fi

# Phase 2: 主要実験（standardモード以上）
log "Phase 2: 主要実験"
run_experiment "実験5: 負荷分散評価" "exp5_load_balance.sh" "★★★★☆"
run_experiment "実験B: LocalHeap効果" "expB_local_global_ratio.sh" "★★★★☆"
run_experiment "実験E: 問題難易度ばらつき" "expE_difficulty_variance.sh" "★★★★☆"
run_experiment "実験F: 置換表ヒット率分析" "expF_tt_hit_rate_analysis.sh" "★★★★☆"

if [[ "$MODE" == "standard" ]]; then
    log "スタンダードモード: Phase 1-2を実行"
    log_header "実験投入完了"
    log "投入された実験数: 7"
    log "tsp で進捗確認してください"
    exit 0
fi

# Phase 3: 追加実験（fullモードのみ）
log "Phase 3: 追加実験"
if [[ "$MODE" != "skip-slow" ]]; then
    run_experiment "実験4: 弱スケーリング" "exp4_weak_scaling.sh" "★★★☆☆"
fi
run_experiment "実験6: NUMA効果" "exp6_numa_effects.sh" "★★★☆☆"

log_header "全実験投入完了"

# 投入された実験の総数
TOTAL_JOBS=$(tsp | tail -n +2 | wc -l)
log "投入された実験総数: $TOTAL_JOBS"
log "実行モード: $MODE"

# 進捗確認方法の案内
log ""
log "========== 実験進捗の確認方法 =========="
log ""
log "1. ジョブ一覧の確認:"
log "   tsp"
log ""
log "2. 特定ジョブのログ確認:"
log "   tsp -c <job_id>"
log ""
log "3. 最新ジョブのリアルタイム監視:"
log "   tsp -t"
log ""
log "4. 結果ディレクトリの確認:"
log "   ls -lh results/"
log ""
log "5. 全ジョブ完了待機:"
log "   tsp -w"
log ""
log "========================================="

# 推定完了時刻の計算
log ""
log "========== 推定完了時刻 =========="
case $MODE in
    "quick")
        ESTIMATED_HOURS="20-40"
        ;;
    "standard")
        ESTIMATED_HOURS="50-80"
        ;;
    "full"|"skip-slow")
        ESTIMATED_HOURS="80-150"
        ;;
esac

CURRENT_TIME=$(date +%s)
ESTIMATED_END_MIN=$((CURRENT_TIME + 20*3600))
ESTIMATED_END_MAX=$((CURRENT_TIME + 150*3600))

log "推定実行時間: $ESTIMATED_HOURS 時間"
log "推定完了時刻: $(date -d @$ESTIMATED_END_MIN '+%Y-%m-%d %H:%M') 〜 $(date -d @$ESTIMATED_END_MAX '+%Y-%m-%d %H:%M')"
log "================================="

log ""
log_header "実験実行の準備完了"
log "全ての実験がtspキューに投入されました"
log "実験は自動的に順次実行されます"
log ""
log "マスターログ: $MASTER_LOG"
log ""
log "実験完了後は以下を実行してください:"
log "  python3 utils/analyze_results.py"
log ""

exit 0
