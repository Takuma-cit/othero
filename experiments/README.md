# オセロ終盤ソルバー 包括的実験スクリプト群

768コアAMD EPYC環境での並列df-pn+オセロソルバーの包括的評価実験

## 概要

本ディレクトリには、卒業論文「低競合ハイブリッドWork-Stealingフレームワークを用いた並列df-pn+探索の実装と評価」のための包括的な実験スクリプトが含まれています。

**実験環境：**
- マシン: AMD EPYC 9965 192-Core Processor × 2ソケット
- 総コア数: 768コア（384物理コア × 2 SMT）
- メモリ: 2.2TB
- NUMA: 2ノード構成
- ジョブ管理: tsp（Task Spooler）

**テスト局面:**
- ディレクトリ: `test_positions/`
- フォーマット: `empties_XX_id_YYY.pos` (XX=01-60, YYY=000-999)
- 空きマス範囲: 1-60

## 実験の全体構成

### Phase 1: 必須実験（全モードで実行）★★★★★

| 実験ID | スクリプト名 | 目的 | 所要時間 | 優先度 |
|--------|------------|------|---------|--------|
| 実験1 | `exp1_basic_comparison.sh` | 3手法の基本性能比較 | 4-8時間 | ★★★★★ |
| 実験2 | `exp2_strong_scaling.sh` | スケーラビリティ測定 | 12-24時間 | ★★★★★ |
| 実験A | `expA_lock_comparison.sh` | ロック方式比較（新規性の核心） | 6-12時間 | ★★★★★ |

### Phase 2: 主要実験（standardモード以上）★★★★☆

| 実験ID | スクリプト名 | 目的 | 所要時間 | 優先度 |
|--------|------------|------|---------|--------|
| 実験5 | `exp5_load_balance.sh` | 負荷分散品質評価 | 4-8時間 | ★★★★☆ |
| 実験B | `expB_local_global_ratio.sh` | LocalHeap効果測定 | 4-6時間 | ★★★★☆ |
| 実験E | `expE_difficulty_variance.sh` | 問題難易度ばらつき分析 | 20-30時間 | ★★★★☆ |
| 実験F | `expF_tt_hit_rate_analysis.sh` | 置換表ヒット率分析 | 6-10時間 | ★★★★☆ |

### Phase 3: 追加実験（fullモード）★★★☆☆

| 実験ID | スクリプト名 | 目的 | 所要時間 | 優先度 |
|--------|------------|------|---------|--------|
| 実験4 | `exp4_weak_scaling.sh` | 弱スケーリング検証 | 6-12時間 | ★★★☆☆ |
| 実験6 | `exp6_numa_effects.sh` | NUMA効果分析 | 6-10時間 | ★★★☆☆ |

## クイックスタート

### 1. 環境準備

```bash
cd experiments

# 環境チェック
bash utils/check_environment.sh
```

必要要件:
- CPU: 64コア以上（推奨: 768コア）
- メモリ: 32GB以上（推奨: 500GB以上）
- ツール: `tsp`, `numactl`, `bc`
- テスト局面: `test_positions/` ディレクトリに配置済み

### 2. 実験実行モード

**最小限の実験（20-40時間）:**
```bash
bash run_all_experiments.sh --quick
# 実験1, 2, A のみ実行
```

**標準的な実験（50-80時間）:**
```bash
bash run_all_experiments.sh --standard
# 実験1, 2, 5, A, B, E, F を実行
```

**全実験（80-120時間）:**
```bash
bash run_all_experiments.sh --full
# 全実験を実行
```

### 3. 進捗確認

```bash
# ジョブ一覧
tsp

# 最新ジョブのリアルタイム監視
tsp -t

# 結果確認
ls -lh results/
```

## 実験の詳細説明

### 実験1: 基本性能比較

**目的:** 3つの実装（逐次版、Work-Stealing版、Hybrid版）の基本性能を固定スレッド数（64スレッド）で比較

**測定項目:**
- 解決時間（秒）
- 探索ノード数
- NPS (Nodes Per Second)
- Worker稼働率

**テスト局面:** 空きマス10, 12, 14（各2問）

**出力:**
```
results/exp1_results.csv
results/exp1_summary.txt
results/logs/exp1_YYYYMMDD_HHMMSS/
```

**論文での使用:**
> 表1に、3手法の基本性能比較を示す。Hybrid版は逐次版に対してX倍のスピードアップを達成し、Work-Stealing版のY倍を上回る性能を示した。

---

### 実験2: 強スケーリング（最重要）

**目的:** 固定問題サイズでコア数を増やした時のスケーラビリティ測定

**スレッド数:** 1, 2, 4, 8, 16, 32, 64, 128, 192, 256, 384, 512, 768

**測定項目:**
- Speedup = T(1) / T(p)
- 並列効率 = Speedup / p × 100%
- 並列オーバーヘッド

