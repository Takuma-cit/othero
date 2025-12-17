#!/bin/bash
################################################################################
# apply_fixes.sh - 全実験スクリプトに修正を一括適用
################################################################################

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================  "
echo "実験スクリプト一括修正ツール"
echo "========================================"
echo ""

# 修正対象スクリプト（exp1, exp2は既に修正済み）
SCRIPTS=(
    "exp4_weak_scaling.sh"
    "exp5_load_balance.sh"
    "exp6_numa_effects.sh"
    "expA_lock_comparison.sh"
    "expB_local_global_ratio.sh"
    "expE_difficulty_variance.sh"
    "expF_tt_hit_rate_analysis.sh"
)

echo "修正対象スクリプト:"
for script in "${SCRIPTS[@]}"; do
    echo "  - $script"
done
echo ""

read -p "これらのスクリプトを修正しますか？ (y/n): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "中止しました"
    exit 0
fi

echo ""
echo "バックアップを作成中..."
mkdir -p backups_$(date +%Y%m%d_%H%M%S)
for script in "${SCRIPTS[@]}"; do
    if [ -f "$script" ]; then
        cp "$script" "backups_$(date +%Y%m%d_%H%M%S)/$script"
        echo "  バックアップ: $script"
    fi
done

echo ""
echo "修正を適用中..."

for script in "${SCRIPTS[@]}"; do
    if [ ! -f "$script" ]; then
        echo "  スキップ: $script (ファイルが存在しません)"
        continue
    fi

    echo "  修正中: $script"

    # 1. grep "^Time:" を grep "^Total:" に変更
    sed -i 's/grep "^Time:"/grep "^Total:"/g' "$script"

    # 2. grep "^Nodes:" を Total行からのパースに変更
    # この修正は複雑なので、個別に対応が必要
    # ここでは warning のみ
    if grep -q 'grep "^Nodes:"' "$script"; then
        echo "    警告: $script には手動修正が必要な箇所があります（Nodes:パース）"
    fi

    # 3. bc計算にエラーハンドリングを追加（簡易版）
    # 完全な修正には手動編集が必要
    sed -i 's/| bc)/| bc 2>\/dev\/null || echo "0")/g' "$script"
    sed -i 's/| bc )/| bc 2>\/dev\/null || echo "0" )/g' "$script"

    # 4. numactl チェックを追加（パターンマッチが難しいため警告のみ）
    if grep -q 'numactl --interleave=all' "$script" && ! grep -q 'command -v numactl' "$script"; then
        echo "    警告: $script にはnumactlチェックの追加が必要です"
    fi

    echo "    完了: $script"
done

echo ""
echo "========================================"
echo "修正完了"
echo "========================================"
echo ""
echo "注意事項:"
echo "1. 一部のスクリプトは手動修正が必要です"
echo "2. FIXES_APPLIED.mdを参照して、各スクリプトを確認してください"
echo "3. バックアップは backups_YYYYMMDD_HHMMSS/ に保存されています"
echo ""
echo "推奨される次のステップ:"
echo "1. exp1_basic_comparison.sh をテスト実行"
echo "2. exp2_strong_scaling.sh をテスト実行"
echo "3. その他のスクリプトを個別に確認・修正"
echo ""
