# 実験スクリプト テスト手順書

## WSL環境でのテスト結果 ✓

**日時:** 2025年12月15日
**状態:** 全スクリプト構文チェック完了

### チェック済みスクリプト

- ✓ exp1_basic_comparison.sh
- ✓ exp2_strong_scaling.sh
- ✓ exp4_weak_scaling.sh
- ✓ exp5_load_balance.sh
- ✓ exp6_numa_effects.sh
- ✓ expA_lock_comparison.sh
- ✓ expB_local_global_ratio.sh
- ✓ expE_difficulty_variance.sh
- ✓ expF_tt_hit_rate_analysis.sh
- ✓ run_all_experiments.sh
- ✓ utils/check_environment.sh

### 実施した修正

1. **改行コード変換**: Windows形式（CRLF）→ Unix形式（LF）
2. **構文チェック**: `bash -n` で全スクリプト検証済み

---

## 本番環境（202.17.18.122）でのテスト手順

### Phase 1: 環境確認

```bash
# SSH接続
ssh maelab@202.17.18.122

# experimentsディレクトリに移動
cd /path/to/files/experiments

# 環境チェック実行
bash utils/check_environment.sh
```

**期待される結果:**
- CPU: 768コア検出
- メモリ: 2TB以上確認
- tsp: インストール確認
- 実行ファイル: 3つのバイナリ存在確認
- テスト局面: test_positions/ ディレクトリ確認

**必須要件:**
- ✓ CPU: 256コア以上
- ✓ メモリ: 100GB以上
- ✓ tsp (Task Spooler) インストール済み
- ✓ numactl インストール済み
- ✓ 実行ファイル: Deep_Pns_benchmark, othello_endgame_solver_workstealing, othello_endgame_solver_hybrid

---

### Phase 2: ドライラン（実際には実行しない）

各スクリプトが正しく引数を解析できるか確認：

```bash
# スクリプトのヘルプ表示チェック（実行前に内容確認）
head -50 exp1_basic_comparison.sh
head -50 exp2_strong_scaling.sh
head -50 expA_lock_comparison.sh

# 設定値の確認
grep "THREADS=" exp*.sh
grep "TIME_LIMIT=" exp*.sh
grep "TEST_POSITION" exp*.sh
```

---

### Phase 3: 最小テスト実行

**テスト内容:** 最も軽量なテストで動作確認

#### 3-1: 単一局面での動作確認

```bash
# テスト局面の確認
ls -lh test_positions/empties_10_id_000.pos

# Hybrid版の単一実行テスト（1分以内に完了するはず）
numactl --interleave=all \
  timeout 120 \
  ./othello_endgame_solver_hybrid \
  test_positions/empties_10_id_000.pos \
  64 60.0 eval/eval.dat -v
```

**期待される出力:**
```
Result: WIN (または LOSE/DRAW)
Total: XXXXX nodes in X.XX sec (XXXXX NPS)
Worker 0: XXXX nodes
Worker 1: XXXX nodes
...
```

**確認ポイント:**
- ✓ プログラムが正常に起動
- ✓ 結果が出力される
- ✓ Worker統計が表示される
- ✓ エラーメッセージがない

#### 3-2: 実験1の最小テスト（1問のみ）

```bash
# 実験1スクリプトを編集して1問のみに制限
cp exp1_basic_comparison.sh exp1_test.sh

# TEST_POSITIONSを1問だけに変更
nano exp1_test.sh
# TEST_POSITIONS=("empties_10_id_000.pos") に変更

# テスト実行
bash exp1_test.sh
```

**実行時間:** 約5-10分

**期待される結果:**
- ✓ results/exp1_results.csv が生成される
- ✓ 3行のデータ（Sequential, WorkStealing, Hybrid）
- ✓ ログファイルが results/logs/ に生成される
- ✓ エラーなく完了

---

### Phase 4: tspテスト

tsp (Task Spooler) が正常に動作するか確認：

```bash
# tspの状態確認
tsp

# 簡単なテストジョブ投入
tsp echo "Test job 1"
tsp echo "Test job 2"
tsp sleep 5

# ジョブ状態確認
tsp

# ログ確認
tsp -c 0
tsp -c 1
```

**確認ポイント:**
- ✓ tspが順次ジョブを実行
- ✓ 完了したジョブの出力が確認できる
- ✓ キューイング機能が正常動作

---

### Phase 5: 実験スクリプトのtsp投入テスト

最小構成で実験スクリプトをtspに投入：

```bash
# まず、実験1の最小テスト版を投入
tsp -L "テスト実験1" bash exp1_test.sh

# 状態確認
tsp

# リアルタイム監視
tsp -t
```

**確認ポイント:**
- ✓ スクリプトがエラーなく開始
- ✓ 途中経過がログに出力される
- ✓ 正常に完了する

---