**試行回数:** 各条件3回（統計的信頼性確保）

**出力:**
```
results/exp2_workstealing_scaling.csv
results/exp2_hybrid_scaling.csv
results/exp2_speedup_data.csv  # グラフ作成用
results/exp2_combined_summary.txt
```

**論文での使用:**
> 図2に、コア数に対するスピードアップ曲線を示す。Hybrid版は768コアにおいてX倍のスピードアップを達成し、並列効率Y%を維持した。これはWork-Stealing版のZ倍（効率W%）を上回る性能である。

**期待される結果:**
- Hybrid版: 768コアで60-80%の効率維持
- Work-Stealing版: 256コア以降で効率低下
- 超線形スピードアップの可能性（TTキャッシュ効果）

---

### 実験A: ロック方式比較（論文の核心）

**目的:** ロックフリー/低競合設計の効果を実証

**比較対象:**
1. Hybrid版（LocalHeapロックフリー + GlobalChunk粗粒度ロック）
2. Work-Stealing版（Globalキューのみ、全操作でmutex）

**測定項目:**
- Local操作比率（期待値: 80-95%）
- Global操作比率
- スケーラビリティ比較
- Export/Import頻度

**出力:**
```
results/expA_comparison.csv
results/expA_scalability.csv
results/expA_summary.txt
```

**論文での使用:**
> LocalHeapの完全ロックフリー化とGlobalChunkQueueの粗粒度ロック戦略により、768コアにおいてX倍のスピードアップを達成した。これは、従来のWork-Stealing版（Y倍）をZ%上回る性能である。Local操作比率の測定結果から、タスク操作の大部分（80-95%）がロックフリーなLocalHeapで処理されていることが確認できた。

**新規性の主張ポイント:**
1. ハイブリッド設計の有効性（ホットパスのロックフリー化）
2. 実用的なトレードオフ（完全ロックフリーの複雑さを避ける）
3. 大規模並列環境での実証（768コア）

---

### 実験4: 弱スケーリング

**目的:** Gustafson's Lawに基づく弱スケーリング測定

**設定:**
| スレッド数 | 空きマス数 | 問題サイズ比 |
|-----------|-----------|------------|
| 1 | 8 | 1x |
| 4 | 9 | ~4x |
| 16 | 10 | ~16x |
| 64 | 11 | ~64x |
| 256 | 12 | ~256x |
| 768 | 13 | ~768x |

**測定:** 並列効率 = T(1) / T(p) × 100%（理想は100%維持）

**出力:**
```
results/exp4_weak_scaling.csv
results/exp4_summary.txt
```

**論文での使用:**
> Gustafson's Lawに基づき、スレッド数の増加に応じて問題サイズを比例的に拡大した。768コアにおいて並列効率X%を達成し、大規模問題に対しても高い並列性能を維持できることを実証した。

---

### 実験5: 負荷分散評価

**目的:** Work-Stealingの負荷分散効果を定量測定

**測定項目:**
- スレッド別処理ノード数の分散
- 変動係数（CV: Coefficient of Variation）
- スティール成功率・失敗率
- アイドル時間の割合
- Max/Minノード数の比率

**出力:**
```
results/exp5_load_balance.csv
results/exp5_per_thread_stats.csv
results/exp5_summary.txt
```

**論文での使用:**
> 768コアにおいて、Hybrid版の変動係数はX、アイドル時間率はY%であった。これは、Work-Stealingによる動的負荷分散が効果的に機能していることを示している。

**負荷不均衡指標:**
```
CV (変動係数) = StdDev / 平均ノード数
理想値: 0 (完全均等分散)
許容値: < 0.2
```

---

### 実験6: NUMA効果の測定

**目的:** 2ソケットNUMA環境での性能特性を評価

**NUMAポリシー:**
1. デフォルト（OS任せ）
2. 単一NUMAノード固定（384コア以下）: `numactl --cpunodebind=0 --membind=0`
3. メモリインターリーブ: `numactl --interleave=all`

**測定項目:**
- リモートメモリアクセス率
- NUMA間のタスクマイグレーション影響

**出力:**
```
results/exp6_numa_effects.csv
results/exp6_summary.txt
```

**論文での使用:**
> 768コア実行時、デフォルト設定ではX秒を要したが、メモリインターリーブ設定によりY秒に短縮され、Z%の性能改善を達成した。

---

### 実験B: LocalHeap効果の定量測定

**目的:** Hybrid版のLocalHeapがどれだけ利用されているかを測定

**測定項目:**
- Local操作比率 = (LocalPush + LocalPop) / 全操作 × 100%
- Global Export/Import頻度
- LocalHeap容量の影響
- Overflow率

**仮説:** Local操作が80-95%を占める

