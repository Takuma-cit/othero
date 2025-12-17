# 実験スクリプト テスト状態レポート

**日時:** 2025年12月15日 14:50
**環境:** WSL (Ubuntu on Windows)
**状態:** ✅ 全スクリプト検証完了

---

## WSL環境でのテスト結果

### ✅ Phase 1: 構文チェック（完了）

全スクリプトの構文チェックを実施し、エラーなく完了しました。

| スクリプト | 構文チェック | 改行コード変換 | 実行権限 |
|-----------|------------|--------------|---------|
| exp1_basic_comparison.sh | ✅ OK | ✅ LF | ✅ +x |
| exp2_strong_scaling.sh | ✅ OK | ✅ LF | ✅ +x |
| exp4_weak_scaling.sh | ✅ OK | ✅ LF | ✅ +x |
| exp5_load_balance.sh | ✅ OK | ✅ LF | ✅ +x |
| exp6_numa_effects.sh | ✅ OK | ✅ LF | ✅ +x |
| expA_lock_comparison.sh | ✅ OK | ✅ LF | ✅ +x |
| expB_local_global_ratio.sh | ✅ OK | ✅ LF | ✅ +x |
| expE_difficulty_variance.sh | ✅ OK | ✅ LF | ✅ +x |
| expF_tt_hit_rate_analysis.sh | ✅ OK | ✅ LF | ✅ +x |
| run_all_experiments.sh | ✅ OK | ✅ LF | ✅ +x |
| utils/check_environment.sh | ✅ OK | ✅ LF | ✅ +x |
| quick_test.sh | ✅ OK | ✅ LF | ✅ +x |

**実施した修正:**
- Windows改行コード（CRLF）→ Unix改行コード（LF）変換
- 実行権限の付与
- bash -n による構文検証

---

## 本番環境（202.17.18.122）での次のステップ

### Step 1: ファイル転送

```bash
# ローカルマシンで実行（WSLまたはPowerShell）
cd /mnt/c/Users/takum_j7ulelc/OneDrive/デスクトップ/CIT/卒論_オセロ/1214-テーマ/files

# 本番環境にファイルを転送
scp -r experiments maelab@202.17.18.122:/home/maelab/your_project_directory/
```

または rsync を使用（より安全）:
```bash
rsync -avz --progress experiments/ maelab@202.17.18.122:/home/maelab/your_project_directory/experiments/
```

### Step 2: 本番環境でのクイックテスト

```bash
# SSH接続
ssh maelab@202.17.18.122

# プロジェクトディレクトリに移動
cd /home/maelab/your_project_directory

# クイックテスト実行（5-10分）
bash experiments/quick_test.sh
```

**期待される結果:**
```
========================================
実験スクリプト 簡易動作テスト
========================================

テスト1: 実行ファイルの確認
----------------------------
[OK] Deep_Pns_benchmark
[OK] othello_endgame_solver_workstealing
[OK] othello_endgame_solver_hybrid

テスト2: テスト局面の確認
----------------------------
[OK] test_positions/empties_10_id_000.pos が存在します

テスト3: 評価関数ファイルの確認
----------------------------
[OK] eval/eval.dat が存在します

テスト4: 必要なコマンドの確認
----------------------------
[OK] tsp (Task Spooler) インストール済み
[OK] numactl インストール済み
[OK] bc インストール済み

テスト5: Hybrid版の単一実行テスト
----------------------------
[OK] 実行完了: 結果 = WIN
[OK]   ノード数: XXXXX, 時間: X.XX秒
[OK]   Worker統計: 4個のWorkerが動作

テスト6: 結果ディレクトリの作成テスト
----------------------------
[OK] results ディレクトリ作成成功
[OK] CSVファイルの書き込み成功

========================================
テスト結果サマリー
========================================
[OK] 全てのテストに合格しました

次のステップ:
  1. 最小テストの実行:
     bash experiments/exp1_basic_comparison.sh

  2. または全実験の投入:
     bash experiments/run_all_experiments.sh --quick
```

### Step 3: 環境詳細チェック

```bash
# 環境チェックスクリプト実行
bash experiments/utils/check_environment.sh
```

**確認項目:**
- ✅ CPU: 768コア
- ✅ メモリ: 2TB以上
- ✅ tsp: インストール済み
- ✅ numactl: インストール済み
- ✅ 実行ファイル: 3つ存在
- ✅ テスト局面: 配置済み
- ✅ ディスク空き容量: 100GB以上

### Step 4: 最小テスト実行（オプション）

quick_test.sh が成功したら、実験1の最小版でテスト:

```bash
# 実験1を1問だけに制限してテスト
cd experiments
cp exp1_basic_comparison.sh exp1_mini_test.sh

# エディタでTEST_POSITIONSを編集
nano exp1_mini_test.sh
# 以下のように変更:
# TEST_POSITIONS=(
#     "empties_10_id_000.pos"
# )

# 実行（5-10分で完了）
bash exp1_mini_test.sh

# 結果確認
cat results/exp1_results.csv
cat results/exp1_summary.txt
```

### Step 5: TSPへの本番投入

全てのテストが成功したら、本番実験を開始:

#### オプション1: クイックモード（推奨）