### Phase 6: run_all_experiments.sh のテスト

#### 6-1: --quick モードのドライラン確認

```bash
# スクリプトの内容確認
bash run_all_experiments.sh --quick 2>&1 | head -100
```

**期待される動作:**
- 環境チェック実行
- 実験1, 2, A が tsp に投入される
- 投入数: 3個

#### 6-2: 実際の投入（準備ができたら）

```bash
# クイックモードで実験を開始
bash run_all_experiments.sh --quick

# 投入されたジョブ確認
tsp

# マスターログ確認
tail -f results/logs/master_YYYYMMDD_HHMMSS.log
```

---

## トラブルシューティング

### エラー: command not found

**原因:** 実行ファイルが見つからない

**対処:**
```bash
# カレントディレクトリ確認
pwd

# 実行ファイルの存在確認
ls -lh Deep_Pns_benchmark
ls -lh othello_endgame_solver_*

# パスが正しいか確認
which numactl
which tsp
```

### エラー: Permission denied

**原因:** 実行権限がない

**対処:**
```bash
chmod +x exp*.sh
chmod +x run_all_experiments.sh
chmod +x utils/check_environment.sh
```

### エラー: テスト局面が見つからない

**原因:** test_positions/ ディレクトリの位置が違う

**対処:**
```bash
# テスト局面の場所確認
find . -name "empties_10_id_000.pos"

# スクリプト内のPOS_DIRを修正
grep "POS_DIR=" exp*.sh
```

### エラー: メモリ不足

**原因:** 他のプロセスがメモリを使用中

**対処:**
```bash
# メモリ使用状況確認
free -h

# 他のユーザーのプロセス確認
top -u maelab

# 必要に応じてスレッド数を削減
# exp*.sh の FIXED_THREADS や THREAD_COUNTS を編集
```

---

## チェックリスト

### 事前準備
- [ ] SSH接続確認
- [ ] experimentsディレクトリ確認
- [ ] 実行権限確認 (`chmod +x *.sh`)
- [ ] 改行コード確認（必要なら `dos2unix` 実行）

### 環境確認
- [ ] `bash utils/check_environment.sh` 実行
- [ ] CPU: 768コア確認
- [ ] メモリ: 2TB確認
- [ ] tsp インストール確認
- [ ] numactl インストール確認
- [ ] 実行ファイル3つ確認
- [ ] test_positions/ ディレクトリ確認

### 最小テスト
- [ ] 単一実行テスト成功
- [ ] exp1_test.sh（1問のみ）成功
- [ ] tsp動作確認
- [ ] 結果ファイル生成確認

### 本番投入準備
- [ ] ディスク容量確認（100GB以上推奨）
- [ ] 他ユーザーの負荷確認
- [ ] 実験期間の確保（--quick: 20-40時間）
- [ ] バックアップ確認

---

## 本番実行の推奨手順

1. **まず --quick モードで開始**
   ```bash
   bash run_all_experiments.sh --quick
   ```
   - 実験1, 2, A のみ（20-40時間）
   - 論文の最低限必要なデータ取得

2. **結果確認後、--standard モードを追加実行**
   ```bash
   bash run_all_experiments.sh --standard
   ```
   - 実験5, B, E, F を追加（+40-60時間）
   - より詳細な分析データ取得

3. **必要に応じて --full モード**
   ```bash
   bash run_all_experiments.sh --full
   ```
   - 実験4, 6 も追加（+20-40時間）
   - 完全なデータセット

---

## 進捗モニタリング

### リアルタイム監視

```bash
# tsp状態を定期的に確認
watch -n 60 tsp

# 最新ジョブのログをリアルタイム表示
tsp -t

# 結果ディレクトリの容量確認
du -sh results/
```

### 完了確認

```bash
# 全ジョブ完了待機
tsp -w

# 結果ファイル一覧
ls -lh results/*.csv
ls -lh results/*.txt

# サマリーファイル確認
cat results/exp1_summary.txt
cat results/exp2_combined_summary.txt
cat results/expA_summary.txt
```

---

## 完了後の作業

1. **結果のバックアップ**
   ```bash
   tar -czf results_backup_$(date +%Y%m%d).tar.gz results/
   ```

2. **ローカルへのコピー**
   ```bash
   # ローカルマシンで実行
   scp -r maelab@202.17.18.122:/path/to/files/experiments/results/ ./
   ```

3. **データ検証**
   - CSV ファイルが正しく生成されているか
   - サマリーファイルに結果が記載されているか
   - ログにエラーがないか

4. **グラフ作成**
   - LibreOffice Calc でCSVを開く
   - スピードアップ曲線を作成
   - 論文用の図表を準備

---

**作成日:** 2025年12月15日
**更新日:** 2025年12月15日
**対象環境:** AMD EPYC 9965 768コア (202.17.18.122)
