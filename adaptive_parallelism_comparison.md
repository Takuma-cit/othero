# 適応的並列度制御：既存研究とあなたの実装の比較分析

## 1. 概要

本文書では、ゲーム木探索における適応的並列度制御の既存研究を詳細に調査し、あなたの実装（othello_endgame_solver_workstealing.c）との比較分析を行う。

---

## 2. 既存研究の適応的並列度制御アルゴリズム

### 2.1 A-STEAL (Agrawal, He, Leiserson, 2006-2008)

**概要**: Fork-Join型並列計算のための適応的Work-Stealingスケジューラ。

**アルゴリズムの核心**:
```
量子(quantum) q における要求プロセッサ数 d_q の計算:

1. 利用率パラメータ δ (典型的には0.8) を設定
2. 前回の量子 q-1 における:
   - 要求数 d_{q-1}
   - 割当数 a_{q-1}  
   - 非スチール使用量 n_{q-1}
   を記録

3. 適応アルゴリズム (MIMD: Multiplicative-Increase, Multiplicative-Decrease):
   if (n_{q-1} >= δ * L * a_{q-1}):  # 高利用率
       d_q = 2 * d_{q-1}              # 倍増（指数的増加）
   else:
       d_q = max(1, d_{q-1} / 2)      # 半減（指数的減少）
```

**特徴**:
- 時間を「量子」に分割し、各量子開始時にプロセッサ要求数を調整
- 利用率に基づく MIMD (倍増/半減) アルゴリズム
- 理論的保証: 完了時間 O(T₁/P + T∞)、空間 O(S₁P)
- **ゲーム木探索への適用**: なし（一般並列計算向け）

**参考論文**:
- Agrawal, He, Leiserson. "Adaptive Work Stealing with Parallelism Feedback." PPoPP 2007, TOCS 2008.

---

### 2.2 Adaptive Cut-off (Duran, Corbalán, Ayguadé, 2008)

**概要**: OpenMPタスク並列性のための適応的カットオフ制御。

**アルゴリズムの核心**:
```
タスク生成決定ロジック:

1. 各タスクに「深さ」カウンタを付与
2. システム負荷を監視:
   - アクティブスレッド数
   - 待機タスク数
   
3. カットオフ決定:
   if (タスク深さ < adaptive_cutoff && キュー内タスク数 < threshold):
       新規タスクを生成（並列化）
   else:
       インライン実行（逐次化）

4. カットオフ値の動的調整:
   if (アイドルスレッドが多い):
       adaptive_cutoff++  # より多くのタスクを生成
   if (キューが過負荷):
       adaptive_cutoff--  # タスク生成を抑制
```

**特徴**:
- タスク生成深さを動的に制御
- システム負荷に基づくフィードバック制御
- オーバーヘッドと並列性のトレードオフを自動調整
- **ゲーム木探索への適用**: 間接的（一般タスク並列向け）

**参考論文**:
- Duran, Corbalán, Ayguadé. "An Adaptive Cut-off for Task Parallelism." SC 2008.

---

### 2.3 BWS / Lin-Hwang スケジューラ (2020)

**概要**: タスク依存グラフ向けの適応的ワーカー管理。

**アルゴリズムの核心**:
```
ワーカー状態遷移:

1. 各ワーカーは3状態を持つ:
   - ACTIVE: タスク実行中
   - STEALING: タスク探索中
   - SLEEPING: 休眠中

2. 適応的ワーカー管理:
   ready_tasks = キュー内の準備完了タスク数
   active_workers = アクティブワーカー数
   
   # 不変条件を維持
   target_workers = min(ready_tasks, max_workers)
   
   if (active_workers < target_workers):
       wake_up_workers(target_workers - active_workers)
   elif (active_workers > target_workers && スチール失敗):
       put_worker_to_sleep()

3. EventCount を使用した効率的な休眠/起床制御
```

**特徴**:
- タスク並列度に応じてワーカー数を動的調整
- 不必要なスピンループを回避しエネルギー効率向上
- 15%の実行時間削減、36%のエネルギー削減を達成
- **ゲーム木探索への適用**: DAG構造向け、AND/OR木未対応

**参考論文**:
- Lin, Hwang et al. "An Efficient Work-Stealing Scheduler for Task Dependency Graph." ICPADS 2020.

---

### 2.4 SPDFPN (Pawlewicz & Hayward, 2013)

**概要**: 並列df-pn探索のスケーラブル実装。**ゲーム木探索に特化した唯一の主要研究**。