**出力:**
```
results/expB_local_global_ratio.csv
results/expB_capacity_test.csv
results/expB_summary.txt
```

**論文での使用:**
> Hybrid版において、タスク操作のX%がローカルヒープで処理され、グローバルキューへのアクセスはY%に抑えられた。これにより、768コア環境においても、ロック競合による性能劣化を最小限に抑えることができた。LocalHeap容量はZ個が最適であり、Overflow率はW%に抑えられた。

---

### 実験E: 問題難易度のばらつき分析【新規】

**目的:** 同じ空きマス数でも問題によって難易度が異なることを定量化し、並列化手法のロバスト性を評価

**測定項目:**
- 同一空きマス数での実行時間・ノード数の分散
- 変動係数（CV）によるロバスト性評価
- 最難問題と最易問題の比率（Max/Min Ratio）
- 各ソルバーの安定性

**テスト:** 空きマス12, 14, 16, 18, 20（各30問 - 統計的信頼性向上）

**推定時間:** 20-30時間

**出力:**
```
results/expE_variance_by_empties.csv
results/expE_solver_robustness.csv
results/expE_summary.txt
```

**論文での使用:**
> 図Xに、問題難易度のばらつきを示す。同一の空きマス数でも、最難問題と最易問題では実行時間がX倍異なることが確認された。Hybrid版の変動係数はYであり、Work-Stealing版と同程度のロバスト性を示した。これは、提案手法が特定の問題構造に依存せず、広範な問題に対して安定した性能を発揮できることを示している。

**実用的意義:**
並列ソルバーの実用性には、最良ケース性能だけでなく、最悪ケース性能とロバスト性（ばらつきの小ささ）も重要。本実験により、提案手法が全ての側面で優れていることを実証。

---

### 実験F: 置換表ヒット率の詳細分析【新規】

**目的:** 置換表（Transposition Table）の効果を定量測定

**測定項目:**
- 空きマス数とTTヒット率の関係
- 並列度とTTヒット率の関係
- TTの衝突率・置換率
- 複数問題での平均ヒット率

**テスト:** 空きマス10-20

**推定時間:** 6-10時間

**出力:**
```
results/expF_tt_size_effect.csv
results/expF_tt_empties_effect.csv
results/expF_summary.txt
```

**論文での使用:**
> 図Yに、空きマス数と置換表ヒット率の関係を示す。空きマス12で約X%のヒット率を達成し、探索の重複を大幅に削減できた。Hybrid版とWorkStealing版でヒット率に有意な差は見られず、提案手法の置換表実装は従来手法と同等の効果を維持していることが確認された。

**並列環境での置換表:**
- 利点: 全スレッドで共有 → 探索効率向上
- 課題: 競合アクセス → ロックフリー設計が必要
- 実装: CAS操作によるAlways-Replace戦略

---

## 実行方法の詳細

### 個別実験の実行

```bash
cd experiments

# 実験1のみ実行
bash exp1_basic_comparison.sh

# 実験2のみ実行（最重要）
bash exp2_strong_scaling.sh

# 実験Aのみ実行（ロック方式比較）
bash expA_lock_comparison.sh
```

### tspによる順次実行

実験スクリプトは `tsp` (Task Spooler) を使用して順次実行されます。

```bash
# 現在のジョブ状態確認
tsp

# 出力例:
# ID   State      Output               E-Level  Times(r/u/s)   Command
#  0   running    /tmp/ts-out.ABC123   0        1:23:45/...    bash exp1_...
#  1   queued     (file)               -        -              bash exp2_...

# 特定ジョブのログ確認
tsp -c 0

# 最新ジョブのリアルタイム表示
tsp -t

# 全ジョブ完了待機
tsp -w
```

### 実行フォーマット（run_full.sh準拠）

全ての並列実験は以下の形式で実行されます:

```bash
numactl --interleave=all \
  timeout $((TIME_LIMIT + 60)) \
  ./ソルバー名 テスト局面.pos スレッド数 制限時間 eval.dat -v
```

例:
```bash
numactl --interleave=all \
  timeout 360 \
  ./othello_endgame_solver_hybrid \
  test_positions/empties_12_id_000.pos \
  768 300.0 eval/eval.dat -v
```

## 結果ファイル

### ディレクトリ構造

```
experiments/
├── results/
│   ├── exp1_results.csv
│   ├── exp1_summary.txt
│   ├── exp2_speedup_data.csv
│   ├── expA_comparison.csv
│   ├── expE_variance_by_empties.csv
│   ├── expF_tt_empties_effect.csv
│   └── logs/
│       ├── exp1_20250115_123456/
│       │   ├── master.log
│       │   ├── Sequential_empties_12_id_000.log
│       │   └── ...
│       └── exp2_20250115_140000/
│           └── ...
└── test_positions/
    ├── empties_01_id_000.pos
    ├── empties_12_id_000.pos
    └── ...
```

