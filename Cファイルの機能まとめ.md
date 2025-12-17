# Cファイルの機能まとめ

このディレクトリには、オセロの終盤ソルバーを実装した3つのCファイルが含まれています。

## 1. Deep_Pns_benchmark.c

### 概要
DeepPN（Deep Proof Number）アルゴリズムを用いたオセロ終盤ソルバーのベンチマーク版。逐次実行（シングルスレッド）で動作します。

### 主要な機能

#### アルゴリズム
- **DeepPN探索**: 証明数探索（Proof Number Search）に深さ情報を組み込んだ探索手法
  - `dpn = proof * (1 - R) + proof * deep * R`
  - R=1で証明数探索、R=0で深さ優先探索
  - 深層値（deep）: `1/(60-depth)` で計算

#### 主要な関数
- `pns()`: 証明数探索のメインルーチン
- `pns_search()`: ノードの探索処理
- `generate_children()`: 子ノードの生成（全合法手を展開）
- `update_proof_disproof()`: 証明数・反証数の更新
  - ORノード: 子の最小pn、子のdnの和
  - ANDノード: 子のpnの和、子の最小dn
- `DPN()`: Deep Proof Number値の計算
- `sort_children()`: 子ノードをDPN値でソート

#### データ構造
- **node_t**: 探索木のノード
  - bitboard（黒・白の配置）
  - proof/disproof（証明数・反証数）
  - dpn（Deep Proof Number）
  - 親子ポインタ
- **hash_t**: 置換表（Transposition Table）
  - チェイン法でハッシュ衝突を解決
  - 盤面状態をキーに証明数を保存

#### その他の機能
- ファイルからポジション読み込み
- タイムアウト機能（実行時間制限）
- ベンチマーク結果のパース可能な出力形式

---

## 2. othello_endgame_solver_workstealing.c

### 概要
Work Stealing方式による並列化を実装したオセロ終盤ソルバー。負荷分散を改善し、マルチコア環境で高速化を図ります。

### 主要な機能

#### アルゴリズム
- **Work Stealing並列化**
  - 固定数のワーカースレッド
  - グローバルタスクキュー（優先度ヒープ）
  - アイドル状態のワーカーが他のタスクを盗む
  - 勝利手発見時の早期終了

#### 並列化の仕組み
- **動的タスクスポーン**: 探索中に新しいタスクを生成
  - 最大世代数（SPAWN_MAX_GENERATION）
  - 最小深さ（SPAWN_MIN_DEPTH）
  - ノードあたりの最大スポーン数（SPAWN_LIMIT_PER_NODE）
- **タスクキュー**: バイナリヒープによる優先度管理
  - 評価スコアに基づく優先順位付け
  - スレッドセーフな push/pop 操作

#### データ構造
- **Task**: 並列タスクの単位
  - player/opponent（盤面状態）
  - priority（優先度）
  - eval_score（評価スコア）
  - generation（タスク世代）
- **TaskQueue**: スレッドセーフなタスクキュー
  - mutex/condition variable による同期
  - ヒープベースの優先度管理

#### 機能
- 置換表（Transposition Table）
  - ストライプロック（TT_LOCK_STRIPES）で競合を軽減
  - アトミック操作による高速化
- デバッグ・統計機能
  - スレッド活動追跡
  - Work Stealing統計
  - CSV/JSON形式での結果出力

---

## 3. othello_endgame_solver_hybrid_check_tthit_fixed.c

### 概要
LocalHeap + GlobalChunkの2層構造による高度なハイブリッド並列化を実装したオセロ終盤ソルバー。最も高度な並列化手法を採用しています。

### 主要な機能

#### アルゴリズム
- **ハイブリッド並列化**
  - **LocalHeap**: 各ワーカーのローカルタスクキュー
    - ロックフリーな push/pop（所有者のみアクセス）
    - キャッシュ効率が高い
  - **GlobalChunkQueue**: グローバル共有キュー
    - チャンク単位（CHUNK_SIZE個）でのタスク移動
    - ロックオーバーヘッドを削減
  - **SharedTaskArray**: 起動時・終盤フェーズ用の共有配列