**アルゴリズムの核心**:
```
Virtual Proof/Disproof Numbers による並列制御:

1. 仮想証明数/反証数の計算:
   virtual_pn(node) = pn(node) + スレッド数_in_subtree
   virtual_dn(node) = dn(node) + スレッド数_in_subtree

2. ジョブ割り当て (TryRunJob):
   for each スレッド:
       node = root
       while not at_leaf(node):
           # 仮想pn/dnを用いて最も証明しやすい子を選択
           best_child = select_by_virtual_pn_dn(node)
           if (work_budget_exceeded):
               return  # ジョブを開始
           node = best_child
       
       # リーフでジョブ実行
       run_dfpn_with_budget(node, MaxWorkPerJob)

3. 1+ε 閾値法:
   # 分岐切り替えを抑制
   threshold_pn = min(pn, ⌈(1 + ε) * second_best_pn⌉)

4. パラメータ:
   - MaxWorkPerJob: 1回のジョブでの最大MID呼び出し数 (100-500)
   - SplitThreshold: ジョブ分割の閾値
   - ε: 分岐切り替え抑制係数 (0.25)
```

**特徴**:
- 仮想証明数によるスレッド間協調
- ジョブ単位での負荷分散（Work-Stealingではない）
- 16スレッドで74%の並列効率 (Hex)
- **適応的制御**: 固定パラメータ、実行時適応なし

**参考論文**:
- Pawlewicz, Hayward. "Scalable Parallel DFPN Search." CG 2013.

---

### 2.5 SLAW (Guo et al., 2010)

**概要**: Locality-Aware な適応的Work-Stealingスケジューラ。

**アルゴリズムの核心**:
```
局所性を考慮したスチール戦略:

1. プロセッサを階層的にグループ化:
   - 同一L2キャッシュ共有グループ
   - 同一ソケット
   - 異なるソケット

2. スチール先選択の優先順位:
   steal_victim = select_by_hierarchy():
       1st: 同一L2キャッシュグループ内
       2nd: 同一ソケット内
       3rd: 他ソケット

3. 適応的スチール間隔:
   if (最近のスチール成功率が低い):
       steal_interval *= 2  # スチール頻度を下げる
   else:
       steal_interval = 初期値
```

**特徴**:
- メモリ局所性を考慮した適応的スチール
- キャッシュ効率の向上
- **ゲーム木探索への適用**: 一般並列向け

**参考論文**:
- Guo et al. "SLAW: A Scalable Locality-Aware Adaptive Work-Stealing Scheduler." IPDPS 2010.

---

## 3. あなたの実装の適応的並列度制御

### 3.1 実装の概要

あなたの実装（othello_endgame_solver_workstealing.c）は、**世代ベースの階層的タスクスポーン**を採用している。

### 3.2 アルゴリズムの詳細

```c
// あなたの実装のコア制御パラメータ
static int SPAWN_MAX_GENERATION = 3;    // 最大タスク生成世代
static int SPAWN_MIN_DEPTH = 6;         // タスク生成の最小残り深さ
static int SPAWN_LIMIT_PER_NODE = 3;    // ノードあたり最大生成数

// タスク構造体
typedef struct {
    uint64_t player, opponent;
    int root_move;
    int priority;           // 評価値ベースの優先度
    int generation;         // 0=ルート, 1=子, 2=孫, ...
    int depth;              // 残り空きマス数
    NodeType node_type;
} Task;
```

### 3.3 タスクスポーン制御ロジック

```c
// spawn_child_tasks() の制御ロジック（簡略化）

static int spawn_child_tasks(Worker *worker, DFPNNode *node, Task *parent_task) {
    int generation = parent_task->generation;
    
    // 制御条件1: 世代制限
    if (generation >= worker->global->max_generation) 
        return 0;
    
    // 制御条件2: 深さ制限
    if (node->depth < worker->global->min_depth_for_spawn) 
        return 0;
    
    // 各子ノードの優先度計算
    for (int i = 0; i < node->n_children; i++) {
        if (node->type == NODE_OR) {
            // ORノード: 低pn（証明しやすい）を優先
            priority = (PN_INF - child->pn) / 1000 + child->eval_score;
        } else {
            // ANDノード: 低dn（反証しやすい）を優先
            priority = (DN_INF - child->dn) / 1000 - child->eval_score;
        }
    }
    
    // 制御条件3: 優先度閾値
    // 最良の80%以上の優先度を持つ子のみスポーン
    if (priority < best_priority * 0.8) 
        continue;
    
    // 制御条件4: ノードあたりの生成数制限
    if (spawned >= spawn_limit) 
        break;
    
    // サブタスク作成（世代を増加、優先度を調整）
    Task subtask = {
        .generation = generation + 1,
        .priority = priority + 5000 - generation * 1000,  // 世代ペナルティ
        ...
    };
    taskqueue_push(queue, &subtask);
}
```