```bash
# 最小限の実験（実験1, 2, A）を実行
bash experiments/run_all_experiments.sh --quick

# 投入されたジョブ確認
tsp

# 進捗モニタリング
tsp -t
```

**実行時間:** 20-40時間
**含まれる実験:** 1, 2, A（論文に必須のデータ）

#### オプション2: スタンダードモード

```bash
# 主要実験を実行
bash experiments/run_all_experiments.sh --standard

# 進捗確認
tsp
```

**実行時間:** 60-100時間
**含まれる実験:** 1, 2, 5, A, B, E, F

#### オプション3: フルモード

```bash
# 全実験を実行
bash experiments/run_all_experiments.sh --full

# 進捗確認
tsp
```

**実行時間:** 100-140時間
**含まれる実験:** 1, 2, 4, 5, 6, A, B, E, F

---

## トラブルシューティング

### 問題: quick_test.sh でエラーが出る

**対処1: 実行ファイルの確認**
```bash
ls -lh Deep_Pns_benchmark
ls -lh othello_endgame_solver_*

# ない場合はコンパイル
make clean && make
```

**対処2: パスの確認**
```bash
# カレントディレクトリを確認
pwd

# 想定: /home/maelab/your_project_directory
# 実行ファイルと experiments/ が同じ階層にあること
```

**対処3: 改行コード再確認**
```bash
# もしWSLで転送した場合、再度変換
cd experiments
for script in *.sh; do
    sed -i 's/\r$//' "$script"
done
```

### 問題: tsp が見つからない

```bash
# インストール
sudo apt-get update
sudo apt-get install task-spooler

# 確認
which tsp
tsp
```

### 問題: メモリ不足

```bash
# 他のプロセス確認
top -u maelab

# メモリ状況確認
free -h

# 必要に応じてスレッド数削減
# exp*.sh の FIXED_THREADS や THREAD_COUNTS を編集
```

---

## 実験実行のチェックリスト

### 🔲 事前準備
- [ ] ファイルを本番環境に転送
- [ ] SSH接続確認
- [ ] プロジェクトディレクトリ移動

### 🔲 テスト実行
- [ ] `bash experiments/quick_test.sh` 成功
- [ ] `bash experiments/utils/check_environment.sh` 成功
- [ ] （オプション）`bash exp1_mini_test.sh` 成功

### 🔲 本番投入
- [ ] ディスク容量確認（100GB以上）
- [ ] 他ユーザーの負荷確認（`top` で確認）
- [ ] 実験期間の確保（--quick: 20-40時間）
- [ ] `bash experiments/run_all_experiments.sh --quick` 実行

### 🔲 モニタリング
- [ ] `tsp` で定期的にジョブ状態確認
- [ ] `tsp -t` でリアルタイムログ監視
- [ ] `du -sh results/` で容量監視

### 🔲 完了後
- [ ] 全ジョブ完了確認（`tsp` で State: finished）
- [ ] 結果ファイル確認（`ls -lh results/*.csv`）
- [ ] サマリー確認（`cat results/*_summary.txt`）
- [ ] バックアップ作成（`tar -czf results_backup.tar.gz results/`）
- [ ] ローカルにコピー（`scp` または `rsync`）

---

## スクリプト一覧と概要

### 実行スクリプト

| ファイル | 目的 | 実行時間 | 優先度 |
|---------|------|---------|--------|
| `quick_test.sh` | 動作確認テスト | 5-10分 | 最優先 |
| `run_all_experiments.sh` | 全実験の一括投入 | モードによる | - |
| `exp1_basic_comparison.sh` | 基本性能比較 | 4-8時間 | ★★★★★ |
| `exp2_strong_scaling.sh` | 強スケーリング | 12-24時間 | ★★★★★ |
| `expA_lock_comparison.sh` | ロック方式比較 | 6-12時間 | ★★★★★ |
| `exp4_weak_scaling.sh` | 弱スケーリング | 6-12時間 | ★★★☆☆ |
| `exp5_load_balance.sh` | 負荷分散評価 | 4-8時間 | ★★★★☆ |
| `exp6_numa_effects.sh` | NUMA効果 | 6-10時間 | ★★★☆☆ |
| `expB_local_global_ratio.sh` | LocalHeap効果 | 4-6時間 | ★★★★☆ |
| `expE_difficulty_variance.sh` | 難易度ばらつき | 20-30時間 | ★★★★☆ |
| `expF_tt_hit_rate_analysis.sh` | 置換表ヒット率 | 6-10時間 | ★★★★☆ |

### ユーティリティスクリプト

| ファイル | 目的 |
|---------|------|
| `utils/check_environment.sh` | 環境チェック |

### ドキュメント

| ファイル | 内容 |
|---------|------|
| `README.md` | 実験の詳細説明 |
| `TEST_PROCEDURE.md` | テスト手順書 |
| `TEST_STATUS.md` | このファイル |

---

## 問い合わせ

問題が発生した場合:
1. `TEST_PROCEDURE.md` のトラブルシューティングを確認
2. ログファイル（`results/logs/`）を確認
3. `bash -x スクリプト名` でデバッグ実行

---

**最終更新:** 2025年12月15日 14:50
**ステータス:** ✅ WSLテスト完了、本番環境テスト準備完了
**次のアクション:** 本番環境での `quick_test.sh` 実行