#### 並列化戦略
- **優先度ベースのタスク移行**
  - LocalHeapが閾値を超えたら高優先度タスクをGlobalへエクスポート
  - アイドル時はGlobalからインポート
- **評価関数統合**
  - 評価スコアによる手の優先順位付け
  - 評価関数影響分析機能（EvalImpact）

#### データ構造
- **LocalHeap**: ワーカー専用ヒープ（配列ベース）
- **Chunk**: タスクのバッチ処理単位（CHUNK_SIZE個）
- **GlobalChunkQueue**: チャンクのキュー
- **EvalImpact**: 評価関数の影響を追跡
  - 各手の評価スコアと実際の探索結果を記録
  - 評価関数の精度を分析

#### 高度な機能
- **早期終了**: 勝利手が見つかったら即座に終了
- **置換表ヒット追跡**: TT効率の詳細分析
- **リアルタイム監視**: 探索中の統計情報を表示
- **評価関数影響分析**: 評価関数がどの程度正確に最善手を予測したかを分析

#### パラメータ調整
- コンパイル時固定パラメータ
  - MAX_THREADS, TT_SIZE_MB, CHUNK_SIZE, LOCAL_HEAP_CAPACITY
- 実行時変更可能パラメータ
  - SPAWN_MAX_GENERATION (-G)
  - SPAWN_MIN_DEPTH (-D)
  - SPAWN_LIMIT_PER_NODE (-S)

---

## アルゴリズムの比較

| ファイル | 並列化方式 | 主な特徴 | 用途 |
|---------|-----------|---------|------|
| Deep_Pns_benchmark.c | なし（逐次） | DeepPN、置換表 | ベンチマーク、基準測定 |
| othello_endgame_solver_workstealing.c | Work Stealing | グローバルキュー、動的負荷分散 | 中規模並列（4-16コア） |
| othello_endgame_solver_hybrid_check_tthit_fixed.c | ハイブリッド2層 | Local+Global、評価関数統合 | 大規模並列（40コア以上） |

---

## 共通の技術要素

### 1. 証明数探索（Proof Number Search）
全てのソルバーで使用される基本アルゴリズム：
- ORノード（自分の手番）: いずれかの子が証明されれば証明
- ANDノード（相手の手番）: 全ての子が反証されれば反証
- 証明数・反証数を伝播させて探索を効率化

### 2. Bitboard表現
- 64ビット整数で盤面を表現（1ビット=1マス）
- 高速なビット演算で合法手生成や石の反転を実行

### 3. 置換表（Transposition Table）
- 盤面状態をハッシュ化して保存
- 同一局面の再探索を回避
- 探索効率を大幅に向上

### 4. 評価関数
- 序盤・中盤の局面評価（ファイルから読み込み可能）
- 手の優先順位付けに使用
- より良い手を先に探索することで枝刈り効果

---

## コンパイル・実行例

### Deep_Pns_benchmark.c
```bash
gcc -O3 -march=native -o deep_pns_benchmark Deep_Pns_benchmark.c -lm
./deep_pns_benchmark position.txt 300
```

### othello_endgame_solver_workstealing.c
```bash
gcc -O3 -march=native -pthread -o solver_ws othello_endgame_solver_workstealing.c -lm
./solver_ws position.txt 8 60.0 eval.dat -G 3 -D 6 -S 3
```

### othello_endgame_solver_hybrid_check_tthit_fixed.c
```bash
gcc -O3 -march=native -pthread -o solver_hybrid othello_endgame_solver_hybrid_check_tthit_fixed.c -lm
./solver_hybrid position.txt 40 60.0 eval.dat -G 4 -D 5 -S 5 -v -e
```

---

## まとめ

これらのプログラムは、オセロの終盤問題を完全解析するための高性能ソルバー群です。逐次版から高度な並列化版まで段階的に実装されており、並列化技術とゲーム木探索の最適化手法が組み合わされています。特にハイブリッド版は、40コア以上の大規模マルチコア環境で最高のパフォーマンスを発揮するよう設計されています。