### 3.4 ワーカースレッド動作

```c
// worker_thread() の動作（簡略化）

while (!shutdown && !found_win) {
    // タイムアウト付きでタスク取得を試行
    if (taskqueue_pop_wait(queue, &task, 100ms)) {
        process_task(worker, &task);
        
        // タスク処理後、有望な子タスクをスポーン
        if (root->children && !proven) {
            spawn_child_tasks(worker, root, &task);
        }
    } else {
        // タスクなし：短時間待機
        usleep(1000);
    }
}
```

---

## 4. 比較分析

### 4.1 比較表

| 側面 | A-STEAL | Adaptive Cut-off | BWS/Lin-Hwang | SPDFPN | あなたの実装 |
|------|---------|------------------|---------------|--------|-------------|
| **対象領域** | 一般Fork-Join | OpenMPタスク | DAGタスク | df-pn/Hex | df-pn+/オセロ |
| **適応の単位** | 時間量子 | タスク生成時 | 継続的 | なし（静的） | タスク生成時 |
| **適応メトリクス** | プロセッサ利用率 | キュー長・深さ | タスク数 | なし | 世代・深さ・優先度 |
| **フィードバック** | MIMD（倍増/半減） | 線形調整 | 閾値比較 | なし | 閾値ベース |
| **ワーカー数制御** | 動的要求 | 固定 | 動的休眠/起床 | 固定 | 固定 |
| **ゲーム木対応** | × | × | × | ○ | ○ |
| **pn/dn統合** | × | × | × | ○（仮想pn/dn） | ○（優先度計算） |

### 4.2 詳細比較

#### 4.2.1 適応の粒度

| 手法 | 適応粒度 | 説明 |
|------|----------|------|
| A-STEAL | 粗い（量子単位） | 数百〜数千ノード処理後に調整 |
| SPDFPN | なし | パラメータは実行前に固定 |
| **あなたの実装** | **細かい（タスク単位）** | 各タスク処理時に子タスク生成を判断 |

**評価**: あなたの実装は、タスク単位で適応的に並列度を制御しており、探索木の局所的な特性に即座に対応できる。

#### 4.2.2 ゲーム木探索への特化

| 手法 | pn/dn活用 | 評価関数統合 | AND/OR木対応 |
|------|-----------|-------------|--------------|
| A-STEAL | × | × | × |
| SPDFPN | ○ 仮想pn/dn | × | ○ |
| **あなたの実装** | **○ 優先度計算** | **○ eval_score統合** | **○** |

**評価**: あなたの実装は、証明数/反証数と評価関数の両方を優先度計算に統合しており、SPDFPNより豊富な情報を活用。

#### 4.2.3 タスク生成制御の比較

**SPDFPN**:
```
仮想証明数 = 実証明数 + サブツリー内スレッド数
→ 暗黙的にスレッド集中を分散
→ 固定パラメータ（MaxWorkPerJob, SplitThreshold）
```

**あなたの実装**:
```
タスク生成条件 = f(世代, 深さ, 優先度)
→ 明示的な階層制御（generation）
→ 探索木形状への適応（depth, priority threshold）
→ 設定可能なパラメータ（-G, -D, -S オプション）
```

**評価**: あなたの実装は、より明示的で制御可能な適応機構を持つ。

---

## 5. あなたの実装の新規性

### 5.1 既存研究との差異

1. **世代ベースの階層制御**: 
   - A-STEALやSPDFPNにはない、タスク世代（generation）による階層的制御
   - 世代に応じた優先度ペナルティ（`- generation * 1000`）

2. **df-pn+とWork-Stealingの融合**:
   - SPDFPNは仮想pn/dnによるジョブ分配（Work-Sharing的）
   - あなたの実装はグローバルキューからのWork-Stealing

3. **評価関数統合型優先度**:
   - 既存手法: pn/dnのみ、または利用率のみ
   - あなたの実装: `priority = f(pn, dn, eval_score, generation, depth)`