### CSVフォーマット

**exp1_results.csv:**
```csv
Solver,Position,Empties,Result,Time_Sec,Total_Nodes,NPS,Worker_Util,Subtasks,Status
Sequential,empties_12_id_000.pos,12,WIN,45.23,1234567,27289,0,0,SOLVED
WorkStealing,empties_12_id_000.pos,12,WIN,2.15,1245000,578837,92.5,1234,SOLVED
Hybrid,empties_12_id_000.pos,12,WIN,1.89,1238000,655026,94.2,1156,SOLVED
```

**exp2_speedup_data.csv:**
```csv
Threads,Solver,Avg_Time,Avg_Speedup,Avg_Efficiency,Ideal_Speedup,Amdahl_Speedup
1,Hybrid,45.234,1.000,100.00,1,1.000
768,Hybrid,0.234,193.231,25.16,768,10.000
```

## トラブルシューティング

### tspが見つからない

```bash
sudo apt-get install task-spooler
```

### numactlが見つからない

```bash
sudo apt-get install numactl
```

### メモリ不足

```bash
# メモリ使用状況確認
free -h

# スレッド数を削減して実験
# exp2_strong_scaling.sh の THREAD_COUNTS を編集
```

### 実験が途中で停止

```bash
# ログを確認
tail -n 50 results/logs/exp2_YYYYMMDD_HHMMSS/master.log

# 必要に応じてスクリプトを再実行（途中から続行可能）
bash exp2_strong_scaling.sh
```

## 推定実行時間まとめ

| モード | 実験数 | 推定時間 | 含まれる実験 |
|--------|--------|----------|--------------|
| `--quick` | 3 | 20-40時間 | 1, 2, A |
| `--standard` | 7 | 60-100時間 | 1, 2, 5, A, B, E, F |
| `--full` | 9 | 100-140時間 | 1, 2, 4, 5, 6, A, B, E, F |

## 論文執筆への活用

### 必須図表

1. **表1: 基本性能比較（実験1）**
   - 3つの実装の実行時間・NPS比較

2. **図2: 強スケーリング曲線（実験2）**
   - X軸: スレッド数（対数スケール）
   - Y軸: スピードアップ
   - 理想線とAmdahlの法則を併記

3. **図3: ロック方式別スケーラビリティ（実験A）**
   - Hybrid版とWorkStealing版の比較

4. **表2: Local/Global操作比率（実験B）**
   - 新規性を示す重要データ

### 推奨図表

5. **図4: 弱スケーリング（実験4）**
6. **図5: 負荷分散評価（実験5）**
7. **図6: 問題難易度のばらつき（実験E）**
8. **図7: 置換表ヒット率（実験F）**
9. **図8: NUMA効果（実験6）**

## まとめ

この実験スクリプト群により、以下を実証できます:

1. **基本性能**: Hybrid版がWork-Stealing版を上回る（実験1）
2. **スケーラビリティ**: 768コアで高効率を維持（実験2）
3. **ロックフリー設計の効果**: Local操作比率80-95%（実験A, B）
4. **負荷分散**: Work-Stealingの効果（実験5）
5. **ロバスト性**: 幅広い問題で安定性能（実験E）
6. **置換表効果**: 並列環境でのTT効率（実験F）
7. **NUMA最適化**: メモリインターリーブの効果（実験6）
8. **弱スケーリング**: Gustafson's Lawの検証（実験4）

これらのデータを用いて、**「低競合Hybrid Work-Stealingフレームワークによる並列df-pn+探索の実装と評価」**という卒論テーマを完成させることができます。

---

**作成日**: 2025年12月15日
**対象環境**: AMD EPYC 9965 768コア
**推定総実験時間**: 100-140時間（実験E増量により延長）
**論文完成目標**: 実験完了後2-3週間




4つのソルバーを比較:
  | ソルバー         | アルゴリズム | 並列化手法           |
  |------------------|--------------|----------------------|
  | Hybrid_5Features | df-pn+       | 5機能ハイブリッド    |
  | WorkStealing     | df-pn+       | ワークスティーリング |
  | WPNS_TT_Parallel | 弱証明数探索 | TT並列化（Lazy SMP） |
  | Sequential       | df-pn+       | 逐次（ベースライン） |


othello_endgame_solver_hybrid_check_tthit_fixed.c
wpns_tt_parallel.c
を比較するベンチマークを作成してほしいです。

今まで作成していただいた.shのベンチマークの等を参考に作成してほしい
実行する環境は、学内マシン.txtだが、まずこのWSL環境で実行テストするのでパラメータでかえれるようにできるように
そして、実行を再起できるできるような処理はいりません。実験環境では、一度実行したら途中でエンターキーを押すなどの処理ができないからです
空きマスによる局面：test_positions
難しい局面：ffotest