4. **多段階フィルタリング**:
   - 世代制限 → 深さ制限 → 優先度閾値 → 生成数制限
   - 既存研究にはない複合的な制御

### 5.2 学術的貢献の可能性

| 貢献 | 内容 | 新規性 |
|------|------|--------|
| 世代ベース制御 | タスク階層を明示的に追跡・制御 | **高** |
| 評価関数統合 | df-pn+の評価とタスク優先度の統合 | **中〜高** |
| 複合的閾値制御 | 多要因による適応的タスク生成 | **中** |
| オセロ終盤特化 | ビットボード・対称性との統合 | **中** |

---

## 6. 改善提案：論文化に向けた強化ポイント

### 6.1 動的パラメータ調整の追加

現在の実装は静的パラメータ（-G, -D, -S）を使用。以下の動的調整を追加すると新規性が向上：

```c
// 提案: 実行時適応的パラメータ調整

typedef struct {
    // 現行の静的パラメータ
    int max_generation;
    int min_depth_for_spawn;
    int spawn_limit;
    
    // 新規: 適応的調整用メトリクス
    double queue_utilization;      // キュー利用率
    double worker_idle_rate;       // ワーカーアイドル率
    int recent_spawn_success;      // 最近のスポーン成功数
    int recent_spawn_pruned;       // 最近のスポーン後枝刈り数
} AdaptiveParams;

void adapt_spawn_parameters(GlobalState *global) {
    // A-STEAL的なMIMD調整
    if (global->queue_utilization < 0.3 && global->worker_idle_rate > 0.5) {
        // 並列性不足：より多くのタスクを生成
        global->max_generation++;
        global->spawn_limit = min(global->spawn_limit + 1, MAX_SPAWN);
    } else if (global->queue_utilization > 0.8) {
        // キュー過負荷：タスク生成を抑制
        global->max_generation = max(global->max_generation - 1, 1);
    }
    
    // 探索効率に基づく調整（新規提案）
    double spawn_efficiency = (double)global->recent_spawn_success / 
                              (global->recent_spawn_success + global->recent_spawn_pruned + 1);
    if (spawn_efficiency < 0.2) {
        // スポーンしたタスクの多くが枝刈り：閾値を厳しく
        global->spawn_threshold *= 1.2;
    }
}
```

### 6.2 実験で示すべき指標

論文化に向けて、以下の指標を実験で示すことを推奨：

1. **並列効率**: スピードアップ / スレッド数
2. **タスク生成効率**: 有効タスク数 / 総タスク数
3. **負荷分散**: ワーカー間のノード数分散
4. **適応性**: パラメータ変化時の性能安定性

---

## 7. 結論

### 7.1 あなたの実装の位置づけ

あなたの実装は、**ゲーム木探索に特化した適応的Work-Stealing**として、以下の点で既存研究と差別化される：

1. A-STEALの適応的フィードバック思想をゲーム木探索に適用
2. SPDFPNの仮想証明数を、より明示的な世代ベース制御に置換
3. df-pn+の評価関数をタスク優先度に統合

### 7.2 推奨される論文タイトル案

- 「Adaptive Generation-Based Work Stealing for Parallel df-pn+ Search」
- 「世代ベース適応的Work-Stealingによる並列df-pn+探索の高速化」

### 7.3 関連論文リスト（引用推奨）

**Work-Stealing基礎**:
1. Blumofe, Leiserson. "Scheduling Multithreaded Computations by Work Stealing." JACM 1999.
2. Chase, Lev. "Dynamic Circular Work-Stealing Deque." SPAA 2005.

**適応的並列度制御**:
3. Agrawal, He, Leiserson. "Adaptive Work Stealing with Parallelism Feedback." TOCS 2008.
4. Duran et al. "An Adaptive Cut-off for Task Parallelism." SC 2008.
5. Lin et al. "An Efficient Work-Stealing Scheduler for Task Dependency Graph." ICPADS 2020.

**並列df-pn**:
6. Pawlewicz, Hayward. "Scalable Parallel DFPN Search." CG 2013.
7. Kaneko. "Parallel Depth First Proof Number Search." J-STAGE 2010.
8. Saito et al. "Randomized Parallel Proof-Number Search." CG 2010.

**df-pn基礎**:
9. Nagai. "df-pn Algorithm for Searching AND/OR Trees." GPW 1998, 1999.
10. Kishimoto et al. "Game-Tree Search Using Proof Numbers: The First Twenty Years." ICGA 2012.
