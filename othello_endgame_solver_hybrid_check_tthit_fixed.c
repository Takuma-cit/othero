/**
 * @file othello_endgame_solver_hybrid.c
 * @brief Othello Endgame Solver with Hybrid LocalHeap + GlobalChunk Parallelization
 *
 * Hybrid implementation combining:
 * - Per-worker LocalHeap (lock-free push/pop for owner)
 * - GlobalChunkQueue with chunk-based operations
 * - SharedTaskArray for startup and endgame phases
 * - Priority-based task migration between Local and Global
 * - Early termination when WIN is found
 * - Evaluation impact tracking (-e option)
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <sys/time.h>
#include <limits.h>
#include <stdatomic.h>

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Lock-free Atomic Operations Helper
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Atomic compare-and-swap for Result type (int8_t)
// Returns true if exchange succeeded
static inline bool atomic_cas_result(volatile int8_t *ptr, int8_t expected, int8_t desired) {
    return __sync_bool_compare_and_swap(ptr, expected, desired);
}

// Atomic load with acquire semantics
static inline int8_t atomic_load_result(volatile int8_t *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

// Atomic store with release semantics
static inline void atomic_store_result(volatile int8_t *ptr, int8_t value) {
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

// Atomic add for uint64_t
static inline uint64_t atomic_add_u64(volatile uint64_t *ptr, uint64_t value) {
    return __sync_fetch_and_add(ptr, value);
}

// Atomic add for int
static inline int atomic_add_int(volatile int *ptr, int value) {
    return __sync_fetch_and_add(ptr, value);
}

// Atomic load for bool
static inline bool atomic_load_bool(volatile bool *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

// Atomic store for bool
static inline void atomic_store_bool(volatile bool *ptr, bool value) {
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

// Atomic compare-and-swap for bool
static inline bool atomic_cas_bool(volatile bool *ptr, bool expected, bool desired) {
    return __sync_bool_compare_and_swap(ptr, expected, desired);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Debug Configuration
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

typedef struct {
    bool enabled;
    bool log_to_file;
    bool verbose;
    bool track_threads;
    bool track_eval_impact;
    bool track_tree_stats;
    bool real_time_monitor;
    bool track_work_stealing;
    bool output_csv;            // Output CSV format results
    bool output_json;           // Output JSON format results
    char log_filename[256];
    char csv_filename[256];
    char json_filename[256];
    FILE *log_file;
    FILE *csv_file;
    FILE *json_file;
    pthread_mutex_t log_mutex;
} DebugConfig;

static DebugConfig DEBUG_CONFIG = {0};

// ============================================================
// 設定パラメータ
// ============================================================
//
// パラメータは2種類に分類されます：
//
// 【コンパイル時固定パラメータ】
//   - メモリレイアウトや配列サイズに関わるため、ビルド時に決定が必要
//   - 変更する場合は再コンパイルが必要
//   - gcc -DMAX_THREADS=64 のようにビルド時に指定可能
//
// 【実行時変更可能パラメータ】
//   - アルゴリズムの動作調整に関わり、実行時に変更可能
//   - コマンドラインオプションで指定可能（-G, -D, -S, -v など）
//   - ここで定義するのはデフォルト値
//
// ============================================================

// ────────────────────────────────────────────────────────────
// 【コンパイル時固定パラメータ】
// メモリ確保・構造体サイズに影響するため、ビルド時に決定
// ────────────────────────────────────────────────────────────

// --- スレッド関連 ---
#ifndef MAX_THREADS
#define MAX_THREADS 128                 // 最大スレッド数（配列サイズに影響）
#endif

// --- チャンク関連 ---
#ifndef CHUNK_SIZE
#define CHUNK_SIZE 16                    // 1チャンクのタスク数（Chunk構造体サイズに影響）
#endif

// --- LocalHeap関連 ---
#ifndef LOCAL_HEAP_CAPACITY
#define LOCAL_HEAP_CAPACITY 1024        // 各LocalHeapの最大サイズ（ワーカーごとの配列サイズ）
#endif

// --- GlobalChunkQueue関連 ---
#ifndef GLOBAL_QUEUE_CAPACITY
#define GLOBAL_QUEUE_CAPACITY 4096      // GlobalQueueの最大チャンク数（ヒープ配列サイズ）
#endif

// --- SharedTaskArray関連 ---
#ifndef SHARED_ARRAY_SIZE
#define SHARED_ARRAY_SIZE 65536         // SharedTaskArrayのサイズ（768コア用に拡大: 64K）
#endif

// --- トランスポジションテーブル関連 ---
#ifndef TT_SIZE_MB
#define TT_SIZE_MB 10240                 // TTサイズ（MB）
#endif

// ────────────────────────────────────────────────────────────
// 【実行時変更可能パラメータ】
// アルゴリズム動作の調整用。ここではデフォルト値を定義。
// 実行時にコマンドラインオプションで上書き可能。
// ────────────────────────────────────────────────────────────

// --- エクスポート/インポート閾値 ---
// 将来的に -E オプション等で実行時指定可能にすることを想定
#ifndef LOCAL_EXPORT_THRESHOLD
#define LOCAL_EXPORT_THRESHOLD (CHUNK_SIZE + 4)  // チャンク数+4 (16+4=20)
#endif

// --- タスクスポーン関連 ---
// 実行時に -G, -D, -S オプションで変更可能
// 40コアチューニング結果: G=4, D=5, S=5 が最短実行時間（235.5秒）
#ifndef DEFAULT_SPAWN_MAX_GENERATION
#define DEFAULT_SPAWN_MAX_GENERATION 1  // タスクスポーンの最大世代（-G で変更）
#endif

#ifndef DEFAULT_SPAWN_MIN_DEPTH
#define DEFAULT_SPAWN_MIN_DEPTH 5       // スポーンする最小残り空きマス数（-D で変更）
#endif

#ifndef DEFAULT_SPAWN_LIMIT_PER_NODE
#define DEFAULT_SPAWN_LIMIT_PER_NODE 9999  // ノードあたりの最大スポーン数（-S で変更）
#endif

// --- デバッグ・統計関連 ---
// 実行時に -v, -w, -t, -s, -m 等のオプションで有効化
//
// 以下のデバッグオプションはコマンドラインで指定:
//   -v            : verbose（詳細出力）
//   -w            : track_work_stealing（ワークスティール追跡）
//   -t            : track_threads（スレッド活動追跡）
//   -e            : track_eval_impact（評価関数影響追跡）
//   -s            : track_tree_stats（探索木統計）
//   -m            : real_time_monitor（リアルタイム監視）
//   -d <file>     : log_to_file（ファイルへログ出力）
//   -c <file>     : output_csv（CSV形式で結果出力）
//   -j <file>     : output_json（JSON形式で結果出力）
//
// これらは DebugConfig 構造体で管理され、実行時に設定される。
// 詳細は上部の DebugConfig 定義を参照。

#ifndef ENABLE_HYBRID_STATS
#define ENABLE_HYBRID_STATS 1           // ハイブリッド統計を有効化 (0/1)
#endif

#ifndef VERBOSE_EXPORT_IMPORT
#define VERBOSE_EXPORT_IMPORT 0         // エクスポート/インポート詳細ログ (0/1)
#endif

// --- Global比較ベンチマークモード ---
// コンパイル時に -DENABLE_GLOBAL_CHECK_BENCHMARK=1 で有効化
// 各スレッドがどれくらいの頻度でGlobalと比較しているかを計測
#ifndef ENABLE_GLOBAL_CHECK_BENCHMARK
#define ENABLE_GLOBAL_CHECK_BENCHMARK 0  // デフォルト無効（オーバーヘッドあり）
#endif

// ────────────────────────────────────────────────────────────
// 【評価関数影響分析機能 (EvalImpact)】
// ────────────────────────────────────────────────────────────
//
// 評価関数が探索にどのような影響を与えたかを分析する機能。
// 実行時の -e オプションと組み合わせて使用。
//
// 【出力内容】
//   - 各手の評価スコアと評価関数による順序
//   - 各手の探索結果 (WIN/LOSE/DRAW/UNKNOWN)
//   - 探索ノード数、時間、NPS
//   - 早期終了によるカットオフの有無
//   - 勝利手が評価関数の最高評価だったかどうか
//
// 【使用例】
//   1. このdefineを1に設定してコンパイル
//   2. 実行時に -e オプションを指定
//      ./othello_solver test.pos 8 60.0 eval.dat -v -e
//
// 【注意】
//   - メモリオーバーヘッド: 各手につき sizeof(EvalImpact) ≒ 64バイト
//   - 実行時オーバーヘッド: 軽微（タイマー取得程度）
//   - 本番環境では無効化推奨（デバッグ・分析用途）
//
#ifndef ENABLE_EVAL_IMPACT
#define ENABLE_EVAL_IMPACT 1             // 評価関数影響分析 (0=無効, 1=有効)
#endif

// Benchmark result structure for output
typedef struct {
    char filename[256];
    int empties;
    int legal_moves;
    char result[16];
    char best_move[4];
    uint64_t total_nodes;
    double time_sec;
    double nps;
    uint64_t tt_hits;
    uint64_t tt_stores;
    uint64_t tt_collisions;
    double tt_hit_rate;
    int spawn_max_gen;
    int spawn_min_depth;
    int spawn_limit;
    uint64_t subtasks_spawned;
    uint64_t subtasks_completed;
    int num_threads;
    int win_count;
    int lose_count;
    int draw_count;
    int unknown_count;
    // Per-worker stats
    uint64_t worker_nodes[MAX_THREADS];
    uint64_t worker_tasks[MAX_THREADS];
} BenchmarkResult;

static BenchmarkResult g_benchmark_result = {0};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Dynamic Task Spawning Configuration
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// These can be adjusted for different hardware configurations
// Defaults are for 4-core systems; for 40-core use: max=5, min_depth=4, limit=6
static int SPAWN_MAX_GENERATION = DEFAULT_SPAWN_MAX_GENERATION;
static int SPAWN_MIN_DEPTH = DEFAULT_SPAWN_MIN_DEPTH;
static int SPAWN_LIMIT_PER_NODE = DEFAULT_SPAWN_LIMIT_PER_NODE;

// Work stealing statistics
typedef struct {
    uint64_t tasks_stolen;
    uint64_t tasks_created;
    uint64_t tasks_completed;
    uint64_t steal_attempts;
    uint64_t steal_failures;
} WorkStealingStats;

// Thread-specific statistics
typedef struct {
    int thread_id;
    char current_move[4];
    int current_depth;
    uint64_t nodes_explored;
    uint64_t tt_hits;
    uint64_t tt_stores;
    uint64_t tasks_processed;   // NEW
    uint64_t tasks_stolen;      // NEW
    int best_eval_score;
    time_t start_time;
    time_t last_update;
    bool is_active;
} ThreadStats;

#if ENABLE_EVAL_IMPACT
// ────────────────────────────────────────────────────────────
// EvalImpact: 評価関数影響分析用のデータ構造
// ────────────────────────────────────────────────────────────
// 各ルートムーブについて、評価関数の評価と実際の探索結果を記録。
// 評価関数が探索をどの程度正しく導いたかを分析するために使用。
//
// Note: Result enum はファイル後半で定義されるため、ここでは int を使用。
//       値の対応: 0=UNKNOWN, 1=WIN, -1=LOSE, 2=DRAW
typedef struct {
    int move;                   // 手の位置 (0-63)
    int eval_score;             // 評価スコア
    int original_order;         // 評価関数による順序 (0=最高評価)
    int final_order;            // 最終的な探索順序
    uint64_t nodes_searched;    // この手の探索ノード数
    double time_spent;          // この手の探索時間(秒)
    int result;                 // 最終結果 (Result enum値)
    int pn_final;               // 最終pn値
    int dn_final;               // 最終dn値
    double nps;                 // この手のNPS
    bool was_cutoff;            // 早期終了でカットされたか
} EvalImpact;
#endif // ENABLE_EVAL_IMPACT

// Search tree statistics
typedef struct {
    uint64_t nodes_by_depth[65];
    uint64_t pn_dn_updates;
    uint64_t expansions;
    uint64_t terminal_nodes;
    uint64_t pass_nodes;
    double avg_branching_factor;
} TreeStats;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Debug Logging Functions
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static void debug_log(const char *format, ...) {
    if (!DEBUG_CONFIG.enabled) return;

    pthread_mutex_lock(&DEBUG_CONFIG.log_mutex);

    va_list args, args_copy;
    va_start(args, format);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t nowtime = tv.tv_sec;
    struct tm *nowtm = localtime(&nowtime);
    char tmbuf[64];
    strftime(tmbuf, sizeof(tmbuf), "%H:%M:%S", nowtm);

    char timestamp[80];
    snprintf(timestamp, sizeof(timestamp), "[%s.%03ld]", tmbuf, tv.tv_usec / 1000);

    if (DEBUG_CONFIG.verbose) {
        va_copy(args_copy, args);
        printf("%s ", timestamp);
        vprintf(format, args_copy);
        fflush(stdout);
        va_end(args_copy);
    }

    if (DEBUG_CONFIG.log_to_file && DEBUG_CONFIG.log_file) {
        fprintf(DEBUG_CONFIG.log_file, "%s ", timestamp);
        vfprintf(DEBUG_CONFIG.log_file, format, args);
        fflush(DEBUG_CONFIG.log_file);
    }

    va_end(args);
    pthread_mutex_unlock(&DEBUG_CONFIG.log_mutex);
}

static void debug_init(const char *log_filename, bool verbose, bool track_threads,
                      bool track_eval, bool track_tree, bool real_time, bool track_ws) {
    DEBUG_CONFIG.enabled = true;
    DEBUG_CONFIG.verbose = verbose;
    DEBUG_CONFIG.track_threads = track_threads;
    DEBUG_CONFIG.track_eval_impact = track_eval;
    DEBUG_CONFIG.track_tree_stats = track_tree;
    DEBUG_CONFIG.real_time_monitor = real_time;
    DEBUG_CONFIG.track_work_stealing = track_ws;

    pthread_mutex_init(&DEBUG_CONFIG.log_mutex, NULL);

    if (log_filename) {
        strncpy(DEBUG_CONFIG.log_filename, log_filename, sizeof(DEBUG_CONFIG.log_filename) - 1);
        DEBUG_CONFIG.log_to_file = true;
        DEBUG_CONFIG.log_file = fopen(log_filename, "w");
        if (!DEBUG_CONFIG.log_file) {
            fprintf(stderr, "Warning: Cannot open log file %s\n", log_filename);
            DEBUG_CONFIG.log_to_file = false;
        } else {
            debug_log("=== Debug Log Started (Work Stealing Version) ===\n");
        }
    }
}

static void debug_close() {
    if (!DEBUG_CONFIG.enabled) return;

    debug_log("=== Debug Log Ended ===\n");

    if (DEBUG_CONFIG.log_file) {
        fclose(DEBUG_CONFIG.log_file);
        DEBUG_CONFIG.log_file = NULL;
    }

    if (DEBUG_CONFIG.csv_file) {
        fclose(DEBUG_CONFIG.csv_file);
        DEBUG_CONFIG.csv_file = NULL;
    }

    if (DEBUG_CONFIG.json_file) {
        fclose(DEBUG_CONFIG.json_file);
        DEBUG_CONFIG.json_file = NULL;
    }

    pthread_mutex_destroy(&DEBUG_CONFIG.log_mutex);
}

// Output benchmark result to CSV file
static void output_csv_result(const BenchmarkResult *r) {
    if (!DEBUG_CONFIG.output_csv) return;

    FILE *f = DEBUG_CONFIG.csv_file;
    if (!f) {
        f = fopen(DEBUG_CONFIG.csv_filename, "a");
        if (!f) return;
        DEBUG_CONFIG.csv_file = f;
    }

    // Check if file is empty (need header)
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) {
        fprintf(f, "Filename,Empties,Legal_Moves,Result,Best_Move,Total_Nodes,Time_Sec,NPS,"
                   "TT_Hits,TT_Stores,TT_Collisions,TT_Hit_Rate,"
                   "Spawn_Max_Gen,Spawn_Min_Depth,Spawn_Limit,"
                   "Subtasks_Spawned,Subtasks_Completed,Num_Threads,"
                   "WIN_Count,LOSE_Count,DRAW_Count,UNKNOWN_Count\n");
    }

    fprintf(f, "%s,%d,%d,%s,%s,%llu,%.6f,%.0f,%llu,%llu,%llu,%.2f,%d,%d,%d,%llu,%llu,%d,%d,%d,%d,%d\n",
            r->filename, r->empties, r->legal_moves, r->result, r->best_move,
            (unsigned long long)r->total_nodes, r->time_sec, r->nps,
            (unsigned long long)r->tt_hits, (unsigned long long)r->tt_stores,
            (unsigned long long)r->tt_collisions, r->tt_hit_rate,
            r->spawn_max_gen, r->spawn_min_depth, r->spawn_limit,
            (unsigned long long)r->subtasks_spawned, (unsigned long long)r->subtasks_completed,
            r->num_threads, r->win_count, r->lose_count, r->draw_count, r->unknown_count);
    fflush(f);
}

// Output benchmark result to JSON file
static void output_json_result(const BenchmarkResult *r) {
    if (!DEBUG_CONFIG.output_json) return;

    FILE *f = DEBUG_CONFIG.json_file;
    if (!f) {
        f = fopen(DEBUG_CONFIG.json_filename, "w");
        if (!f) return;
        DEBUG_CONFIG.json_file = f;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"filename\": \"%s\",\n", r->filename);
    fprintf(f, "  \"empties\": %d,\n", r->empties);
    fprintf(f, "  \"legal_moves\": %d,\n", r->legal_moves);
    fprintf(f, "  \"result\": \"%s\",\n", r->result);
    fprintf(f, "  \"best_move\": \"%s\",\n", r->best_move);
    fprintf(f, "  \"total_nodes\": %llu,\n", (unsigned long long)r->total_nodes);
    fprintf(f, "  \"time_sec\": %.6f,\n", r->time_sec);
    fprintf(f, "  \"nps\": %.0f,\n", r->nps);
    fprintf(f, "  \"transposition_table\": {\n");
    fprintf(f, "    \"hits\": %llu,\n", (unsigned long long)r->tt_hits);
    fprintf(f, "    \"stores\": %llu,\n", (unsigned long long)r->tt_stores);
    fprintf(f, "    \"collisions\": %llu,\n", (unsigned long long)r->tt_collisions);
    fprintf(f, "    \"hit_rate\": %.2f\n", r->tt_hit_rate);
    fprintf(f, "  },\n");
    fprintf(f, "  \"spawn_settings\": {\n");
    fprintf(f, "    \"max_generation\": %d,\n", r->spawn_max_gen);
    fprintf(f, "    \"min_depth\": %d,\n", r->spawn_min_depth);
    fprintf(f, "    \"limit_per_node\": %d\n", r->spawn_limit);
    fprintf(f, "  },\n");
    fprintf(f, "  \"subtasks\": {\n");
    fprintf(f, "    \"spawned\": %llu,\n", (unsigned long long)r->subtasks_spawned);
    fprintf(f, "    \"completed\": %llu\n", (unsigned long long)r->subtasks_completed);
    fprintf(f, "  },\n");
    fprintf(f, "  \"num_threads\": %d,\n", r->num_threads);
    fprintf(f, "  \"result_counts\": {\n");
    fprintf(f, "    \"win\": %d,\n", r->win_count);
    fprintf(f, "    \"lose\": %d,\n", r->lose_count);
    fprintf(f, "    \"draw\": %d,\n", r->draw_count);
    fprintf(f, "    \"unknown\": %d\n", r->unknown_count);
    fprintf(f, "  },\n");
    fprintf(f, "  \"worker_stats\": [\n");
    for (int i = 0; i < r->num_threads; i++) {
        fprintf(f, "    {\"id\": %d, \"nodes\": %llu, \"tasks\": %llu}%s\n",
                i, (unsigned long long)r->worker_nodes[i],
                (unsigned long long)r->worker_tasks[i],
                i < r->num_threads - 1 ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fflush(f);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constants (PN_INF/DN_INF for df-pn algorithm)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

#define PN_INF 100000000
#define DN_INF 100000000

typedef enum {
    RESULT_UNKNOWN = 0,
    RESULT_EXACT_WIN = 1,
    RESULT_EXACT_LOSE = -1,
    RESULT_EXACT_DRAW = 2
} Result;

typedef enum {
    NODE_OR,
    NODE_AND
} NodeType;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Task Structure for Work Stealing
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

typedef struct {
    uint64_t player;
    uint64_t opponent;
    int root_move;          // Original move at root (-1 if not root task)
    int priority;           // For ordering (higher = better)
    int eval_score;
    bool is_root_task;      // True if this is a root-level move

    // For dynamic task spawning (subtasks)
    int depth;              // Remaining empty squares
    NodeType node_type;     // OR or AND node
    int generation;         // Task generation (0=root, 1=child, 2=grandchild, ...)
} Task;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Hybrid Data Structures (LocalHeap + GlobalChunkQueue + SharedTaskArray)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Chunk: グローバルキューで扱う単位
typedef struct {
    Task tasks[CHUNK_SIZE];      // チャンク内のタスク配列
    int count;                   // 実際のタスク数 (1～CHUNK_SIZE)
    int top_priority;            // tasks[0].priority (ソート用)
} Chunk;

// LocalHeap: 各ワーカー専用のヒープ（オーナーのpush/popはロック不要）
typedef struct {
    Task *heap;                  // Binary max-heap array
    int size;                    // 現在のタスク数
    int capacity;                // 最大容量 (LOCAL_HEAP_CAPACITY)

    // 統計
    uint64_t local_pushes;
    uint64_t local_pops;
    uint64_t exported_to_global; // Globalへ送出した数
    uint64_t imported_from_global; // Globalから取得した数
} LocalHeap;

// GlobalChunkQueue: チャンク単位で管理するグローバルキュー
typedef struct {
    Chunk *heap;                 // Binary max-heap (チャンクのtop_priorityで順序付け)
    int size;                    // 現在のチャンク数
    int capacity;                // 最大チャンク数 (GLOBAL_QUEUE_CAPACITY)
    pthread_mutex_t mutex;       // push/pop用
    pthread_cond_t cond;         // タスク追加通知用（usleepポーリング置き換え）

    // ロック不要で参照可能なTop Priority
    _Atomic int top_priority;    // 空の時は INT_MIN

    // 統計
    uint64_t chunks_pushed;
    uint64_t chunks_popped;
} GlobalChunkQueue;

// SharedTaskArray: 探索開始・終盤用のロックフリー配列
typedef struct {
    Task *tasks;                 // タスク配列
    int capacity;                // 最大容量 (SHARED_ARRAY_SIZE)
    _Atomic uint32_t head;       // pop位置
    _Atomic uint32_t tail;       // push位置
} SharedTaskArray;

// WorkerState: ワーカー起動状態追跡（ビットマップ方式 - 1024スレッド対応）
//
// 従来方式: _Atomic int busy_workers
//   → 全スレッドが同じカウンタを更新 → キャッシュラインバウンシング
//
// ビットマップ方式: 各スレッドが自分のビットだけを操作
//   → キャッシュライン競合が大幅に減少（特に400スレッド環境で有効）
//
#define WORKER_BITMAP_WORDS 16  // 16 * 64 = 1024スレッド対応（768コア環境向け拡張）

typedef struct {
    _Atomic int active_workers;                      // 起動済みワーカー数
    _Atomic uint64_t busy_bitmap[WORKER_BITMAP_WORDS]; // ビットマップ（各ビット=1ワーカー）
    int total_workers;                               // 全ワーカー数
    int fast_sharing_threshold;                      // 高速共有モード閾値
} WorkerState;

// 高速共有モード判定用閾値（アクティブワーカーがこの割合以下なら高速共有モード）
#define FAST_SHARING_THRESHOLD 1.0  // 100%稼働まで高速共有モード継続

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// WorkerState Bitmap Operations (lock-free)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// ワーカーをbusy状態にセット
static inline void worker_set_busy(WorkerState *ws, int worker_id) {
    int word = worker_id / 64;
    int bit = worker_id % 64;
    atomic_fetch_or(&ws->busy_bitmap[word], 1ULL << bit);
}

// ワーカーをidle状態にセット
static inline void worker_set_idle(WorkerState *ws, int worker_id) {
    int word = worker_id / 64;
    int bit = worker_id % 64;
    atomic_fetch_and(&ws->busy_bitmap[word], ~(1ULL << bit));
}

// busyワーカー数をカウント（popcountで高速）
static inline int worker_count_busy(WorkerState *ws) {
    int count = 0;
    int words_needed = (ws->total_workers + 63) / 64;
    for (int i = 0; i < words_needed; i++) {
        count += __builtin_popcountll(atomic_load(&ws->busy_bitmap[i]));
    }
    return count;
}

// 暇なワーカーがいるか？（高速判定）
// busy_bitmap の全ビットが立っていなければ暇なワーカーがいる
static inline bool worker_has_idle(WorkerState *ws) {
    int words_needed = (ws->total_workers + 63) / 64;
    int remaining = ws->total_workers;

    for (int i = 0; i < words_needed; i++) {
        uint64_t bitmap = atomic_load(&ws->busy_bitmap[i]);
        int bits_in_word = (remaining >= 64) ? 64 : remaining;
        uint64_t full_mask = (bits_in_word == 64) ? ~0ULL : ((1ULL << bits_in_word) - 1);

        // このワードで暇なワーカーがいるか
        if ((bitmap & full_mask) != full_mask) {
            return true;
        }
        remaining -= 64;
    }
    return false;  // 全員busy
}

// WorkerState初期化
static inline void worker_state_init(WorkerState *ws, int total) {
    atomic_store(&ws->active_workers, 0);
    for (int i = 0; i < WORKER_BITMAP_WORDS; i++) {
        atomic_store(&ws->busy_bitmap[i], 0);
    }
    ws->total_workers = total;
    ws->fast_sharing_threshold = (int)(total * FAST_SHARING_THRESHOLD);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// LocalHeap Operations (lock-free for owner)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static void local_heap_init(LocalHeap *lh) {
    lh->heap = calloc(LOCAL_HEAP_CAPACITY, sizeof(Task));
    lh->size = 0;
    lh->capacity = LOCAL_HEAP_CAPACITY;
    lh->local_pushes = 0;
    lh->local_pops = 0;
    lh->exported_to_global = 0;
    lh->imported_from_global = 0;
}

static void local_heap_destroy(LocalHeap *lh) {
    if (lh->heap) {
        free(lh->heap);
        lh->heap = NULL;
    }
}

// Push to LocalHeap (NO LOCK - owner only)
static bool local_heap_push(LocalHeap *lh, const Task *task) {
    if (lh->size >= lh->capacity) {
        return false;  // Heap full
    }

    // Sift up
    int i = lh->size;
    lh->size++;
    lh->local_pushes++;

    while (i > 0) {
        int parent = (i - 1) / 2;
        if (task->priority <= lh->heap[parent].priority) {
            break;
        }
        lh->heap[i] = lh->heap[parent];
        i = parent;
    }
    lh->heap[i] = *task;
    return true;
}

// Pop from LocalHeap (NO LOCK - owner only)
static bool local_heap_pop(LocalHeap *lh, Task *out_task) {
    if (lh->size == 0) {
        return false;
    }

    *out_task = lh->heap[0];
    lh->size--;
    lh->local_pops++;

    if (lh->size > 0) {
        Task last = lh->heap[lh->size];
        int i = 0;

        // Sift down
        while (i * 2 + 1 < lh->size) {
            int child = i * 2 + 1;
            if (child + 1 < lh->size && lh->heap[child + 1].priority > lh->heap[child].priority) {
                child++;
            }
            if (last.priority >= lh->heap[child].priority) {
                break;
            }
            lh->heap[i] = lh->heap[child];
            i = child;
        }
        lh->heap[i] = last;
    }
    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// GlobalChunkQueue Operations
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static GlobalChunkQueue* global_chunk_queue_create(void) {
    GlobalChunkQueue *gq = calloc(1, sizeof(GlobalChunkQueue));
    gq->heap = calloc(GLOBAL_QUEUE_CAPACITY, sizeof(Chunk));
    gq->capacity = GLOBAL_QUEUE_CAPACITY;
    gq->size = 0;
    pthread_mutex_init(&gq->mutex, NULL);
    pthread_cond_init(&gq->cond, NULL);  // 条件変数初期化
    atomic_store(&gq->top_priority, INT_MIN);
    gq->chunks_pushed = 0;
    gq->chunks_popped = 0;
    return gq;
}

static void global_chunk_queue_destroy(GlobalChunkQueue *gq) {
    if (gq) {
        pthread_mutex_destroy(&gq->mutex);
        pthread_cond_destroy(&gq->cond);  // 条件変数破棄
        free(gq->heap);
        free(gq);
    }
}

// Push chunk to GlobalChunkQueue
static bool global_chunk_queue_push(GlobalChunkQueue *gq, const Chunk *chunk) {
    pthread_mutex_lock(&gq->mutex);

    if (gq->size >= gq->capacity) {
        pthread_mutex_unlock(&gq->mutex);
        return false;
    }

    // Sift up
    int i = gq->size;
    gq->size++;
    gq->chunks_pushed++;

    while (i > 0) {
        int parent = (i - 1) / 2;
        if (chunk->top_priority <= gq->heap[parent].top_priority) {
            break;
        }
        gq->heap[i] = gq->heap[parent];
        i = parent;
    }
    gq->heap[i] = *chunk;

    // Update atomic top_priority
    atomic_store(&gq->top_priority, gq->heap[0].top_priority);

    // 待機中のワーカーを起床（usleepポーリング置き換え）
    pthread_cond_broadcast(&gq->cond);

    pthread_mutex_unlock(&gq->mutex);
    return true;
}

// Pop chunk from GlobalChunkQueue
static bool global_chunk_queue_pop(GlobalChunkQueue *gq, Chunk *out_chunk) {
    pthread_mutex_lock(&gq->mutex);

    if (gq->size == 0) {
        pthread_mutex_unlock(&gq->mutex);
        return false;
    }

    *out_chunk = gq->heap[0];
    gq->size--;
    gq->chunks_popped++;

    if (gq->size > 0) {
        Chunk last = gq->heap[gq->size];
        int i = 0;

        // Sift down
        while (i * 2 + 1 < gq->size) {
            int child = i * 2 + 1;
            if (child + 1 < gq->size && gq->heap[child + 1].top_priority > gq->heap[child].top_priority) {
                child++;
            }
            if (last.top_priority >= gq->heap[child].top_priority) {
                break;
            }
            gq->heap[i] = gq->heap[child];
            i = child;
        }
        gq->heap[i] = last;

        // Update atomic top_priority
        atomic_store(&gq->top_priority, gq->heap[0].top_priority);
    } else {
        // Queue is now empty
        atomic_store(&gq->top_priority, INT_MIN);
    }

    pthread_mutex_unlock(&gq->mutex);
    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// SharedTaskArray Operations (lock-free)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static SharedTaskArray* shared_array_create(void) {
    SharedTaskArray *sa = calloc(1, sizeof(SharedTaskArray));
    sa->tasks = calloc(SHARED_ARRAY_SIZE, sizeof(Task));
    sa->capacity = SHARED_ARRAY_SIZE;
    atomic_store(&sa->head, 0);
    atomic_store(&sa->tail, 0);
    return sa;
}

static void shared_array_destroy(SharedTaskArray *sa) {
    if (sa) {
        free(sa->tasks);
        free(sa);
    }
}

// Push to SharedTaskArray (lock-free with CAS)
// 修正: 複数スレッドが同時にpushしても競合しないようCASを使用
// 注意: tail更新前に書き込みを完了し、メモリバリアで順序を保証
static bool shared_array_push(SharedTaskArray *sa, const Task *task) {
    while (1) {
        uint32_t tail = atomic_load(&sa->tail);
        uint32_t head = atomic_load(&sa->head);

        // Check if full
        if (tail - head >= (uint32_t)sa->capacity) {
            return false;
        }

        // CAS to reserve this slot (tail -> tail+1)
        if (atomic_compare_exchange_weak(&sa->tail, &tail, tail + 1)) {
            // スロット確保成功 - 書き込み
            uint32_t idx = tail % sa->capacity;
            sa->tasks[idx] = *task;
            // メモリバリア: 書き込み完了を他スレッドに可視化
            __atomic_thread_fence(__ATOMIC_RELEASE);
            return true;
        }
        // Failed (another thread pushed), retry
    }
}

// Pop側でも書き込み完了を待つためのスピン待機が必要
// ただしSharedTaskArrayは起動/終盤フェーズでのみ使用され、
// 通常は低競合なので、書き込み完了前に読まれる確率は極めて低い

// Pop from SharedTaskArray (CAS-based)
static bool shared_array_pop(SharedTaskArray *sa, Task *out_task) {
    while (1) {
        uint32_t head = atomic_load(&sa->head);
        uint32_t tail = atomic_load(&sa->tail);

        // Check if empty
        if (head >= tail) {
            return false;
        }

        uint32_t idx = head % sa->capacity;
        *out_task = sa->tasks[idx];

        // CAS to claim this slot
        if (atomic_compare_exchange_weak(&sa->head, &head, head + 1)) {
            return true;
        }
        // Failed, retry
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Evaluation Function Structures
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

typedef struct {
    uint32_t n_square;
    uint8_t x[12];
} FeatureToCoordinate;

static int16_t ***EVAL_WEIGHT = NULL;

static const FeatureToCoordinate EVAL_F2X[] = {
    { 9, {0, 1, 8, 9, 2, 16, 10, 17, 18}},
    { 9, {7, 6, 15, 14, 5, 23, 13, 22, 21}},
    { 9, {56, 48, 57, 49, 40, 58, 41, 50, 42}},
    { 9, {63, 55, 62, 54, 47, 61, 46, 53, 45}},
    {10, {32, 24, 16, 8, 0, 9, 1, 2, 3, 4}},
    {10, {39, 31, 23, 15, 7, 14, 6, 5, 4, 3}},
    {10, {24, 32, 40, 48, 56, 49, 57, 58, 59, 60}},
    {10, {31, 39, 47, 55, 63, 54, 62, 61, 60, 59}},
    {10, {9, 0, 1, 2, 3, 4, 5, 6, 7, 14}},
    {10, {49, 56, 57, 58, 59, 60, 61, 62, 63, 54}},
    {10, {9, 0, 8, 16, 24, 32, 40, 48, 56, 49}},
    {10, {14, 7, 15, 23, 31, 39, 47, 55, 63, 54}},
    {10, {0, 2, 3, 10, 11, 18, 19, 4, 5, 7}},
    {10, {56, 58, 59, 50, 51, 42, 43, 60, 61, 63}},
    {10, {0, 16, 24, 17, 25, 33, 41, 32, 40, 56}},
    {10, {7, 23, 31, 22, 30, 38, 46, 39, 47, 63}},
    { 8, {8, 9, 10, 11, 12, 13, 14, 15}},
    { 8, {48, 49, 50, 51, 52, 53, 54, 55}},
    { 8, {1, 9, 17, 25, 33, 41, 49, 57}},
    { 8, {6, 14, 22, 30, 38, 46, 54, 62}},
    { 8, {16, 17, 18, 19, 20, 21, 22, 23}},
    { 8, {40, 41, 42, 43, 44, 45, 46, 47}},
    { 8, {2, 10, 18, 26, 34, 42, 50, 58}},
    { 8, {5, 13, 21, 29, 37, 45, 53, 61}},
    { 8, {24, 25, 26, 27, 28, 29, 30, 31}},
    { 8, {32, 33, 34, 35, 36, 37, 38, 39}},
    { 8, {3, 11, 19, 27, 35, 43, 51, 59}},
    { 8, {4, 12, 20, 28, 36, 44, 52, 60}},
    { 8, {0, 9, 18, 27, 36, 45, 54, 63}},
    { 8, {56, 49, 42, 35, 28, 21, 14, 7}},
    { 7, {1, 10, 19, 28, 37, 46, 55}},
    { 7, {15, 22, 29, 36, 43, 50, 57}},
    { 7, {8, 17, 26, 35, 44, 53, 62}},
    { 7, {6, 13, 20, 27, 34, 41, 48}},
    { 6, {2, 11, 20, 29, 38, 47}},
    { 6, {16, 25, 34, 43, 52, 61}},
    { 6, {5, 12, 19, 26, 33, 40}},
    { 6, {23, 30, 37, 44, 51, 58}},
    { 5, {3, 12, 21, 30, 39}},
    { 5, {24, 33, 42, 51, 60}},
    { 5, {4, 11, 18, 25, 32}},
    { 5, {31, 38, 45, 52, 59}},
    { 4, {3, 10, 17, 24}},
    { 4, {32, 41, 50, 59}},
    { 4, {4, 13, 22, 31}},
    { 4, {39, 46, 53, 60}},
    { 0, {64}},
    { 0, {64}}
};

static const uint32_t EVAL_SIZE[] = {19683, 59049, 59049, 59049, 6561, 6561, 6561, 6561, 2187, 729, 243, 81, 1};
static const uint32_t EVAL_PACKED_SIZE[] = {10206, 29889, 29646, 29646, 3321, 3321, 3321, 3321, 1134, 378, 135, 45, 1};
static const uint32_t EVAL_N_WEIGHT = 226315;
static const uint32_t EVAL_N_PLY = 61;
static const uint32_t EVAL_N_FEATURE = 47;

static const uint32_t FEATURE_OFFSET[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    6561, 6561, 6561, 6561, 13122, 13122, 13122, 13122, 19683, 19683,
    26244, 26244, 26244, 26244, 28431, 28431, 28431, 28431,
    29160, 29160, 29160, 29160, 29403, 29403, 29403, 29403, 29484, 29485
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Priority Queue
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

typedef struct {
    int move;
    int priority;
} PriorityMove;

typedef struct {
    PriorityMove *moves;
    int size;
    int capacity;
} PriorityQueue;

static PriorityQueue* pq_create(int capacity) {
    PriorityQueue *pq = malloc(sizeof(PriorityQueue));
    pq->moves = malloc(capacity * sizeof(PriorityMove));
    pq->size = 0;
    pq->capacity = capacity;
    return pq;
}

static void pq_free(PriorityQueue *pq) {
    free(pq->moves);
    free(pq);
}

static void pq_push(PriorityQueue *pq, int move, int priority) {
    if (pq->size >= pq->capacity) return;

    int i = pq->size++;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (priority <= pq->moves[parent].priority) break;
        pq->moves[i] = pq->moves[parent];
        i = parent;
    }
    pq->moves[i].move = move;
    pq->moves[i].priority = priority;
}

static int pq_pop(PriorityQueue *pq) {
    if (pq->size == 0) return -1;

    int result = pq->moves[0].move;
    pq->size--;

    if (pq->size > 0) {
        PriorityMove last = pq->moves[pq->size];
        int i = 0;

        while (i * 2 + 1 < pq->size) {
            int child = i * 2 + 1;
            if (child + 1 < pq->size && pq->moves[child + 1].priority > pq->moves[child].priority) {
                child++;
            }
            if (last.priority >= pq->moves[child].priority) break;
            pq->moves[i] = pq->moves[child];
            i = child;
        }
        pq->moves[i] = last;
    }

    return result;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Bitboard Operations
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

#define popcount(x) __builtin_popcountll(x)
#define first_one(x) __builtin_ctzll(x)

static inline uint64_t bswap_64(uint64_t b) {
    return __builtin_bswap64(b);
}

static inline uint64_t vertical_mirror(uint64_t b) {
    return bswap_64(b);
}

static inline uint64_t horizontal_mirror(uint64_t b) {
    b = ((b >> 1) & 0x5555555555555555ULL) | ((b << 1) & 0xAAAAAAAAAAAAAAAAULL);
    b = ((b >> 2) & 0x3333333333333333ULL) | ((b << 2) & 0xCCCCCCCCCCCCCCCCULL);
    b = ((b >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((b << 4) & 0xF0F0F0F0F0F0F0F0ULL);
    return b;
}

static inline uint64_t transpose(uint64_t b) {
    uint64_t t;
    t = (b ^ (b >> 7)) & 0x00aa00aa00aa00aaULL;
    b = b ^ t ^ (t << 7);
    t = (b ^ (b >> 14)) & 0x0000cccc0000ccccULL;
    b = b ^ t ^ (t << 14);
    t = (b ^ (b >> 28)) & 0x00000000f0f0f0f0ULL;
    b = b ^ t ^ (t << 28);
    return b;
}

static inline void board_symmetry(uint64_t player, uint64_t opponent, int s,
                                   uint64_t *sym_player, uint64_t *sym_opponent) {
    *sym_player = player;
    *sym_opponent = opponent;

    if (s & 1) {
        *sym_player = horizontal_mirror(*sym_player);
        *sym_opponent = horizontal_mirror(*sym_opponent);
    }
    if (s & 2) {
        *sym_player = vertical_mirror(*sym_player);
        *sym_opponent = vertical_mirror(*sym_opponent);
    }
    if (s & 4) {
        *sym_player = transpose(*sym_player);
        *sym_opponent = transpose(*sym_opponent);
    }
}

static inline bool board_lesser(uint64_t p1, uint64_t o1, uint64_t p2, uint64_t o2) {
    return (p1 < p2) || (p1 == p2 && o1 < o2);
}

// ────────────────────────────────────────────────────────────
// [スカラー版] board_unique - 元の実装（フォールバック用）
// ────────────────────────────────────────────────────────────
static int board_unique_scalar(uint64_t player, uint64_t opponent,
                               uint64_t *unique_player, uint64_t *unique_opponent) {
    uint64_t sym_p, sym_o;
    int best_sym = 0;

    *unique_player = player;
    *unique_opponent = opponent;

    for (int s = 1; s < 8; s++) {
        board_symmetry(player, opponent, s, &sym_p, &sym_o);
        if (board_lesser(sym_p, sym_o, *unique_player, *unique_opponent)) {
            *unique_player = sym_p;
            *unique_opponent = sym_o;
            best_sym = s;
        }
    }

    return best_sym;
}

// ────────────────────────────────────────────────────────────
// [AVX2版] board_unique - 4つの対称形を並列計算
// ────────────────────────────────────────────────────────────
//
// 【アルゴリズム】
//   8つの対称形を2回に分けて計算（各回4つの対称形を並列処理）
//   - 1回目: s=0,1,2,3 の4つの対称形を計算
//   - 2回目: s=4,5,6,7 の4つの対称形を計算
//   各対称形から最小を選択
//
// 【AVX2レジスタ配置】
//   __m256i = [sym0_p, sym1_p, sym2_p, sym3_p] (4 x 64-bit)
//   __m256i = [sym0_o, sym1_o, sym2_o, sym3_o] (4 x 64-bit)
//
// 【性能】
//   スカラー版: 7回のループ、各回で複数の変換
//   AVX2版: 2回の並列計算 + 8回の比較
//   理論上 ~2-3倍高速（キャッシュ効率による）
// ────────────────────────────────────────────────────────────

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

// AVX2版 horizontal_mirror: 4つの64-bit値を同時に水平反転
static inline __m256i horizontal_mirror_avx2(__m256i b) {
    const __m256i mask1 = _mm256_set1_epi64x(0x5555555555555555ULL);
    const __m256i mask2 = _mm256_set1_epi64x(0xAAAAAAAAAAAAAAAAULL);
    const __m256i mask3 = _mm256_set1_epi64x(0x3333333333333333ULL);
    const __m256i mask4 = _mm256_set1_epi64x(0xCCCCCCCCCCCCCCCCULL);
    const __m256i mask5 = _mm256_set1_epi64x(0x0F0F0F0F0F0F0F0FULL);
    const __m256i mask6 = _mm256_set1_epi64x(0xF0F0F0F0F0F0F0F0ULL);

    b = _mm256_or_si256(
        _mm256_and_si256(_mm256_srli_epi64(b, 1), mask1),
        _mm256_and_si256(_mm256_slli_epi64(b, 1), mask2)
    );
    b = _mm256_or_si256(
        _mm256_and_si256(_mm256_srli_epi64(b, 2), mask3),
        _mm256_and_si256(_mm256_slli_epi64(b, 2), mask4)
    );
    b = _mm256_or_si256(
        _mm256_and_si256(_mm256_srli_epi64(b, 4), mask5),
        _mm256_and_si256(_mm256_slli_epi64(b, 4), mask6)
    );
    return b;
}

// AVX2版 vertical_mirror: 4つの64-bit値を同時に垂直反転（バイトスワップ）
static inline __m256i vertical_mirror_avx2(__m256i b) {
    // バイト順序を反転するシャッフルマスク
    const __m256i shuffle_mask = _mm256_set_epi8(
        8, 9, 10, 11, 12, 13, 14, 15,   // 上位64ビット用
        0, 1, 2, 3, 4, 5, 6, 7,         // 下位64ビット用
        8, 9, 10, 11, 12, 13, 14, 15,   // 上位64ビット用
        0, 1, 2, 3, 4, 5, 6, 7          // 下位64ビット用
    );
    return _mm256_shuffle_epi8(b, shuffle_mask);
}

// AVX2版 transpose: 4つの64-bit値を同時に転置
static inline __m256i transpose_avx2(__m256i b) {
    const __m256i mask1 = _mm256_set1_epi64x(0x00aa00aa00aa00aaULL);
    const __m256i mask2 = _mm256_set1_epi64x(0x0000cccc0000ccccULL);
    const __m256i mask3 = _mm256_set1_epi64x(0x00000000f0f0f0f0ULL);

    __m256i t;
    t = _mm256_and_si256(_mm256_xor_si256(b, _mm256_srli_epi64(b, 7)), mask1);
    b = _mm256_xor_si256(b, _mm256_xor_si256(t, _mm256_slli_epi64(t, 7)));
    t = _mm256_and_si256(_mm256_xor_si256(b, _mm256_srli_epi64(b, 14)), mask2);
    b = _mm256_xor_si256(b, _mm256_xor_si256(t, _mm256_slli_epi64(t, 14)));
    t = _mm256_and_si256(_mm256_xor_si256(b, _mm256_srli_epi64(b, 28)), mask3);
    b = _mm256_xor_si256(b, _mm256_xor_si256(t, _mm256_slli_epi64(t, 28)));
    return b;
}

// AVX2版 board_unique: 8つの対称形を並列計算して最小を選択
static int board_unique_avx2(uint64_t player, uint64_t opponent,
                             uint64_t *unique_player, uint64_t *unique_opponent) {
    // 8つの対称形を格納する配列
    uint64_t sym_p[8], sym_o[8];

    // 元の盤面 (s=0: 恒等変換)
    sym_p[0] = player;
    sym_o[0] = opponent;

    // === 第1グループ: s=0,1,2,3 を並列計算 ===
    // s=0: 恒等 (何もしない)
    // s=1: H (水平反転のみ)
    // s=2: V (垂直反転のみ)
    // s=3: H+V (水平+垂直反転)

    // player用ベクトル: [p, p, p, p]
    __m256i p_vec = _mm256_set1_epi64x(player);
    __m256i o_vec = _mm256_set1_epi64x(opponent);

    // 水平反転を適用するマスク: s=1,3 → インデックス1,3
    __m256i p_h = horizontal_mirror_avx2(p_vec);  // [H(p), H(p), H(p), H(p)]
    __m256i o_h = horizontal_mirror_avx2(o_vec);

    // 垂直反転を適用: s=2,3
    __m256i p_v = vertical_mirror_avx2(p_vec);    // [V(p), V(p), V(p), V(p)]
    __m256i o_v = vertical_mirror_avx2(o_vec);

    // 水平+垂直反転
    __m256i p_hv = vertical_mirror_avx2(p_h);     // [VH(p), ...]
    __m256i o_hv = vertical_mirror_avx2(o_h);

    // 結果を抽出
    sym_p[1] = _mm256_extract_epi64(p_h, 0);
    sym_o[1] = _mm256_extract_epi64(o_h, 0);
    sym_p[2] = _mm256_extract_epi64(p_v, 0);
    sym_o[2] = _mm256_extract_epi64(o_v, 0);
    sym_p[3] = _mm256_extract_epi64(p_hv, 0);
    sym_o[3] = _mm256_extract_epi64(o_hv, 0);

    // === 第2グループ: s=4,5,6,7 (転置 + 上記の組み合わせ) ===
    // s=4: T (転置のみ)
    // s=5: T+H
    // s=6: T+V
    // s=7: T+H+V

    __m256i p_t = transpose_avx2(p_vec);          // [T(p), ...]
    __m256i o_t = transpose_avx2(o_vec);

    __m256i p_th = horizontal_mirror_avx2(p_t);   // [HT(p), ...]
    __m256i o_th = horizontal_mirror_avx2(o_t);

    __m256i p_tv = vertical_mirror_avx2(p_t);     // [VT(p), ...]
    __m256i o_tv = vertical_mirror_avx2(o_t);

    __m256i p_thv = vertical_mirror_avx2(p_th);   // [VHT(p), ...]
    __m256i o_thv = vertical_mirror_avx2(o_th);

    // 結果を抽出
    sym_p[4] = _mm256_extract_epi64(p_t, 0);
    sym_o[4] = _mm256_extract_epi64(o_t, 0);
    sym_p[5] = _mm256_extract_epi64(p_th, 0);
    sym_o[5] = _mm256_extract_epi64(o_th, 0);
    sym_p[6] = _mm256_extract_epi64(p_tv, 0);
    sym_o[6] = _mm256_extract_epi64(o_tv, 0);
    sym_p[7] = _mm256_extract_epi64(p_thv, 0);
    sym_o[7] = _mm256_extract_epi64(o_thv, 0);

    // === 最小の対称形を選択 ===
    int best_sym = 0;
    *unique_player = sym_p[0];
    *unique_opponent = sym_o[0];

    for (int s = 1; s < 8; s++) {
        if (board_lesser(sym_p[s], sym_o[s], *unique_player, *unique_opponent)) {
            *unique_player = sym_p[s];
            *unique_opponent = sym_o[s];
            best_sym = s;
        }
    }

    return best_sym;
}

#endif // defined(__x86_64__) || defined(_M_X64)

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Move Generation (with optional SIMD acceleration)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Check for AVX2 support at runtime
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <cpuid.h>
static bool cpu_has_avx2 = false;
static bool cpu_checked = false;

static void check_cpu_features(void) {
    if (cpu_checked) return;
    cpu_checked = true;

    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        // Check for AVX (bit 28 of ECX)
        bool has_avx = (ecx & (1 << 28)) != 0;
        // Check for OSXSAVE (bit 27 of ECX) - OS supports AVX
        bool has_osxsave = (ecx & (1 << 27)) != 0;

        if (has_avx && has_osxsave) {
            // Check for AVX2 support (need to check extended features)
            if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
                cpu_has_avx2 = (ebx & (1 << 5)) != 0;  // AVX2 is bit 5 of EBX
            }
        }
    }
}
#else
#define cpu_has_avx2 false
static void check_cpu_features(void) {}
#endif

// ────────────────────────────────────────────────────────────
// board_unique: ランタイムでAVX2/スカラー版を選択（実装）
// ────────────────────────────────────────────────────────────
static int board_unique(uint64_t player, uint64_t opponent,
                        uint64_t *unique_player, uint64_t *unique_opponent) {
#if defined(__x86_64__) || defined(_M_X64)
    if (cpu_has_avx2) {
        return board_unique_avx2(player, opponent, unique_player, unique_opponent);
    }
#endif
    return board_unique_scalar(player, opponent, unique_player, unique_opponent);
}

// Scalar version of get_moves (always available)
static inline uint64_t get_moves_scalar(uint64_t P, uint64_t O) {
    uint64_t mask, moves, flip_l, flip_r, pre_l, pre_r;
    uint64_t flip_h, flip_v, flip_d1, flip_d2;

    mask = O & 0x7e7e7e7e7e7e7e7eULL;

    flip_l = mask & (P << 1);
    flip_r = mask & (P >> 1);
    flip_l |= mask & (flip_l << 1);
    flip_r |= mask & (flip_r >> 1);
    pre_l = mask & (flip_l << 1);
    pre_r = mask & (flip_r >> 1);
    flip_l |= pre_l | (pre_l << 2);
    flip_r |= pre_r | (pre_r >> 2);
    flip_l |= mask & (flip_l << 1);
    flip_r |= mask & (flip_r >> 1);

    mask = O & 0x00ffffffffffff00ULL;
    flip_h = mask & (P << 8);
    flip_v = mask & (P >> 8);
    flip_h |= mask & (flip_h << 8);
    flip_v |= mask & (flip_v >> 8);
    flip_h |= mask & ((flip_h & (mask << 8)) << 16);
    flip_v |= mask & ((flip_v & (mask >> 8)) >> 16);
    flip_h |= mask & (flip_h << 8);
    flip_v |= mask & (flip_v >> 8);

    mask = O & 0x007e7e7e7e7e7e00ULL;
    flip_d1 = mask & (P << 9);
    flip_d2 = mask & (P << 7);
    flip_d1 |= mask & (flip_d1 << 9);
    flip_d2 |= mask & (flip_d2 << 7);
    flip_d1 |= mask & ((flip_d1 & (mask << 9)) << 18);
    flip_d2 |= mask & ((flip_d2 & (mask << 7)) << 14);
    flip_d1 |= mask & (flip_d1 << 9);
    flip_d2 |= mask & (flip_d2 << 7);

    flip_d1 |= mask & (P >> 9);
    flip_d2 |= mask & (P >> 7);
    flip_d1 |= mask & (flip_d1 >> 9);
    flip_d2 |= mask & (flip_d2 >> 7);
    flip_d1 |= mask & ((flip_d1 & (mask >> 9)) >> 18);
    flip_d2 |= mask & ((flip_d2 & (mask >> 7)) >> 14);
    flip_d1 |= mask & (flip_d1 >> 9);
    flip_d2 |= mask & (flip_d2 >> 7);

    moves = (flip_l << 1) | (flip_r >> 1) |
            (flip_h << 8) | (flip_v >> 8) |
            (flip_d1 << 9) | (flip_d1 >> 9) |
            (flip_d2 << 7) | (flip_d2 >> 7);

    return moves & ~(P | O);
}

// AVX2-accelerated version of get_moves
#if defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX2__
#include <immintrin.h>

// AVX2 version: processes 4 directions in parallel using 256-bit registers
static inline uint64_t get_moves_avx2(uint64_t P, uint64_t O) {
    // Masks for different directions
    const uint64_t mask_h = 0x7e7e7e7e7e7e7e7eULL;  // Horizontal mask
    const uint64_t mask_v = 0x00ffffffffffff00ULL;  // Vertical mask
    const uint64_t mask_d = 0x007e7e7e7e7e7e00ULL;  // Diagonal mask

    // Load masks into 256-bit registers (4 x 64-bit)
    __m256i vmask = _mm256_set_epi64x(
        O & mask_d,   // Diagonal 2
        O & mask_d,   // Diagonal 1
        O & mask_v,   // Vertical
        O & mask_h    // Horizontal
    );

    // Shift amounts for each direction (use different shifts)
    // Horizontal: 1, Vertical: 8, Diagonal1: 9, Diagonal2: 7
    __m256i vP = _mm256_set1_epi64x(P);

    // Process left/up shifts
    __m256i vshift_l = _mm256_set_epi64x(
        P << 7,   // Diagonal 2 left
        P << 9,   // Diagonal 1 left
        P << 8,   // Vertical up
        P << 1    // Horizontal left
    );

    // First iteration of flood fill
    __m256i vflip_l = _mm256_and_si256(vmask, vshift_l);

    // Continue flood fill (2nd iteration)
    __m256i vflip_l2 = _mm256_set_epi64x(
        _mm256_extract_epi64(vflip_l, 3) << 7,
        _mm256_extract_epi64(vflip_l, 2) << 9,
        _mm256_extract_epi64(vflip_l, 1) << 8,
        _mm256_extract_epi64(vflip_l, 0) << 1
    );
    vflip_l = _mm256_or_si256(vflip_l, _mm256_and_si256(vmask, vflip_l2));

    // Continue for 4 more iterations (simplified - full expansion)
    for (int i = 0; i < 4; i++) {
        vflip_l2 = _mm256_set_epi64x(
            _mm256_extract_epi64(vflip_l, 3) << 7,
            _mm256_extract_epi64(vflip_l, 2) << 9,
            _mm256_extract_epi64(vflip_l, 1) << 8,
            _mm256_extract_epi64(vflip_l, 0) << 1
        );
        vflip_l = _mm256_or_si256(vflip_l, _mm256_and_si256(vmask, vflip_l2));
    }

    // Process right/down shifts
    __m256i vshift_r = _mm256_set_epi64x(
        P >> 7,   // Diagonal 2 right
        P >> 9,   // Diagonal 1 right
        P >> 8,   // Vertical down
        P >> 1    // Horizontal right
    );

    __m256i vflip_r = _mm256_and_si256(vmask, vshift_r);

    for (int i = 0; i < 5; i++) {
        __m256i vflip_r2 = _mm256_set_epi64x(
            _mm256_extract_epi64(vflip_r, 3) >> 7,
            _mm256_extract_epi64(vflip_r, 2) >> 9,
            _mm256_extract_epi64(vflip_r, 1) >> 8,
            _mm256_extract_epi64(vflip_r, 0) >> 1
        );
        vflip_r = _mm256_or_si256(vflip_r, _mm256_and_si256(vmask, vflip_r2));
    }

    // Extract results and compute final moves
    uint64_t flip_h_l = _mm256_extract_epi64(vflip_l, 0);
    uint64_t flip_v_l = _mm256_extract_epi64(vflip_l, 1);
    uint64_t flip_d1_l = _mm256_extract_epi64(vflip_l, 2);
    uint64_t flip_d2_l = _mm256_extract_epi64(vflip_l, 3);

    uint64_t flip_h_r = _mm256_extract_epi64(vflip_r, 0);
    uint64_t flip_v_r = _mm256_extract_epi64(vflip_r, 1);
    uint64_t flip_d1_r = _mm256_extract_epi64(vflip_r, 2);
    uint64_t flip_d2_r = _mm256_extract_epi64(vflip_r, 3);

    // Combine all directions
    uint64_t moves = (flip_h_l << 1) | (flip_h_r >> 1) |
                     (flip_v_l << 8) | (flip_v_r >> 8) |
                     (flip_d1_l << 9) | (flip_d1_r >> 9) |
                     (flip_d2_l << 7) | (flip_d2_r >> 7);

    return moves & ~(P | O);
}
#endif  // __AVX2__
#endif  // x86_64

// Dispatcher function - selects best implementation at runtime
static inline uint64_t get_moves(uint64_t P, uint64_t O) {
    // ────────────────────────────────────────────────────────────
    // [最適化] AVX2版get_movesを無効化し、スカラー版を使用
    // ────────────────────────────────────────────────────────────
    // 【理由】
    //   現在のget_moves_avx2実装には以下の問題がある：
    //   1. ループ内で_mm256_extract_epi64を多用（スカラー抽出）
    //   2. 抽出後に再度_mm256_set_epi64xでベクトル化
    //   3. これではSIMDの並列性が活かせず、スカラー版と同等以下の性能
    //
    // 【元のコード】
    // #if defined(__x86_64__) || defined(_M_X64)
    // #ifdef __AVX2__
    //     if (cpu_has_avx2) {
    //         return get_moves_avx2(P, O);
    //     }
    // #endif
    // #endif
    //
    // 【補足】
    //   board_unique_avx2は効率的なAVX2実装のため有効のまま
    //   get_movesのAVX2最適化は、可変シフト（_mm256_sllv_epi64）を
    //   使った完全並列化が必要だが、フリップ計算の複雑さから
    //   実装コストが高い
    // ────────────────────────────────────────────────────────────
    return get_moves_scalar(P, O);
}

static uint64_t flip_discs(uint64_t P, uint64_t O, int pos) {
    uint64_t flip = 0;
    uint64_t move_bit = 1ULL << pos;
    uint64_t PO = P | O;

    int x = pos & 7;
    int y = pos >> 3;

    if (x < 6) {
        uint64_t mask = 0x7eULL << (y * 8);
        uint64_t outflank = ((0x80ULL << (y * 8)) - move_bit) & PO & mask;
        if (outflank) {
            uint64_t boundary = outflank & -outflank;
            if (P & boundary) {
                flip |= (boundary - move_bit) & mask;
            }
        }
    }

    if (x > 1) {
        uint64_t mask = 0x7eULL << (y * 8);
        uint64_t outflank = (move_bit - 1) & PO & mask;
        if (outflank) {
            uint64_t boundary = 1ULL << (63 - __builtin_clzll(outflank));
            if (P & boundary) {
                flip |= (move_bit - boundary - 1) & mask;
            }
        }
    }

    if (y < 6) {
        uint64_t mask = 0x00ffffffffffff00ULL & (0x0101010101010101ULL << x);
        uint64_t outflank = ((0x8000000000000000ULL >> (7 - x)) - move_bit) & PO & mask;
        if (outflank) {
            uint64_t boundary = outflank & -outflank;
            if (P & boundary) {
                flip |= (boundary - move_bit) & mask;
            }
        }
    }

    if (y > 1) {
        uint64_t mask = 0x00ffffffffffff00ULL & (0x0101010101010101ULL << x);
        uint64_t outflank = (move_bit - 1) & PO & mask;
        if (outflank) {
            uint64_t boundary = 1ULL << (63 - __builtin_clzll(outflank));
            if (P & boundary) {
                flip |= (move_bit - boundary - 1) & mask;
            }
        }
    }

    const int dirs[] = {7, 9, -7, -9};
    for (int d = 0; d < 4; d++) {
        int dir = dirs[d];
        int p = pos + dir;
        uint64_t line = 0;

        while (p >= 0 && p < 64) {
            int px = p & 7, py = p >> 3;
            int dx = px - x, dy = py - y;
            if (abs(dx) != abs(dy)) break;

            if (O & (1ULL << p)) {
                line |= 1ULL << p;
                p += dir;
            } else if (P & (1ULL << p)) {
                flip |= line;
                break;
            } else {
                break;
            }
        }
    }

    return flip;
}

static inline void make_move(uint64_t *P, uint64_t *O, int pos) {
    uint64_t flip = flip_discs(*P, *O, pos);
    uint64_t move = 1ULL << pos;

    *P = (*P | move | flip);
    *O = (*O ^ flip);

    uint64_t tmp = *P;
    *P = *O;
    *O = tmp;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Evaluation Function
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static inline int board_get_square_color(uint64_t player, uint64_t opponent, int sq) {
    if (player & (1ULL << sq)) return 0;
    if (opponent & (1ULL << sq)) return 1;
    return 2;
}

static uint16_t compute_feature(uint64_t player, uint64_t opponent, int feature_idx) {
    const FeatureToCoordinate *f2x = &EVAL_F2X[feature_idx];
    uint16_t feature = 0;

    for (uint32_t j = 0; j < f2x->n_square; j++) {
        int sq = f2x->x[j];
        if (sq >= 64) continue;
        int color = board_get_square_color(player, opponent, sq);
        feature = feature * 3 + color;
    }

    return feature + FEATURE_OFFSET[feature_idx];
}

// スカラー版評価関数
static int evaluate_position_scalar(uint64_t player, uint64_t opponent) {
    if (!EVAL_WEIGHT) return 0;

    int empties = popcount(~(player | opponent));
    int ply = 60 - empties;
    if (ply >= (int)EVAL_N_PLY) ply = EVAL_N_PLY - 1;

    int16_t *weights = EVAL_WEIGHT[ply][0];
    int sum = 0;

    for (int i = 0; i < (int)EVAL_N_FEATURE; i++) {
        uint16_t feat = compute_feature(player, opponent, i);
        if (feat < EVAL_N_WEIGHT) {
            sum += weights[feat];
        }
    }

    return sum / 128;
}

#ifdef __AVX2__
// AVX2版評価関数（手動gather + 水平加算）
static int evaluate_position_avx2(uint64_t player, uint64_t opponent) {
    if (!EVAL_WEIGHT) return 0;

    int empties = popcount(~(player | opponent));
    int ply = 60 - empties;
    if (ply >= (int)EVAL_N_PLY) ply = EVAL_N_PLY - 1;

    int16_t *weights = EVAL_WEIGHT[ply][0];

    // 特徴量を計算
    uint16_t features[EVAL_N_FEATURE];
    for (int i = 0; i < (int)EVAL_N_FEATURE; i++) {
        features[i] = compute_feature(player, opponent, i);
    }

    // AVX2: 16個のint16_tを一度に加算
    __m256i sum_vec = _mm256_setzero_si256();
    int i = 0;

    // 16要素ずつ処理（EVAL_N_FEATURE=47なので、2回ループ = 32要素）
    for (; i + 16 <= (int)EVAL_N_FEATURE; i += 16) {
        // 手動gather: 16個の重みをロード
        __m256i w = _mm256_set_epi16(
            (features[i+15] < EVAL_N_WEIGHT) ? weights[features[i+15]] : 0,
            (features[i+14] < EVAL_N_WEIGHT) ? weights[features[i+14]] : 0,
            (features[i+13] < EVAL_N_WEIGHT) ? weights[features[i+13]] : 0,
            (features[i+12] < EVAL_N_WEIGHT) ? weights[features[i+12]] : 0,
            (features[i+11] < EVAL_N_WEIGHT) ? weights[features[i+11]] : 0,
            (features[i+10] < EVAL_N_WEIGHT) ? weights[features[i+10]] : 0,
            (features[i+9] < EVAL_N_WEIGHT) ? weights[features[i+9]] : 0,
            (features[i+8] < EVAL_N_WEIGHT) ? weights[features[i+8]] : 0,
            (features[i+7] < EVAL_N_WEIGHT) ? weights[features[i+7]] : 0,
            (features[i+6] < EVAL_N_WEIGHT) ? weights[features[i+6]] : 0,
            (features[i+5] < EVAL_N_WEIGHT) ? weights[features[i+5]] : 0,
            (features[i+4] < EVAL_N_WEIGHT) ? weights[features[i+4]] : 0,
            (features[i+3] < EVAL_N_WEIGHT) ? weights[features[i+3]] : 0,
            (features[i+2] < EVAL_N_WEIGHT) ? weights[features[i+2]] : 0,
            (features[i+1] < EVAL_N_WEIGHT) ? weights[features[i+1]] : 0,
            (features[i+0] < EVAL_N_WEIGHT) ? weights[features[i+0]] : 0
        );
        sum_vec = _mm256_add_epi16(sum_vec, w);
    }

    // 水平加算: __m256i (16 x int16_t) → int32_t
    // Step 1: 隣接する16-bitペアを32-bitに加算
    __m256i sum32 = _mm256_madd_epi16(sum_vec, _mm256_set1_epi16(1));
    // Step 2: 256-bit → 128-bit
    __m128i sum128 = _mm_add_epi32(_mm256_castsi256_si128(sum32),
                                   _mm256_extracti128_si256(sum32, 1));
    // Step 3: 水平加算
    sum128 = _mm_hadd_epi32(sum128, sum128);
    sum128 = _mm_hadd_epi32(sum128, sum128);
    int sum = _mm_cvtsi128_si32(sum128);

    // 残りをスカラーで処理
    for (; i < (int)EVAL_N_FEATURE; i++) {
        if (features[i] < EVAL_N_WEIGHT) {
            sum += weights[features[i]];
        }
    }

    return sum / 128;
}
#endif

// 評価関数（ランタイム選択）
static int evaluate_position(uint64_t player, uint64_t opponent) {
#ifdef __AVX2__
    if (cpu_has_avx2) {
        return evaluate_position_avx2(player, opponent);
    }
#endif
    return evaluate_position_scalar(player, opponent);
}

static bool load_evaluation_weights(const char *filename) {
    const uint32_t n_w = 114364;
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Warning: Cannot open evaluation file %s\n", filename);
        return false;
    }

    uint32_t edax_header, eval_header, version, release, build;
    double date;

    if (fread(&edax_header, sizeof(uint32_t), 1, f) != 1 ||
        fread(&eval_header, sizeof(uint32_t), 1, f) != 1 ||
        fread(&version, sizeof(uint32_t), 1, f) != 1 ||
        fread(&release, sizeof(uint32_t), 1, f) != 1 ||
        fread(&build, sizeof(uint32_t), 1, f) != 1 ||
        fread(&date, sizeof(double), 1, f) != 1) {
        fprintf(stderr, "Warning: Cannot read eval.dat header\n");
        fclose(f);
        return false;
    }

    EVAL_WEIGHT = calloc(EVAL_N_PLY, sizeof(int16_t**));
    for (uint32_t ply = 0; ply < EVAL_N_PLY; ply++) {
        EVAL_WEIGHT[ply] = calloc(1, sizeof(int16_t*));
        EVAL_WEIGHT[ply][0] = calloc(EVAL_N_WEIGHT, sizeof(int16_t));
    }

    int16_t *w = malloc(n_w * sizeof(int16_t));

    for (uint32_t ply = 0; ply < EVAL_N_PLY; ply++) {
        if (fread(w, sizeof(int16_t), n_w, f) != n_w) {
            fprintf(stderr, "Warning: Incomplete eval.dat file at ply %u\n", ply);
            break;
        }

        int j = 0;
        for (int i = 0; i < 13 && j < (int)EVAL_N_WEIGHT; i++) {
            for (uint32_t k = 0; k < EVAL_SIZE[i] && j < (int)EVAL_N_WEIGHT; k++, j++) {
                if (k < EVAL_PACKED_SIZE[i]) {
                    EVAL_WEIGHT[ply][0][j] = w[k];
                }
            }
        }
    }

    free(w);
    fclose(f);

    debug_log("Loaded evaluation weights from %s (version %u.%u.%u)\n",
           filename, version, release, build);

    return true;
}

static void free_evaluation_weights() {
    if (EVAL_WEIGHT) {
        for (uint32_t ply = 0; ply < EVAL_N_PLY; ply++) {
            if (EVAL_WEIGHT[ply]) {
                free(EVAL_WEIGHT[ply][0]);
                free(EVAL_WEIGHT[ply]);
            }
        }
        free(EVAL_WEIGHT);
        EVAL_WEIGHT = NULL;
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Transposition Table
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

typedef struct {
    uint64_t key;
    uint32_t pn;
    uint32_t dn;
    Result result;
    int8_t depth;
    int16_t eval_score;
    uint8_t age;
} TTEntry;

// Stripe lock configuration for TT
// Fixed number of stripes with cache line padding to prevent false sharing
#define TT_LOCK_STRIPES 1024
#define CACHE_LINE_SIZE 64

// Cache-line aligned lock structure to prevent false sharing
typedef struct {
    pthread_rwlock_t lock;
    char padding[CACHE_LINE_SIZE - sizeof(pthread_rwlock_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) AlignedLock;

typedef struct {
    TTEntry *entries;
    size_t size;
    size_t mask;
    AlignedLock locks[TT_LOCK_STRIPES];  // Fixed stripe locks with padding

    // Statistics (use atomic operations, no lock needed)
    volatile uint64_t hits;
    volatile uint64_t stores;
    volatile uint64_t collisions;
} TranspositionTable;

static uint64_t zobrist_table[2][64];
static bool zobrist_initialized = false;

static void init_zobrist() {
    if (zobrist_initialized) return;
    srand(12345);
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 64; j++) {
            zobrist_table[i][j] = ((uint64_t)rand() << 32) | rand();
        }
    }
    zobrist_initialized = true;
}

static inline uint64_t hash_position(uint64_t P, uint64_t O) {
    // ────────────────────────────────────────────────────────────
    // [最適化] init_zobrist()呼び出しを削除
    // ────────────────────────────────────────────────────────────
    // 【元のコード】
    //   init_zobrist();
    //
    // 【削除理由】
    //   hash_position()はTTアクセスのたびに呼ばれるホットパス
    //   （tt_probe, tt_storeから毎ノード2回呼ばれる）
    //   init_zobrist()は内部でzobrist_initializedをチェックするが、
    //   毎回の関数呼び出しオーバーヘッドが無駄。
    //   例: 100万ノード探索 → 約200万回の無駄な関数呼び出し
    //
    // 【変更内容】
    //   solve_endgame()の初期化時に一度だけinit_zobrist()を呼ぶ
    //   （ワーカースレッド起動前に呼ぶため安全）
    //
    // 【元に戻す場合】
    //   上記のinit_zobrist();のコメントを外し、
    //   solve_endgame()内の対応するinit_zobrist()呼び出しを削除する
    // ────────────────────────────────────────────────────────────

    uint64_t unique_P, unique_O;
    board_unique(P, O, &unique_P, &unique_O);

    uint64_t hash = 0;
    for (int i = 0; i < 64; i++) {
        if (unique_P & (1ULL << i)) hash ^= zobrist_table[0][i];
        if (unique_O & (1ULL << i)) hash ^= zobrist_table[1][i];
    }
    return hash;
}

static TranspositionTable* tt_create(size_t size_mb) {
    TranspositionTable *tt = calloc(1, sizeof(TranspositionTable));

    size_t entry_size = sizeof(TTEntry);
    size_t n_entries = (size_mb << 20) / entry_size;

    size_t size = 1;
    while (size < n_entries) size <<= 1;
    size >>= 1;

    tt->size = size;
    tt->mask = size - 1;
    tt->entries = calloc(size, sizeof(TTEntry));

    // Initialize fixed stripe locks (much fewer than entries)
    for (int i = 0; i < TT_LOCK_STRIPES; i++) {
        pthread_rwlock_init(&tt->locks[i].lock, NULL);
    }

    debug_log("TT created: %zu MB (%zu entries)\n", size_mb, size);

    return tt;
}

static void tt_free(TranspositionTable *tt) {
    for (int i = 0; i < TT_LOCK_STRIPES; i++) {
        pthread_rwlock_destroy(&tt->locks[i].lock);
    }
    free(tt->entries);
    free(tt);
}

static bool tt_probe(TranspositionTable *tt, uint64_t key, int depth,
                    uint32_t *pn, uint32_t *dn, Result *result, int16_t *eval_score) {
    size_t index = key & tt->mask;
    // Use higher bits of key for stripe selection (better distribution)
    int lock_index = (key >> 20) & (TT_LOCK_STRIPES - 1);

    pthread_rwlock_rdlock(&tt->locks[lock_index].lock);
    TTEntry *entry = &tt->entries[index];

    bool hit = (entry->key == key && entry->depth >= depth);
    if (hit) {
        *pn = entry->pn;
        *dn = entry->dn;
        *result = entry->result;
        if (eval_score) *eval_score = entry->eval_score;
        __sync_fetch_and_add(&tt->hits, 1);
    } else if (entry->key != 0 && entry->key != key) {
        __sync_fetch_and_add(&tt->collisions, 1);
    }

    pthread_rwlock_unlock(&tt->locks[lock_index].lock);
    return hit;
}

static void tt_store(TranspositionTable *tt, uint64_t key, int depth,
                    uint32_t pn, uint32_t dn, Result result, int16_t eval_score) {
    size_t index = key & tt->mask;
    // Use higher bits of key for stripe selection (same as tt_probe)
    int lock_index = (key >> 20) & (TT_LOCK_STRIPES - 1);

    pthread_rwlock_wrlock(&tt->locks[lock_index].lock);
    TTEntry *entry = &tt->entries[index];

    if (entry->depth <= depth) {
        entry->key = key;
        entry->pn = pn;
        entry->dn = dn;
        entry->result = result;
        entry->depth = depth;
        entry->eval_score = eval_score;
        entry->age = 0;
        __sync_fetch_and_add(&tt->stores, 1);
    }

    pthread_rwlock_unlock(&tt->locks[lock_index].lock);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// df-pn+ Node
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

typedef struct DFPNNode {
    uint64_t player;
    uint64_t opponent;

    uint32_t pn;
    uint32_t dn;
    uint32_t threshold_pn;
    uint32_t threshold_dn;

    Result result;
    NodeType type;
    int16_t eval_score;
    bool is_proven;  // 完全に証明されたかどうか（終端ノードから正しく伝播）

    struct DFPNNode **children;
    int n_children;
    int depth;

    uint64_t visits;
} DFPNNode;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// MoveWithEval: 手と評価値を保存（expand_node最適化用）
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

typedef struct {
    int move;
    int eval_score;
    uint64_t player;
    uint64_t opponent;
} MoveWithEval;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Memory Pool for DFPNNode (Arena Allocator)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

#define NODE_POOL_BLOCK_SIZE 8192

typedef struct NodePoolBlock {
    DFPNNode *nodes;
    struct NodePoolBlock *next;
} NodePoolBlock;

typedef struct {
    NodePoolBlock *first_block;
    NodePoolBlock *current_block;
    int current_index;
    int block_size;
    uint64_t total_allocated;
} NodePool;

static void node_pool_init(NodePool *pool) {
    pool->block_size = NODE_POOL_BLOCK_SIZE;
    pool->first_block = malloc(sizeof(NodePoolBlock));
    pool->first_block->nodes = calloc(pool->block_size, sizeof(DFPNNode));
    pool->first_block->next = NULL;
    pool->current_block = pool->first_block;
    pool->current_index = 0;
    pool->total_allocated = 0;
}

static DFPNNode* node_pool_alloc(NodePool *pool) {
    if (pool->current_index >= pool->block_size) {
        // Need new block
        if (pool->current_block->next == NULL) {
            // Allocate new block
            NodePoolBlock *new_block = malloc(sizeof(NodePoolBlock));
            new_block->nodes = calloc(pool->block_size, sizeof(DFPNNode));
            new_block->next = NULL;
            pool->current_block->next = new_block;
        }
        pool->current_block = pool->current_block->next;
        pool->current_index = 0;
        // Reset nodes in reused block
        memset(pool->current_block->nodes, 0, pool->block_size * sizeof(DFPNNode));
    }
    DFPNNode *node = &pool->current_block->nodes[pool->current_index];
    pool->current_index++;
    pool->total_allocated++;
    // Note: is_proven is initialized to false via calloc/memset
    return node;
}

static void node_pool_reset(NodePool *pool) {
    // Reset to beginning - blocks are retained for reuse
    pool->current_block = pool->first_block;
    pool->current_index = 0;
    // Reset first block (others reset on demand in node_pool_alloc)
    memset(pool->first_block->nodes, 0, pool->block_size * sizeof(DFPNNode));
}

static void node_pool_destroy(NodePool *pool) {
    NodePoolBlock *block = pool->first_block;
    while (block) {
        NodePoolBlock *next = block->next;
        free(block->nodes);
        free(block);
        block = next;
    }
    pool->first_block = NULL;
    pool->current_block = NULL;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Worker with Work Stealing
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Global state for hybrid work distribution
typedef struct Worker Worker;  // Forward declaration

typedef struct {
    TranspositionTable *tt;

    // Hybrid: GlobalChunkQueue + SharedTaskArray
    GlobalChunkQueue *global_chunk_queue;
    SharedTaskArray *shared_array;
    WorkerState worker_state;

    // Worker references (for statistics)
    Worker **workers;
    int n_workers;

    // Results per root move
    Result *move_results;
    uint64_t *move_nodes;
    int *move_list;
    int *move_evals;
    int n_moves;

#if ENABLE_EVAL_IMPACT
    // Evaluation impact tracking (-e option)
    // 評価関数影響分析用のデータ（ENABLE_EVAL_IMPACT=1 時のみ有効）
    EvalImpact *eval_impacts;
    struct timespec *move_start_times;  // 各手の探索開始時刻
#endif

    // Shared state
    volatile bool found_win;
    volatile int winning_move;
    volatile bool shutdown;
    volatile int tasks_completed;  // Track completed root tasks

    double time_limit;
    struct timespec start_time;
    bool use_evaluation;

    // Dynamic task spawning settings (adjustable for different hardware)
    int max_generation;         // Max depth of task spawning (default: 3, 40-core: 5)
    int min_depth_for_spawn;    // Don't spawn subtasks below this depth (default: 6, 40-core: 4)
    int spawn_threshold;        // Only spawn children with priority above this
    int spawn_limit;            // Max children to spawn per node (default: 3, 40-core: 6)

    // Subtask statistics
    volatile uint64_t subtasks_spawned;
    volatile uint64_t subtasks_completed;

    // Hybrid statistics
    volatile uint64_t total_exports;
    volatile uint64_t total_imports;
    volatile uint64_t global_switches;  // tt_store時のGlobal切り替え回数

    // Statistics
    WorkStealingStats ws_stats;
    pthread_mutex_t stats_mutex;
} GlobalState;

struct Worker {
    pthread_t thread;
    int id;
    GlobalState *global;

    // Hybrid: Per-worker LocalHeap
    LocalHeap local_heap;

    // Local statistics
    uint64_t nodes;
    uint64_t tasks_processed;
    uint64_t tasks_stolen;

    // Memory pool for node allocation (per-worker, no locking needed)
    NodePool node_pool;

    ThreadStats *stats;
    TreeStats *tree_stats;

#if ENABLE_GLOBAL_CHECK_BENCHMARK
    // Global comparison statistics (per-thread)
    uint64_t global_check_count;          // should_switch_to_global呼び出し回数
    uint64_t global_check_true_count;     // Globalの方が良かった回数
    uint64_t cumulative_nodes;            // 累積ノード数（タスク間でリセットされない）
    uint64_t nodes_at_last_check;         // 前回チェック時の累積ノード数
    uint64_t check_interval_sum;          // チェック間隔（ノード数）の合計
    uint64_t check_interval_min;          // チェック間隔の最小値
    uint64_t check_interval_max;          // チェック間隔の最大値
#endif

    // TTヒット時のGlobal切り替え用
    volatile bool should_abort_task;       // タスク中断フラグ
    int current_task_priority;             // 現在処理中のタスクの優先度

    // busy_workers追跡用
    bool is_busy;                          // このワーカーがタスクを持っているか

    // check_and_export最適化用
    bool has_entered_chunk_mode;           // 一度でもchunk modeに入ったか
    uint64_t nodes_at_last_export_check;   // 前回export checkした時のノード数
};

static void dfpn_solve_node(Worker *worker, DFPNNode *node);

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Hybrid Export/Import Operations
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Export top CHUNK_SIZE tasks from LocalHeap to GlobalChunkQueue
static void export_top_chunk(Worker *worker) {
    LocalHeap *lh = &worker->local_heap;
    GlobalChunkQueue *gq = worker->global->global_chunk_queue;

    if (lh->size < CHUNK_SIZE + 1) {
        return;  // Not enough tasks to export (keep at least 1 for self)
    }

    Chunk chunk;
    chunk.count = 0;

    // Pop the best task first (will be pushed back)
    Task best;
    if (!local_heap_pop(lh, &best)) {
        return;
    }

    // Pop CHUNK_SIZE tasks for the chunk
    for (int i = 0; i < CHUNK_SIZE && lh->size > 0; i++) {
        if (local_heap_pop(lh, &chunk.tasks[i])) {
            chunk.count++;
        }
    }

    // Push back the best task
    local_heap_push(lh, &best);

    // Push chunk to global queue
    if (chunk.count > 0) {
        chunk.top_priority = chunk.tasks[0].priority;
        if (global_chunk_queue_push(gq, &chunk)) {
            lh->exported_to_global += chunk.count;
            __sync_fetch_and_add(&worker->global->total_exports, chunk.count);

            #if VERBOSE_EXPORT_IMPORT
            printf("[Worker %d] Exported %d tasks to Global (top_priority=%d)\n",
                   worker->id, chunk.count, chunk.top_priority);
            #endif
        }
    }
}

// Check and export tasks if conditions are met
static void check_and_export(Worker *worker) {
    LocalHeap *lh = &worker->local_heap;
    GlobalChunkQueue *gq = worker->global->global_chunk_queue;

    // Condition 1: Local has enough tasks
    if (lh->size < LOCAL_EXPORT_THRESHOLD) {
        return;
    }

    int global_top = atomic_load(&gq->top_priority);
    int local_top = lh->heap[0].priority;

    // Continue exporting while conditions are met
    while (lh->size >= CHUNK_SIZE + 1) {
        bool global_empty = (global_top == INT_MIN);
        bool local_is_better = (local_top >= global_top);

        if (!global_empty && !local_is_better) {
            break;  // Global has better tasks, stop exporting
        }

        export_top_chunk(worker);

        // Update for next iteration
        global_top = atomic_load(&gq->top_priority);
        local_top = (lh->size > 0) ? lh->heap[0].priority : INT_MIN;
    }
}

// Import chunk from GlobalChunkQueue to LocalHeap
static bool import_chunk_from_global(Worker *worker, Task *out_task) {
    GlobalChunkQueue *gq = worker->global->global_chunk_queue;
    LocalHeap *lh = &worker->local_heap;

    Chunk chunk;
    if (global_chunk_queue_pop(gq, &chunk)) {
        // First task goes to caller
        *out_task = chunk.tasks[0];

        // Remaining tasks go to LocalHeap
        for (int i = 1; i < chunk.count; i++) {
            local_heap_push(lh, &chunk.tasks[i]);
        }

        lh->imported_from_global += chunk.count;
        __sync_fetch_and_add(&worker->global->total_imports, chunk.count);

        #if VERBOSE_EXPORT_IMPORT
        printf("[Worker %d] Imported %d tasks from Global (top_priority=%d)\n",
               worker->id, chunk.count, chunk.top_priority);
        #endif

        return true;
    }
    return false;
}

// Check if should switch to Global (called after tt_store)
// TTヒット時に呼び出され、Globalの優先度と現在のタスク優先度を比較
// Globalの方が良ければshould_abort_taskフラグを立てる
static bool should_switch_to_global(Worker *worker) {
    GlobalChunkQueue *gq = worker->global->global_chunk_queue;

    int global_top = atomic_load(&gq->top_priority);
    int current_priority = worker->current_task_priority;

    // Globalの優先度が現在のタスクより十分高ければ切り替え
    bool result = (global_top > current_priority);

    if (result) {
        // 中断フラグを立てる（dfpn_solve_nodeで検知）
        worker->should_abort_task = true;
    }

#if ENABLE_GLOBAL_CHECK_BENCHMARK
    // 統計を記録
    worker->global_check_count++;
    if (result) {
        worker->global_check_true_count++;
    }

    // チェック間隔（ノード数）を計算
    // NOTE: worker->nodesはタスクごとにリセットされるため、累積カウンタを使用
    uint64_t current_nodes = worker->cumulative_nodes;
    if (worker->nodes_at_last_check > 0 && current_nodes > worker->nodes_at_last_check) {
        uint64_t interval = current_nodes - worker->nodes_at_last_check;
        worker->check_interval_sum += interval;

        if (interval < worker->check_interval_min || worker->check_interval_min == 0) {
            worker->check_interval_min = interval;
        }
        if (interval > worker->check_interval_max) {
            worker->check_interval_max = interval;
        }
    }
    worker->nodes_at_last_check = current_nodes;
#endif

    return result;
}

// 高速共有モードかどうかを判定（暇なワーカーがいるか）
// ビットマップ方式: worker_has_idle() で高速判定
static inline bool is_fast_sharing_mode(GlobalState *g) {
    // 暇なワーカーがいれば高速共有モード
    // ビットマップで効率的に判定（各ワーカーが自分のビットだけを更新するため競合が少ない）
    if (worker_has_idle(&g->worker_state)) {
        return true;
    }

    // 終盤フェーズ: アクティブワーカーが閾値以下なら高速共有モード
    int active = atomic_load(&g->worker_state.active_workers);
    return (active < g->worker_state.fast_sharing_threshold);
}

// Hybrid task acquisition
static bool get_next_task_hybrid(Worker *worker, Task *out_task) {
    GlobalState *g = worker->global;
    LocalHeap *lh = &worker->local_heap;
    GlobalChunkQueue *gq = g->global_chunk_queue;

    bool fast_sharing = is_fast_sharing_mode(g);

    // ========================================
    // 高速共有モード（起動フェーズ / 終盤フェーズ）
    // - SharedTaskArrayを優先（ソート不要、CASのみ）
    // - チャンクは使わない（オーバーヘッド回避）
    // ========================================
    if (fast_sharing) {
        // 1. まずLocalHeapから取得（自分のタスクがあれば処理）
        if (lh->size > 0) {
            local_heap_pop(lh, out_task);
            return true;
        }

        // 2. SharedTaskArrayから取得（他ワーカーが投入したタスク）
        if (shared_array_pop(g->shared_array, out_task)) {
            if (DEBUG_CONFIG.track_work_stealing) {
                debug_log("Worker %d got task from SharedArray (fast_sharing, priority=%d, busy=%d/%d)\n",
                       worker->id, out_task->priority,
                       worker_count_busy(&g->worker_state),
                       g->worker_state.total_workers);
            }
            return true;
        }

        return false;
    }

    // ========================================
    // 通常モード（全ワーカー稼働中）
    // - LocalHeap + GlobalChunkQueue のハイブリッド
    // - 優先度に基づく選択
    // ========================================

    int global_top = atomic_load(&gq->top_priority);
    int local_top = (lh->size > 0) ? lh->heap[0].priority : INT_MIN;

    // Check 1: Global has better task?
    if (global_top > local_top) {
        if (import_chunk_from_global(worker, out_task)) {
            return true;
        }
    }

    // Check 2: Get from Local
    if (lh->size > 0) {
        local_heap_pop(lh, out_task);
        return true;
    }

    // Check 3: Get from Global (Local is empty)
    if (import_chunk_from_global(worker, out_task)) {
        return true;
    }

    // Fallback: SharedTaskArray (終盤に残ったタスク)
    if (shared_array_pop(g->shared_array, out_task)) {
        return true;
    }

    return false;  // No task available
}

// Share remaining tasks to SharedTaskArray (for endgame)
static void share_remaining_tasks(Worker *worker) {
    GlobalState *g = worker->global;
    LocalHeap *lh = &worker->local_heap;

    // Keep one for self, share the rest
    while (lh->size > 1) {
        Task task;
        if (!local_heap_pop(lh, &task)) {
            break;
        }
        if (!shared_array_push(g->shared_array, &task)) {
            // SharedArray is full, put it back
            local_heap_push(lh, &task);
            break;
        }
    }
}

static DFPNNode* select_best_child_with_priority(DFPNNode *node) {
    if (!node->children || node->n_children == 0) return NULL;

    // [最適化] 優先度キューを線形探索に置き換え
    // 理由: 1つの要素のみ取り出すため、O(n)の線形探索で十分
    //       malloc/freeのオーバーヘッドも削減
    int best_idx = -1;
    int best_priority = INT_MIN;

    if (node->type == NODE_OR) {
        for (int i = 0; i < node->n_children; i++) {
            int priority = (PN_INF - node->children[i]->pn) + node->children[i]->eval_score;
            if (priority > best_priority) {
                best_priority = priority;
                best_idx = i;
            }
        }
    } else {
        for (int i = 0; i < node->n_children; i++) {
            int priority = (DN_INF - node->children[i]->dn) - node->children[i]->eval_score;
            if (priority > best_priority) {
                best_priority = priority;
                best_idx = i;
            }
        }
    }

    return (best_idx >= 0) ? node->children[best_idx] : NULL;
}

static void update_pn_dn(DFPNNode *node) {
    if (node->children == NULL || node->n_children == 0) {
        return;
    }

    // 修正版: is_provenフラグを使用した厳密なDRAW伝播
    //
    // pn/dn計算は従来通り行う（探索を正しく進めるため）
    // DRAWの伝播のみ is_proven フラグで厳密に管理
    //
    // 「証明済みDRAW」の定義:
    // - is_proven == true && result == RESULT_EXACT_DRAW
    // - これは終端ノード（スコア=0）から正しく伝播されたDRAWのみ

    if (node->type == NODE_OR) {
        // ORノード: pn = min(子のpn), dn = sum(子のdn)
        uint32_t min_pn = PN_INF;
        uint64_t sum_dn = 0;

        int proven_draw_count = 0;
        int proven_win_count = 0;
        int total_proven = 0;

        for (int i = 0; i < node->n_children; i++) {
            DFPNNode *child = node->children[i];

            if (child->pn < min_pn) {
                min_pn = child->pn;
            }
            sum_dn += child->dn;
            if (sum_dn >= DN_INF) sum_dn = DN_INF;

            // 証明済み状態のカウント
            if (child->pn == 0) {
                proven_win_count++;
                total_proven++;
            } else if (child->dn == 0) {
                total_proven++;
            } else if (child->is_proven && child->result == RESULT_EXACT_DRAW) {
                proven_draw_count++;
                total_proven++;
            }
        }

        node->pn = min_pn;
        node->dn = (uint32_t)sum_dn;

        // 結果の判定
        if (node->pn == 0) {
            node->result = RESULT_EXACT_WIN;
            node->is_proven = true;
        } else if (node->dn == 0) {
            node->result = RESULT_EXACT_LOSE;
            node->is_proven = true;
        } else if (total_proven == node->n_children && proven_draw_count > 0) {
            // すべての子が証明済みで、DRAWがある → DRAW
            // (WINがあればpn=0になるので、ここには来ない)
            node->result = RESULT_EXACT_DRAW;
            node->is_proven = true;
            node->pn = PN_INF;
            node->dn = DN_INF;
        }

    } else {
        // ANDノード: pn = sum(子のpn), dn = min(子のdn)
        uint64_t sum_pn = 0;
        uint32_t min_dn = DN_INF;

        int proven_draw_count = 0;
        int proven_lose_count = 0;
        int total_proven = 0;

        for (int i = 0; i < node->n_children; i++) {
            DFPNNode *child = node->children[i];

            sum_pn += child->pn;
            if (sum_pn >= PN_INF) sum_pn = PN_INF;
            if (child->dn < min_dn) {
                min_dn = child->dn;
            }

            // 証明済み状態のカウント
            if (child->dn == 0) {
                proven_lose_count++;
                total_proven++;
            } else if (child->pn == 0) {
                total_proven++;
            } else if (child->is_proven && child->result == RESULT_EXACT_DRAW) {
                proven_draw_count++;
                total_proven++;
            }
        }

        node->pn = (uint32_t)sum_pn;
        node->dn = min_dn;

        // 結果の判定
        if (node->dn == 0) {
            node->result = RESULT_EXACT_LOSE;
            node->is_proven = true;
        } else if (node->pn == 0) {
            node->result = RESULT_EXACT_WIN;
            node->is_proven = true;
        } else if (total_proven == node->n_children && proven_draw_count > 0) {
            // すべての子が証明済みで、DRAWがある → DRAW
            // (LOSEがあればdn=0になるので、ここには来ない)
            node->result = RESULT_EXACT_DRAW;
            node->is_proven = true;
            node->pn = PN_INF;
            node->dn = DN_INF;
        }
    }
}

static void expand_node_with_evaluation(Worker *worker, DFPNNode *node) {
    uint64_t moves = get_moves(node->player, node->opponent);

    if (DEBUG_CONFIG.track_tree_stats && worker->tree_stats) {
        worker->tree_stats->expansions++;
    }

    if (moves == 0) {
        uint64_t p = node->opponent;
        uint64_t o = node->player;

        if (get_moves(p, o) == 0) {
            node->n_children = 0;
            if (DEBUG_CONFIG.track_tree_stats && worker->tree_stats) {
                worker->tree_stats->terminal_nodes++;
            }
            return;
        }

        if (DEBUG_CONFIG.track_tree_stats && worker->tree_stats) {
            worker->tree_stats->pass_nodes++;
        }

        DFPNNode *child = node_pool_alloc(&worker->node_pool);
        child->player = p;
        child->opponent = o;
        child->type = (node->type == NODE_OR) ? NODE_AND : NODE_OR;
        child->depth = node->depth;
        child->pn = 1;
        child->dn = 1;

        if (worker->global->use_evaluation) {
            child->eval_score = -evaluate_position(p, o);
        }

        node->children = malloc(sizeof(DFPNNode*));
        node->children[0] = child;
        node->n_children = 1;
        return;
    }

    int n_moves = popcount(moves);
    node->children = malloc(n_moves * sizeof(DFPNNode*));
    node->n_children = n_moves;

    if (DEBUG_CONFIG.track_tree_stats && worker->tree_stats) {
        worker->tree_stats->avg_branching_factor =
            (worker->tree_stats->avg_branching_factor * (worker->tree_stats->expansions - 1) + n_moves) /
            worker->tree_stats->expansions;
    }

    // [最適化] 評価関数の二重呼び出しを削除
    // フェーズ1: 盤面計算と評価を1回だけ実行し、配列に保存
    MoveWithEval *moves_array = malloc(n_moves * sizeof(MoveWithEval));
    PriorityQueue *pq = pq_create(n_moves);
    uint64_t moves_copy = moves;

    int idx = 0;
    while(moves_copy) {
        int move = first_one(moves_copy);
        moves_copy &= moves_copy - 1;

        uint64_t p = node->player;
        uint64_t o = node->opponent;
        make_move(&p, &o, move);

        // 結果を配列に保存
        moves_array[idx].move = move;
        moves_array[idx].player = p;
        moves_array[idx].opponent = o;

        if (worker->global->use_evaluation) {
            moves_array[idx].eval_score = -evaluate_position(p, o);  // 1回のみ評価
        } else {
            moves_array[idx].eval_score = 0;
        }

        pq_push(pq, idx, moves_array[idx].eval_score);
        idx++;
    }

    // フェーズ2: 保存された結果を使用して優先度順に子ノードを作成
    for (int i = 0; i < n_moves; i++) {
        int move_idx = pq_pop(pq);
        MoveWithEval *m = &moves_array[move_idx];

        DFPNNode *child = node_pool_alloc(&worker->node_pool);
        child->player = m->player;
        child->opponent = m->opponent;
        child->type = (node->type == NODE_OR) ? NODE_AND : NODE_OR;
        child->depth = node->depth - 1;
        child->pn = 1;
        child->dn = 1;
        child->eval_score = m->eval_score;  // 保存された評価値を使用

        node->children[i] = child;
    }

    pq_free(pq);
    free(moves_array);
}

static int get_final_score(uint64_t P, uint64_t O) {
    int p_count = popcount(P);
    int o_count = popcount(O);
    int empty = 64 - p_count - o_count;

    // プレイヤーPから見たスコアを返す
    // P > O: プレイヤーの勝ち → 正の値（空きマスもプレイヤーのものになる）
    // O > P: プレイヤーの負け → 負の値（空きマスは相手のものになる）
    // P == O: 引き分け → 0
    if (p_count > o_count) return p_count - o_count + empty;
    else if (o_count > p_count) return -(o_count - p_count + empty);
    else return 0;
}

static void dfpn_solve_node(Worker *worker, DFPNNode *node) {
    worker->nodes++;
#if ENABLE_GLOBAL_CHECK_BENCHMARK
    worker->cumulative_nodes++;
#endif

    // SPECULATIVE TT PROBE OPTIMIZATION:
    // 1. Compute hash key early
    // 2. Issue prefetch for TT entry (non-blocking)
    // 3. Do other work while memory loads
    // 4. Probe TT (data should be in cache)

    // Step 1: Compute hash key
    uint64_t key = hash_position(node->player, node->opponent);

    // Step 2: Issue prefetch for TT entry
    // This starts loading the TT entry into cache while we do other work
    TranspositionTable *tt = worker->global->tt;
    size_t tt_index = key & tt->mask;
    __builtin_prefetch(&tt->entries[tt_index], 0, 3);  // Read, high temporal locality

    // Step 3: Do other work while waiting for memory
    // Update thread stats
    if (DEBUG_CONFIG.track_threads && worker->stats) {
        worker->stats->nodes_explored = worker->nodes;
        worker->stats->current_depth = node->depth;
        worker->stats->is_active = true;
        time(&worker->stats->last_update);
    }

    // Track depth distribution
    if (DEBUG_CONFIG.track_tree_stats && worker->tree_stats && node->depth < 65) {
        worker->tree_stats->nodes_by_depth[node->depth]++;
    }

    // Check for early termination (found win or shutdown)
    if (worker->global->found_win || worker->global->shutdown) {
        return;
    }

    // Check time limit (only every 1000 nodes to reduce overhead)
    if (worker->global->time_limit > 0 && (worker->nodes & 0x3FF) == 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - worker->global->start_time.tv_sec) +
                        (now.tv_nsec - worker->global->start_time.tv_nsec) / 1e9;
        if (elapsed >= worker->global->time_limit) {
            worker->global->shutdown = true;
            return;
        }
    }

    // Step 4: Now probe TT (data should be in cache from prefetch)
    int16_t eval_score = 0;

    if (tt_probe(tt, key, node->depth, &node->pn, &node->dn, &node->result, &eval_score)) {
        if (worker->stats) worker->stats->tt_hits++;

        // TT-HIT VARIANT: Check global queue on TT hit
        // TT hits indicate a "node of interest" - previously explored position
        // Globalの方が優先度が高ければshould_abort_taskフラグが立つ
        should_switch_to_global(worker);

        // If already solved, return immediately
        if (node->result != RESULT_UNKNOWN) {
            // WIN (pn=0) または LOSE (dn=0) の場合は即座にreturn
            if (node->pn == 0 || node->dn == 0) {
                node->is_proven = true;  // TT hitでも証明済みフラグを立てる
                return;
            }
            // DRAW (pn=∞ かつ dn=∞) の場合も即座にreturn
            if (node->result == RESULT_EXACT_DRAW && node->pn == PN_INF && node->dn == DN_INF) {
                node->is_proven = true;  // TT hitでも証明済みフラグを立てる
                return;
            }
        }
        node->eval_score = eval_score;
    }

    if (node->children == NULL) {
        expand_node_with_evaluation(worker, node);

        if (node->n_children == 0) {
            // 終端ノードの判定:
            //
            // get_final_score(node->player, node->opponent) は「現在手番のプレイヤー」視点のスコアを返す。
            // しかし、df-pnでは「証明対象（ルートムーブを打ったプレイヤー）」の勝敗を判定する。
            //
            // - ルートタスクはNODE_AND（相手の手番）から開始
            // - NODE_OR: 自分の手番 → node->playerは「ルートムーブを打ったプレイヤー」
            // - NODE_AND: 相手の手番 → node->playerは「相手」
            //
            // pn/dnの意味（参考実装 df-pn.c に準拠）:
            // - pn = 0: ルートムーブを打ったプレイヤーの勝ちが証明された
            // - dn = 0: ルートムーブを打ったプレイヤーの負けが証明された
            //
            // したがって:
            // - NODE_OR（自分の手番）で score > 0 → 自分の勝ち → pn = 0
            // - NODE_OR（自分の手番）で score < 0 → 自分の負け → dn = 0
            // - NODE_AND（相手の手番）で score > 0 → 相手の勝ち = 自分の負け → dn = 0
            // - NODE_AND（相手の手番）で score < 0 → 相手の負け = 自分の勝ち → pn = 0

            int score = get_final_score(node->player, node->opponent);

            if (node->type == NODE_OR) {
                // 自分の手番: scoreはそのまま自分視点
                if (score > 0) {
                    node->result = RESULT_EXACT_WIN;
                    node->pn = 0;
                    node->dn = DN_INF;
                } else if (score < 0) {
                    node->result = RESULT_EXACT_LOSE;
                    node->pn = PN_INF;
                    node->dn = 0;
                } else {
                    node->result = RESULT_EXACT_DRAW;
                    node->pn = PN_INF;
                    node->dn = DN_INF;
                }
            } else {
                // NODE_AND: 相手の手番 → scoreは相手視点なので反転が必要
                if (score > 0) {
                    // 相手の勝ち = 自分の負け
                    node->result = RESULT_EXACT_LOSE;
                    node->pn = PN_INF;
                    node->dn = 0;
                } else if (score < 0) {
                    // 相手の負け = 自分の勝ち
                    node->result = RESULT_EXACT_WIN;
                    node->pn = 0;
                    node->dn = DN_INF;
                } else {
                    node->result = RESULT_EXACT_DRAW;
                    node->pn = PN_INF;
                    node->dn = DN_INF;
                }
            }
            // 終端ノードは常に証明済み
            node->is_proven = true;

            tt_store(worker->global->tt, key, node->depth, node->pn, node->dn, node->result, node->eval_score);
            if (worker->stats) worker->stats->tt_stores++;
            return;
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // ★ フェーズ4修正: 探索前の早期スポーン（EARLY SPAWN）
    // expand直後にアイドルワーカーがいれば、未証明の子ノードを即座にスポーン
    // これにより、探索中に子ノードが証明されてスポーン機会を逃す問題を解決
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    if (node->children != NULL && node->n_children > 1) {
        bool has_idle = worker_has_idle(&worker->global->worker_state);
        int busy_count = worker_count_busy(&worker->global->worker_state);
        int total_workers = worker->global->worker_state.total_workers;
        float idle_rate = 1.0f - (float)busy_count / (float)total_workers;

        // ★ ローカルヒープサイズで判定（startup_phaseの代わり）
        int local_size = worker->local_heap.size;
        bool local_heap_needs_fill = (local_size < CHUNK_SIZE);

        // SharedArrayの使用率をチェック（オーバーフロー防止）
        uint32_t sa_tail = atomic_load(&worker->global->shared_array->tail);
        uint32_t sa_head = atomic_load(&worker->global->shared_array->head);
        int shared_usage = (int)(sa_tail - sa_head);
        float shared_rate = (float)shared_usage / worker->global->shared_array->capacity;
        bool shared_has_space = (shared_rate < 0.7f);  // 70%未満なら余裕あり

        // ローカルヒープがチャンク未満、またはアイドル率50%以上、または深さが十分な場合に早期スポーン
        // ただしSharedArrayに余裕がある場合のみ
        if (shared_has_space && (local_heap_needs_fill || (has_idle && idle_rate > 0.5f) || node->depth >= worker->global->min_depth_for_spawn)) {
            int early_spawned = 0;
            // ローカルヒープ不足時でもスポーン数を制限（最大15）、それ以外はアイドル率に応じて制限
            int max_early_spawn = local_heap_needs_fill ? ((node->n_children - 1) > 15 ? 15 : (node->n_children - 1)) :
                                  (idle_rate > 0.9f) ? 5 : (idle_rate > 0.7f) ? 3 : 2;

            for (int i = 1; i < node->n_children && early_spawned < max_early_spawn; i++) {
                DFPNNode *child = node->children[i];
                if (child->pn == 0 || child->dn == 0) continue;

                int priority;
                if (node->type == NODE_OR) {
                    priority = (PN_INF - child->pn) / 1000 + child->eval_score;
                } else {
                    priority = (DN_INF - child->dn) / 1000 - child->eval_score;
                }

                Task subtask = {
                    .player = child->player,
                    .opponent = child->opponent,
                    .root_move = 0,  // 探索途中なのでroot_moveは不明
                    .priority = priority + 4000,  // 高優先度
                    .eval_score = child->eval_score,
                    .is_root_task = false,
                    .depth = child->depth,
                    .node_type = child->type,
                    .generation = 3  // 早期スポーンのマーカー
                };

                if (shared_array_push(worker->global->shared_array, &subtask)) {
                    early_spawned++;
                    __sync_fetch_and_add(&worker->global->subtasks_spawned, 1);
                }
            }

            if (early_spawned > 0 && DEBUG_CONFIG.verbose) {
                debug_log("Worker %d: EARLY SPAWN at depth=%d, spawned %d (idle=%.1f%%, local_fill=%s)\n",
                          worker->id, node->depth, early_spawned, idle_rate * 100,
                          local_heap_needs_fill ? "YES" : "NO");
            }
        }
    }
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    // df-pn.cの参考: pn >= INF または dn >= INF で終了（WIN/LOSE/DRAW全ての証明完了を検出）
    uint64_t loop_count = 0;  // ★ フェーズ2: ループカウンタ

    while(node->pn > 0 && node->dn > 0 &&
          node->pn < PN_INF && node->dn < DN_INF &&  // 証明完了時に終了
          node->pn < node->threshold_pn && node->dn < node->threshold_dn) {
        // 終了条件チェック（WINが見つかった/タイムアウト/Global切り替え）
        if (worker->global->found_win || worker->global->shutdown) return;
        if (worker->should_abort_task) return;  // Global優先度が高いので中断

        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        // ★ フェーズ2修正: 探索途中でのスポーン判定（50ループごと）
        // 500→50に変更: 探索がすぐ終了する場合でも中間スポーンを発生させる
        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        loop_count++;
        if (loop_count >= 50 && node->children != NULL && node->n_children > 1) {
            loop_count = 0;

            // アイドルWorkerがいるか確認
            bool has_idle = worker_has_idle(&worker->global->worker_state);
            if (has_idle && node->pn > 0 && node->dn > 0) {
                // 未証明の子ノードをスポーン（最大2つ）
                int spawned = 0;
                for (int i = 0; i < node->n_children && spawned < 2; i++) {
                    DFPNNode *c = node->children[i];
                    if (c->pn == 0 || c->dn == 0) continue;  // 証明済みスキップ
                    if (c->depth < worker->global->min_depth_for_spawn / 2) continue;  // 浅すぎスキップ

                    int priority;
                    if (node->type == NODE_OR) {
                        priority = (PN_INF - c->pn) / 1000 + c->eval_score;
                    } else {
                        priority = (DN_INF - c->dn) / 1000 - c->eval_score;
                    }

                    Task subtask = {
                        .player = c->player,
                        .opponent = c->opponent,
                        .root_move = 0,  // 探索途中なのでroot_moveは不明
                        .priority = priority + 3000,
                        .eval_score = c->eval_score,
                        .is_root_task = false,
                        .depth = c->depth,
                        .node_type = c->type,
                        .generation = 5  // 探索途中スポーンのマーカー
                    };

                    if (shared_array_push(worker->global->shared_array, &subtask)) {
                        spawned++;
                        __sync_fetch_and_add(&worker->global->subtasks_spawned, 1);
                    }
                }

                if (spawned > 0 && DEBUG_CONFIG.verbose) {
                    debug_log("Worker %d: MID-SEARCH SPAWN at depth=%d, spawned %d\n",
                              worker->id, node->depth, spawned);
                }
            }
        }
        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

        DFPNNode *child = select_best_child_with_priority(node);
        if (child == NULL) break;

        if (node->type == NODE_OR) {
            child->threshold_pn = node->threshold_dn - node->dn + child->dn;
            child->threshold_dn = node->threshold_pn;
        } else {
            child->threshold_pn = node->threshold_pn;
            child->threshold_dn = node->threshold_dn - node->dn + child->dn;
        }

        dfpn_solve_node(worker, child);
        update_pn_dn(node);

        if (DEBUG_CONFIG.track_tree_stats && worker->tree_stats) {
            worker->tree_stats->pn_dn_updates++;
        }

    }

    tt_store(worker->global->tt, key, node->depth, node->pn, node->dn, node->result, node->eval_score);
    if (worker->stats) worker->stats->tt_stores++;

    // TT-HIT VARIANT: Global check is done only on TT hit (in tt_probe branch)
    // (removed from here to reduce check frequency)
}

// Free only the children arrays (nodes are managed by memory pool)
static void free_dfpn_tree_children(DFPNNode *node) {
    if (!node) return;
    for (int i = 0; i < node->n_children; i++) {
        free_dfpn_tree_children(node->children[i]);
    }
    if (node->children) {
        free(node->children);
        node->children = NULL;
    }
}

// Spawn child tasks for promising children (HYBRID VERSION)
// Uses LocalHeap instead of global TaskQueue
// Returns number of tasks spawned
static int spawn_child_tasks(Worker *worker, DFPNNode *node, Task *parent_task) {
    if (!node->children || node->n_children == 0) return 0;

    // Check if we should spawn subtasks
    int generation = parent_task->generation;
    int spawn_limit = worker->global->spawn_limit;  // デフォルト: 3

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // ★ フェーズ3修正: アイドル率に応じたパラメータの動的調整
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    int busy_count = worker_count_busy(&worker->global->worker_state);
    int total_workers = worker->global->worker_state.total_workers;
    float idle_rate = 1.0f - (float)busy_count / (float)total_workers;

    // 動的パラメータの初期値
    int effective_max_gen = worker->global->max_generation;
    int effective_spawn_limit = spawn_limit;
    int effective_min_depth = worker->global->min_depth_for_spawn;

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // ★ ローカルヒープ保持スポーン（保守的版）
    // ローカルヒープがチャンク未満なら、制限を緩和（ただし無制限ではない）
    // SharedArrayオーバーフロー防止のため、上限を設ける
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    int local_size = worker->local_heap.size;
    bool local_heap_needs_fill = (local_size < CHUNK_SIZE);

    // SharedArrayの使用率をチェック（オーバーフロー防止）
    uint32_t sa_tail2 = atomic_load(&worker->global->shared_array->tail);
    uint32_t sa_head2 = atomic_load(&worker->global->shared_array->head);
    int shared_array_usage = (int)(sa_tail2 - sa_head2);
    int shared_array_capacity = worker->global->shared_array->capacity;
    float shared_usage_rate = (float)shared_array_usage / shared_array_capacity;
    bool shared_array_has_space = (shared_usage_rate < 0.8f);  // 80%未満なら余裕あり

    if (local_heap_needs_fill && shared_array_has_space) {
        // ローカルヒープがチャンク未満 かつ SharedArrayに余裕あり
        // → 制限を緩和（ただし保守的に）
        effective_max_gen = effective_max_gen + 20;  // 世代制限を+20（無制限ではない）
        effective_spawn_limit = 50;                   // スポーン数上限50
        effective_min_depth = (effective_min_depth > 3) ? effective_min_depth / 2 : 2;

        if (DEBUG_CONFIG.verbose && (worker->nodes & 0xFFF) == 0) {
            debug_log("Worker %d: LOCAL-HEAP-FILL (local=%d, shared=%.1f%%): gen=%d, limit=%d, depth=%d\n",
                     worker->id, local_size, shared_usage_rate * 100,
                     effective_max_gen, effective_spawn_limit, effective_min_depth);
        }
    } else if (local_heap_needs_fill && !shared_array_has_space) {
        // SharedArrayが混雑 → スポーンを控える
        if (DEBUG_CONFIG.verbose && (worker->nodes & 0xFFFF) == 0) {
            debug_log("Worker %d: LOCAL-HEAP-FILL blocked (shared=%.1f%% full)\n",
                     worker->id, shared_usage_rate * 100);
        }
        return 0;  // スポーンしない
    } else if (idle_rate > 0.9f) {
        // 90%以上アイドル → 大幅緩和
        effective_max_gen += 10;
        effective_spawn_limit *= 5;
        effective_min_depth = effective_min_depth / 2;
        if (DEBUG_CONFIG.verbose && (worker->nodes & 0xFFFF) == 0) {
            debug_log("Worker %d: DYNAMIC PARAMS (idle=%.1f%%): max_gen=%d, spawn=%d, min_depth=%d\n",
                     worker->id, idle_rate * 100, effective_max_gen, effective_spawn_limit, effective_min_depth);
        }
    } else if (idle_rate > 0.7f) {
        // 70%以上アイドル → 中程度緩和
        effective_max_gen += 5;
        effective_spawn_limit *= 3;
        effective_min_depth = (effective_min_depth * 2) / 3;
    } else if (idle_rate > 0.5f) {
        // 50%以上アイドル → 軽度緩和
        effective_max_gen += 2;
        effective_spawn_limit *= 2;
    }
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // ★ アイドル駆動スポーン + LocalHeap保持スポーン
    // Generation制限をアイドル状況またはLocalHeap状況に応じて緩和
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    if (generation >= effective_max_gen) {  // ★ フェーズ3: 動的パラメータを使用
        // Generation制限を超えた

        // アイドルWorkerがいるかチェック
        bool has_idle = worker_has_idle(&worker->global->worker_state);

        // LocalHeapのサイズをチェック
        int local_size = worker->local_heap.size;
        const int chunk_size = CHUNK_SIZE;  // 16

        if (DEBUG_CONFIG.verbose) {
            debug_log("Worker %d: generation %d >= max %d, has_idle=%d, local_size=%d, chunk_size=%d, depth=%d, min_depth=%d\n",
                     worker->id, generation, worker->global->max_generation,
                     has_idle, local_size, chunk_size, node->depth, worker->global->min_depth_for_spawn);
        }

        // スポーンする条件:
        // 1. アイドルWorkerがいる OR
        // 2. LocalHeapがチャンク未満（常にチャンク以上保持したい）
        if (!has_idle && local_size >= chunk_size) {
            return 0;  // 全員忙しい かつ LocalHeapに十分タスクがある → スポーンしない
        }

        // アイドルがいる OR LocalHeapが少ない → スポーン許可
        // spawn_limitは制限しない（デフォルト値を使用: -S オプションで指定可能）

        // depth制限は維持（★ フェーズ3: 動的パラメータを使用）
        if (node->depth < effective_min_depth) {
            if (DEBUG_CONFIG.verbose) {
                debug_log("Worker %d: spawn blocked by depth (depth=%d < min=%d, effective=%d)\n",
                         worker->id, node->depth, worker->global->min_depth_for_spawn, effective_min_depth);
            }
            return 0;
        }

        if (DEBUG_CONFIG.verbose) {
            if (has_idle) {
                debug_log("Worker %d: IDLE-DRIVEN SPAWN ENABLED (gen=%d, depth=%d, local_size=%d, spawn_limit=%d)\n",
                         worker->id, generation, node->depth, local_size, spawn_limit);
            } else {
                debug_log("Worker %d: LOCAL-HEAP-PRESERVE SPAWN ENABLED (gen=%d, depth=%d, local_size=%d, spawn_limit=%d)\n",
                         worker->id, generation, node->depth, local_size, spawn_limit);
            }
        }

        // ここを通過 → generation >= G でもスポーン継続
    } else {
        // 通常のGeneration範囲内（★ フェーズ3: 動的パラメータを使用）
        if (node->depth < effective_min_depth) return 0;
    }

    // Calculate priorities for all children
    typedef struct {
        int index;
        int priority;
        int eval_score;
    } ChildPriority;

    ChildPriority *children_prio = malloc(node->n_children * sizeof(ChildPriority));
    int best_priority = -999999;

    for (int i = 0; i < node->n_children; i++) {
        DFPNNode *child = node->children[i];
        int priority;

        if (node->type == NODE_OR) {
            // OR node: prefer low pn (easy to prove win)
            priority = (PN_INF - child->pn) / 1000 + child->eval_score;
        } else {
            // AND node: prefer low dn (easy to prove loss for opponent)
            priority = (DN_INF - child->dn) / 1000 - child->eval_score;
        }

        children_prio[i].index = i;
        children_prio[i].priority = priority;
        children_prio[i].eval_score = child->eval_score;

        if (priority > best_priority) {
            best_priority = priority;
        }
    }

    // Spawn tasks for promising children (skip the best one, we'll handle it ourselves)
    int spawned = 0;
    // spawn_limitは上で設定済み（通常3 or アイドル駆動2）

    // [最適化] ループ前に1回だけモード判定（atomic_load削減）
    // ループ中にモードが変わっても、フォールバック機構で問題なく動作する
    bool fast_sharing = is_fast_sharing_mode(worker->global);

    // ★ フェーズ3: 動的スポーン制限を使用
    for (int i = 0; i < node->n_children && spawned < effective_spawn_limit; i++) {
        DFPNNode *child = node->children[i];

        // Skip already proven nodes
        if (child->pn == 0 || child->dn == 0) continue;

        // Only spawn if priority is good enough (within 80% of best)
        if (children_prio[i].priority < best_priority * 0.8 &&
            children_prio[i].priority < worker->global->spawn_threshold) {
            continue;
        }

        // Create subtask
        Task subtask = {
            .player = child->player,
            .opponent = child->opponent,
            .root_move = parent_task->root_move,  // Track original root move
            .priority = children_prio[i].priority + 5000 - generation * 1000,  // Boost priority
            .eval_score = children_prio[i].eval_score,
            .is_root_task = false,
            .depth = child->depth,
            .node_type = child->type,
            .generation = generation + 1
        };

        // HYBRID: 高速共有モードではSharedTaskArrayを使用、通常モードではLocalHeap
        // (fast_sharingはループ前に1回だけ判定済み)

        if (fast_sharing) {
            // 高速共有モード:
            // - 最初の1つは自分のLocalHeapに残す（自分が処理する）
            // - 残りはSharedTaskArrayに投入（他ワーカーがすぐ取得可能）
            if (spawned == 0) {
                // 最初のタスクは自分用にLocalHeapへ
                if (local_heap_push(&worker->local_heap, &subtask)) {
                    spawned++;
                    __sync_fetch_and_add(&worker->global->subtasks_spawned, 1);

                    if (DEBUG_CONFIG.track_work_stealing) {
                        debug_log("Worker %d spawned subtask gen=%d for root=%c%d, priority=%d, depth=%d (LocalHeap - keep for self)\n",
                               worker->id, generation + 1,
                               'a' + (parent_task->root_move % 8), 8 - (parent_task->root_move / 8),
                               subtask.priority, child->depth);
                    }
                }
            } else {
                // 2つ目以降はSharedTaskArrayへ
                if (shared_array_push(worker->global->shared_array, &subtask)) {
                    spawned++;
                    __sync_fetch_and_add(&worker->global->subtasks_spawned, 1);

                    if (DEBUG_CONFIG.track_work_stealing) {
                        debug_log("Worker %d spawned subtask gen=%d for root=%c%d, priority=%d, depth=%d (SharedArray - for others, busy=%d/%d)\n",
                               worker->id, generation + 1,
                               'a' + (parent_task->root_move % 8), 8 - (parent_task->root_move / 8),
                               subtask.priority, child->depth,
                               worker_count_busy(&worker->global->worker_state),
                               worker->global->worker_state.total_workers);
                    }
                }
            }
        } else {
            // 通常モード: LocalHeapに追加
            if (local_heap_push(&worker->local_heap, &subtask)) {
                spawned++;
                __sync_fetch_and_add(&worker->global->subtasks_spawned, 1);

                if (DEBUG_CONFIG.track_work_stealing) {
                    debug_log("Worker %d spawned subtask gen=%d for root=%c%d, priority=%d, depth=%d (LocalHeap, busy=%d/%d)\n",
                           worker->id, generation + 1,
                           'a' + (parent_task->root_move % 8), 8 - (parent_task->root_move / 8),
                           subtask.priority, child->depth,
                           worker_count_busy(&worker->global->worker_state),
                           worker->global->worker_state.total_workers);
                }
            }
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // ★ アイドル駆動型エクスポート（fast_sharingに関係なく実行）
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    // 一度でもスポーンしたことを記録
    if (!worker->has_entered_chunk_mode) {
        worker->has_entered_chunk_mode = true;
        worker->nodes_at_last_export_check = worker->nodes;
    }

    // 1000ノードごとにアイドルチェック
    uint64_t nodes_since_check = worker->nodes - worker->nodes_at_last_export_check;
    if (nodes_since_check >= 1000) {
        bool has_idle = worker_has_idle(&worker->global->worker_state);

        if (has_idle && worker->local_heap.size > 1) {
            // アイドルWorkerがいて、LocalHeapにタスクがある
            // → 自分用に1つ残して全てSharedArrayへエクスポート
            LocalHeap *lh = &worker->local_heap;
            int exported_count = 0;

            while (lh->size > 1) {  // 自分用に1つ残す
                Task task;
                if (local_heap_pop(lh, &task)) {
                    if (shared_array_push(worker->global->shared_array, &task)) {
                        exported_count++;
                        lh->exported_to_global++;
                    } else {
                        // SharedArrayが満杯 → タスクを戻す
                        local_heap_push(lh, &task);
                        break;
                    }
                }
            }

            if (exported_count > 0) {
                debug_log("Worker %d: Idle-driven export, %d tasks to SharedArray (kept 1 for self)\n",
                         worker->id, exported_count);
            }
        }

        worker->nodes_at_last_export_check = worker->nodes;
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // 通常のチャンクエクスポート（chunk modeのみ）
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    if (!fast_sharing) {
        // check_and_export()は優先度比較ベースのエクスポート
        if (nodes_since_check >= 1000) {
            check_and_export(worker);
        }
    }

    free(children_prio);
    return spawned;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ★ フェーズ1修正: ルートタスクの即座分割
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//
// 目的: ルートタスクを取得したWorkerが即座に子ノードをタスク化し、
//       他のWorkerに仕事を供給することで並列性を向上させる
//
// 動作:
// 1. ルートノードを作成し子ノードを展開
// 2. 全ての子ノードをサブタスクとしてSharedArrayにスポーン
// 3. 最良の子ノードのみ自身で処理
//
// 期待効果:
// - 初期手11個 → 各手の子ノード（10-30個）がタスク化
// - 合計100-300個のタスクが即座に生成
// - アイドルWorkerが即座に作業開始可能
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static bool process_root_task_with_split(Worker *worker, Task *task) {
    uint64_t p = task->player;
    uint64_t o = task->opponent;

    debug_log("Worker %d: ROOT SPLIT START for move %c%d\n",
              worker->id,
              'a' + (task->root_move % 8), 8 - (task->root_move / 8));

    // ルートノード作成
    DFPNNode *root = node_pool_alloc(&worker->node_pool);
    root->player = p;
    root->opponent = o;
    root->type = NODE_AND;  // 相手番（自分が打った後なので）
    root->depth = popcount(~(p | o));
    root->pn = 1;
    root->dn = 1;
    root->eval_score = task->eval_score;
    root->threshold_pn = PN_INF + 1;
    root->threshold_dn = DN_INF + 1;
    root->children = NULL;
    root->n_children = 0;
    root->result = RESULT_UNKNOWN;
    root->is_proven = false;

    // 子ノードを即座に展開
    expand_node_with_evaluation(worker, root);

    // 子がない場合は通常処理にフォールバック
    if (root->children == NULL || root->n_children == 0) {
        debug_log("Worker %d: ROOT SPLIT - no children, fallback to normal\n", worker->id);
        dfpn_solve_node(worker, root);

        // 結果判定
        uint64_t key = hash_position(p, o);
        Result result = RESULT_UNKNOWN;
        if (root->pn == 0) {
            result = (root->type == NODE_OR) ? RESULT_EXACT_WIN : RESULT_EXACT_LOSE;
        } else if (root->dn == 0) {
            result = (root->type == NODE_OR) ? RESULT_EXACT_LOSE : RESULT_EXACT_WIN;
        }
        tt_store(worker->global->tt, key, root->depth, root->pn, root->dn, result, root->eval_score);

        // WIN報告
        if (result == RESULT_EXACT_WIN) {
            if (atomic_cas_bool(&worker->global->found_win, false, true)) {
                __atomic_store_n(&worker->global->winning_move, task->root_move, __ATOMIC_RELEASE);
                debug_log("Worker %d: ROOT SPLIT found WIN for %c%d (no children case)\n",
                          worker->id,
                          'a' + (task->root_move % 8), 8 - (task->root_move / 8));
            }
        }

        free_dfpn_tree_children(root);
        node_pool_reset(&worker->node_pool);
        return true;
    }

    debug_log("Worker %d: ROOT SPLIT - %d children found\n", worker->id, root->n_children);

    // 最良子ノードを特定（自分で処理する用）
    int best_idx = 0;
    int best_priority = INT_MIN;
    for (int i = 0; i < root->n_children; i++) {
        DFPNNode *child = root->children[i];
        int priority;
        if (root->type == NODE_OR) {
            priority = (PN_INF - child->pn) / 1000 + child->eval_score;
        } else {
            priority = (DN_INF - child->dn) / 1000 - child->eval_score;
        }
        if (priority > best_priority) {
            best_priority = priority;
            best_idx = i;
        }
    }

    // 最良以外の子をSharedArrayにスポーン
    int spawned = 0;
    for (int i = 0; i < root->n_children; i++) {
        if (i == best_idx) continue;  // 最良は自分で処理

        DFPNNode *child = root->children[i];
        if (child->pn == 0 || child->dn == 0) continue;  // 証明済みスキップ

        int priority;
        if (root->type == NODE_OR) {
            priority = (PN_INF - child->pn) / 1000 + child->eval_score;
        } else {
            priority = (DN_INF - child->dn) / 1000 - child->eval_score;
        }

        Task subtask = {
            .player = child->player,
            .opponent = child->opponent,
            .root_move = task->root_move,
            .priority = priority + 10000,  // 高優先度
            .eval_score = child->eval_score,
            .is_root_task = false,
            .depth = child->depth,
            .node_type = child->type,
            .generation = 1
        };

        if (shared_array_push(worker->global->shared_array, &subtask)) {
            spawned++;
            __sync_fetch_and_add(&worker->global->subtasks_spawned, 1);
        }
    }

    debug_log("Worker %d: ROOT SPLIT for %c%d, spawned %d/%d children\n",
              worker->id,
              'a' + (task->root_move % 8), 8 - (task->root_move / 8),
              spawned, root->n_children - 1);

    // 最良子ノードを自分で処理
    if (root->children[best_idx]->pn > 0 && root->children[best_idx]->dn > 0) {
        dfpn_solve_node(worker, root->children[best_idx]);
    }
    update_pn_dn(root);

    // 結果判定とTT保存
    uint64_t key = hash_position(p, o);
    Result result = RESULT_UNKNOWN;
    if (root->pn == 0) {
        result = (root->type == NODE_OR) ? RESULT_EXACT_WIN : RESULT_EXACT_LOSE;
    } else if (root->dn == 0) {
        result = (root->type == NODE_OR) ? RESULT_EXACT_LOSE : RESULT_EXACT_WIN;
    } else if (root->pn >= PN_INF && root->dn >= DN_INF) {
        result = RESULT_EXACT_DRAW;
    }
    tt_store(worker->global->tt, key, root->depth, root->pn, root->dn, result, root->eval_score);

    // ルートタスクの結果を記録
    int move_idx = -1;
    for (int i = 0; i < worker->global->n_moves; i++) {
        if (worker->global->move_list[i] == task->root_move) {
            move_idx = i;
            break;
        }
    }

    if (move_idx >= 0) {
        atomic_add_u64(&worker->global->move_nodes[move_idx], worker->nodes);

        if (result != RESULT_UNKNOWN) {
            Result current = (Result)__atomic_load_n(&worker->global->move_results[move_idx], __ATOMIC_ACQUIRE);
            if (current == RESULT_UNKNOWN) {
                __sync_bool_compare_and_swap(&worker->global->move_results[move_idx],
                                              RESULT_UNKNOWN, result);
            }

            // WIN報告と早期終了
            if (result == RESULT_EXACT_WIN) {
                if (atomic_cas_bool(&worker->global->found_win, false, true)) {
                    __atomic_store_n(&worker->global->winning_move, task->root_move, __ATOMIC_RELEASE);
                    debug_log("Worker %d: ROOT SPLIT found WIN for %c%d!\n",
                              worker->id,
                              'a' + (task->root_move % 8), 8 - (task->root_move / 8));

                    if (worker->global->global_chunk_queue) {
                        pthread_cond_broadcast(&worker->global->global_chunk_queue->cond);
                    }
                }
            }

            atomic_add_int(&worker->global->tasks_completed, 1);
        } else {
            // まだ証明されていない → 再キュー（通常処理として）
            if (!worker->global->found_win && !worker->global->shutdown) {
                Task retry_task = *task;
                retry_task.priority = task->priority - 100;
                retry_task.generation = 1;  // ★ generation=1にして通常処理にする（無限ループ防止）
                local_heap_push(&worker->local_heap, &retry_task);

                debug_log("Worker %d: ROOT SPLIT %c%d not proven (pn=%u, dn=%u), re-enqueued as normal task\n",
                          worker->id,
                          'a' + (task->root_move % 8), 8 - (task->root_move / 8),
                          root->pn, root->dn);
            }
        }
    }

    free_dfpn_tree_children(root);
    node_pool_reset(&worker->node_pool);
    return true;
}

// Process a single task with dynamic task spawning
// Uses iterative threshold widening and spawns subtasks for parallelism
// Returns true if task was fully processed, false if aborted for Global switch
static bool process_task(Worker *worker, Task *task) {
    worker->tasks_processed++;

    // ★ フェーズ1修正: ルートタスク（generation=0）は即座分割処理へ分岐
    if (task->is_root_task && task->generation == 0) {
        return process_root_task_with_split(worker, task);
    }

    // TTヒット時のGlobal切り替え用: 現在のタスク優先度を記録
    worker->current_task_priority = task->priority;
    worker->should_abort_task = false;

    if (DEBUG_CONFIG.track_work_stealing) {
        if (task->is_root_task) {
            debug_log("Worker %d processing ROOT task: move=%c%d, priority=%d\n",
                   worker->id,
                   'a' + (task->root_move % 8), 8 - (task->root_move / 8),
                   task->priority);
        } else {
            debug_log("Worker %d processing SUBTASK gen=%d: root=%c%d, priority=%d, depth=%d\n",
                   worker->id, task->generation,
                   'a' + (task->root_move % 8), 8 - (task->root_move / 8),
                   task->priority, task->depth);
        }
    }

    uint64_t p = task->player;
    uint64_t o = task->opponent;

    // For root tasks, node type is AND (opponent's turn after our move)
    // For subtasks, use the stored node type
    NodeType type = task->is_root_task ? NODE_AND : task->node_type;

    DFPNNode *root = node_pool_alloc(&worker->node_pool);
    root->player = p;
    root->opponent = o;
    root->type = type;
    root->depth = task->is_root_task ? popcount(~(p | o)) : task->depth;
    root->pn = 1;
    root->dn = 1;
    root->eval_score = task->eval_score;

    // Set thresholds to infinity + 1 - no artificial cutoff
    // Priority ordering and time limit control the search
    // Note: PN_INF + 1 is needed because when pn = PN_INF, we still want pn < threshold_pn to be true
    root->threshold_pn = PN_INF + 1;
    root->threshold_dn = DN_INF + 1;

    // Perform the search (TT probe is done inside dfpn_solve_node)
    uint64_t key = hash_position(p, o);
    dfpn_solve_node(worker, root);

    // TTヒット時にGlobalの方が優先度が高いと判断された場合、タスクを中断
    // 現在のタスクをLocalHeapに戻し、Globalから新しいタスクを取得する
    if (worker->should_abort_task) {
        // 現在の途中結果をTTに保存（次回の探索で再利用）
        tt_store(worker->global->tt, key, root->depth, root->pn, root->dn, RESULT_UNKNOWN, root->eval_score);

        // ツリーのクリーンアップ
        free_dfpn_tree_children(root);
        node_pool_reset(&worker->node_pool);

        // 統計
        __sync_fetch_and_add(&worker->global->global_switches, 1);

        if (DEBUG_CONFIG.track_work_stealing) {
            debug_log("Worker %d: task aborted for Global switch (task_priority=%d)\n",
                   worker->id, task->priority);
        }

        return false;  // タスク中断
    }

    // Spawn child tasks for parallel exploration if not yet proven
    if (root->children != NULL && root->pn > 0 && root->dn > 0 &&
        !worker->global->found_win && !worker->global->shutdown) {
        int spawned = spawn_child_tasks(worker, root, task);
        if (spawned > 0 && DEBUG_CONFIG.verbose) {
            debug_log("Worker %d spawned %d subtasks\n", worker->id, spawned);
        }
    }

    // Determine result from pn/dn
    //
    // FIXED: ノードタイプを考慮した結果判定
    //
    // df-pnでは、pn/dnはそのノード視点での証明/反証を表す。
    // しかし、ルートタスクはNODE_AND（相手の手番）なので、結果を反転する必要がある。
    //
    // NODE_OR（自分の手番）:
    //   - pn = 0 → 自分が勝てる → WIN
    //   - dn = 0 → 自分が勝てない → LOSE
    //
    // NODE_AND（相手の手番）:
    //   - pn = 0 → 相手が全ての手で勝てる → LOSE
    //   - dn = 0 → 相手の少なくとも1手で負ける → WIN
    //
    Result result = RESULT_UNKNOWN;
    if (root->pn == 0) {
        // 証明完了: ノードタイプに応じて結果を判定
        result = (root->type == NODE_OR) ? RESULT_EXACT_WIN : RESULT_EXACT_LOSE;
    } else if (root->dn == 0) {
        // 反証完了: ノードタイプに応じて結果を判定
        result = (root->type == NODE_OR) ? RESULT_EXACT_LOSE : RESULT_EXACT_WIN;
    } else if (root->pn >= PN_INF) {
        // pn >= INF: 証明不可能
        if (root->dn >= DN_INF) {
            // dn >= INF: 反証も不可能 → DRAW
            result = RESULT_EXACT_DRAW;
        } else {
            // dn < INF: 反証可能 → LOSE (ノードタイプ考慮)
            result = (root->type == NODE_OR) ? RESULT_EXACT_LOSE : RESULT_EXACT_WIN;
        }
    } else if (root->dn >= DN_INF) {
        // dn >= INF && pn < INF: 反証不可能、証明可能 → WIN (ノードタイプ考慮)
        result = (root->type == NODE_OR) ? RESULT_EXACT_WIN : RESULT_EXACT_LOSE;
    }
    // 注: pn < INF && dn < INF なら UNKNOWN のまま（探索未完了）

    // Store result in TT for other workers to find
    tt_store(worker->global->tt, key, root->depth, root->pn, root->dn, result, root->eval_score);

    // Update global results for root tasks (LOCK-FREE)
    if (task->is_root_task) {
        // Find the index for this root move (move_list is read-only, no lock needed)
        int move_idx = -1;
        for (int i = 0; i < worker->global->n_moves; i++) {
            if (worker->global->move_list[i] == task->root_move) {
                move_idx = i;
                break;
            }
        }

        if (move_idx >= 0) {
            // Atomic update of move_nodes
            atomic_add_u64(&worker->global->move_nodes[move_idx], worker->nodes);

            // Atomic update of move_results using CAS
            // Only upgrade from UNKNOWN to definitive result
            if (result != RESULT_UNKNOWN) {
                Result current = (Result)__atomic_load_n(&worker->global->move_results[move_idx], __ATOMIC_ACQUIRE);
                if (current == RESULT_UNKNOWN) {
                    __sync_bool_compare_and_swap(&worker->global->move_results[move_idx],
                                                  RESULT_UNKNOWN, result);

#if ENABLE_EVAL_IMPACT
                    // EvalImpact記録（結果が確定した時）
                    // 評価関数影響分析: 各手の探索結果を記録
                    if (DEBUG_CONFIG.track_eval_impact && worker->global->eval_impacts) {
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        struct timespec *start = &worker->global->move_start_times[move_idx];
                        double time_spent = (now.tv_sec - start->tv_sec) +
                                           (now.tv_nsec - start->tv_nsec) / 1e9;

                        worker->global->eval_impacts[move_idx].result = result;
                        worker->global->eval_impacts[move_idx].nodes_searched = worker->global->move_nodes[move_idx];
                        worker->global->eval_impacts[move_idx].time_spent = time_spent;
                        worker->global->eval_impacts[move_idx].pn_final = root->pn;
                        worker->global->eval_impacts[move_idx].dn_final = root->dn;
                        worker->global->eval_impacts[move_idx].nps = (time_spent > 0) ?
                            worker->global->move_nodes[move_idx] / time_spent : 0;
                        worker->global->eval_impacts[move_idx].was_cutoff = false;
                    }
#endif // ENABLE_EVAL_IMPACT
                }

                // Early termination if WIN found (atomic CAS to prevent race)
                if (result == RESULT_EXACT_WIN) {
                    // Try to set found_win from false to true
                    if (atomic_cas_bool(&worker->global->found_win, false, true)) {
                        // We won the race - set winning_move
                        __atomic_store_n(&worker->global->winning_move, task->root_move, __ATOMIC_RELEASE);
                        debug_log("Worker %d found WIN for move %c%d! Early termination.\n",
                               worker->id,
                               'a' + (task->root_move % 8), 8 - (task->root_move / 8));

                        // 待機中のワーカーを起床（条件変数待機中の場合）
                        if (worker->global->global_chunk_queue) {
                            pthread_cond_broadcast(&worker->global->global_chunk_queue->cond);
                        }
                    }
                }

                // Atomic increment of tasks_completed ONLY when proven
                atomic_add_int(&worker->global->tasks_completed, 1);
            } else {
                // FIX: Root task not yet proven - re-enqueue for further exploration
                // This ensures the task continues until WIN/LOSE/DRAW is determined
                if (!worker->global->found_win && !worker->global->shutdown) {
                    Task retry_task = *task;
                    // Lower priority slightly to avoid starvation of other tasks
                    retry_task.priority = task->priority - 100;

                    // Push back to LocalHeap for re-processing (no lock needed)
                    // TTに途中結果が保存されているので、再処理時にTTヒットで効率的
                    local_heap_push(&worker->local_heap, &retry_task);

                    if (DEBUG_CONFIG.verbose) {
                        debug_log("Worker %d: root task %c%d not proven (pn=%u, dn=%u), re-enqueued to LocalHeap\n",
                               worker->id,
                               'a' + (task->root_move % 8), 8 - (task->root_move / 8),
                               root->pn, root->dn);
                    }
                }
            }
        }
    } else {
        // Subtask completed (atomic increment)
        __sync_fetch_and_add(&worker->global->subtasks_completed, 1);

        // サブタスクの結果伝播ロジック:
        //
        // 終端ノードのpn/dn設定が「ルートムーブを打ったプレイヤー視点」で統一されたため、
        // サブタスクのresultも同じ視点で設定されている:
        // - result == RESULT_EXACT_WIN: ルートムーブを打ったプレイヤーの勝ち
        // - result == RESULT_EXACT_LOSE: ルートムーブを打ったプレイヤーの負け
        //
        // サブタスクがWINを返した場合、それはルートムーブ全体がWINであることを意味する。
        // （サブタスクは部分木の探索であり、その部分木でWINが証明されれば、
        //   その経路を通ることでルートムーブ全体がWINになる）

        if (result == RESULT_EXACT_WIN || result == RESULT_EXACT_LOSE) {
            int move_idx = -1;
            for (int i = 0; i < worker->global->n_moves; i++) {
                if (worker->global->move_list[i] == task->root_move) {
                    move_idx = i;
                    break;
                }
            }

            if (move_idx >= 0) {
                if (result == RESULT_EXACT_WIN) {
                    Result expected = RESULT_UNKNOWN;
                    if (__sync_bool_compare_and_swap(&worker->global->move_results[move_idx],
                                                      expected, RESULT_EXACT_WIN)) {
                        // 早期終了: WINが見つかった
                        if (atomic_cas_bool(&worker->global->found_win, false, true)) {
                            __atomic_store_n(&worker->global->winning_move, task->root_move, __ATOMIC_RELEASE);
                            debug_log("Worker %d: subtask (gen=%d) found WIN for root move %c%d!\n",
                                   worker->id, task->generation,
                                   'a' + (task->root_move % 8), 8 - (task->root_move / 8));

                            // 待機中のワーカーを起床（条件変数待機中の場合）
                            if (worker->global->global_chunk_queue) {
                                pthread_cond_broadcast(&worker->global->global_chunk_queue->cond);
                            }
                        }
                    }
                }
                // LOSEの伝播はサブタスクからは行わない
                // ルートムーブがLOSEになるには、ルートタスク自体が完全に探索されて
                // 全ての経路で負けになる必要がある
                // これはルートタスク完了時の結果判定で処理される
            }
        }
    }

    // Free children arrays, then reset memory pool for reuse
    free_dfpn_tree_children(root);
    node_pool_reset(&worker->node_pool);

    if (DEBUG_CONFIG.track_work_stealing) {
        debug_log("Worker %d completed task: move=%c%d, result=%s, nodes=%llu\n",
               worker->id,
               'a' + (task->root_move % 8), 8 - (task->root_move / 8),
               result == RESULT_EXACT_WIN ? "WIN" :
               (result == RESULT_EXACT_LOSE ? "LOSE" : "UNKNOWN"),
               (unsigned long long)worker->nodes);
    }

    return true;  // タスク完了
}

// Worker thread function with HYBRID work stealing (LocalHeap + GlobalChunk)
static void* worker_thread(void *arg) {
    Worker *worker = (Worker*)arg;
    worker->nodes = 0;
    worker->tasks_processed = 0;
    worker->tasks_stolen = 0;

    if (DEBUG_CONFIG.track_threads && worker->stats) {
        worker->stats->thread_id = worker->id;
        time(&worker->stats->start_time);
        worker->stats->is_active = true;
    }

    // Mark worker as active
    int new_active = __sync_fetch_and_add(&worker->global->worker_state.active_workers, 1) + 1;

    // 全ワーカーが起動したらログ出力（一度だけ）
    if (new_active == worker->global->worker_state.total_workers) {
        debug_log("Worker %d: All %d workers started\n", worker->id, new_active);
    }

    debug_log("Worker %d started (HYBRID LocalHeap+GlobalChunk mode)\n", worker->id);

    while (!worker->global->shutdown && !worker->global->found_win) {
        Task task;

        // HYBRID: Use hybrid task acquisition
        if (get_next_task_hybrid(worker, &task)) {
            // busy_workers追跡: タスク取得成功時にbusy状態に（ビットマップ方式）
            if (!worker->is_busy) {
                worker->is_busy = true;
                worker_set_busy(&worker->global->worker_state, worker->id);
            }

            worker->tasks_stolen++;

            if (DEBUG_CONFIG.track_work_stealing) {
                // Lock-free atomic increment for debug stats
                __sync_fetch_and_add(&worker->global->ws_stats.tasks_stolen, 1);
            }

            // Reset node count for this task
            uint64_t nodes_before = worker->nodes;
            worker->nodes = 0;

            bool task_completed = process_task(worker, &task);

            // タスクが中断された場合（Globalの方が優先度が高い）
            // 中断されたタスクをLocalHeapに戻し、Globalからインポート
            if (!task_completed && !worker->global->shutdown && !worker->global->found_win) {
                // 中断されたタスクをLocalHeapに戻す（後で再処理）
                local_heap_push(&worker->local_heap, &task);

                // Globalからチャンクをインポート（最初のタスクをnew_taskに取得）
                Task new_task;
                if (import_chunk_from_global(worker, &new_task)) {
                    // インポートしたタスクもLocalHeapに追加
                    // （次のループで優先度に従って取得される）
                    local_heap_push(&worker->local_heap, &new_task);

                    if (DEBUG_CONFIG.track_work_stealing) {
                        debug_log("Worker %d: switched to Global task (imported chunk, new_priority=%d)\n",
                               worker->id, new_task.priority);
                    }
                }
            }

            // Accumulate total nodes
            worker->nodes += nodes_before;
        } else {
            // No task available - busy_workers追跡: タスク取得失敗時にidle状態に（ビットマップ方式）
            if (worker->is_busy) {
                worker->is_busy = false;
                worker_set_idle(&worker->global->worker_state, worker->id);
            }

            // check if we should exit
            if (worker->global->shutdown || worker->global->found_win) {
                break;
            }

            // ────────────────────────────────────────────────────────────
            // [最適化] usleepポーリングを条件変数に置き換え
            // ────────────────────────────────────────────────────────────
            // 【元のコード】
            //   if (local_size == 0 && global_size == 0) {
            //       usleep(1000);  // 1msポーリング
            //   }
            //
            // 【問題点】
            //   40コアでタスクが枯渇気味の時、多数のスレッドが同時に
            //   スリープ→起床を繰り返し、CPU時間を無駄に消費
            //
            // 【改善】
            //   条件変数でGlobalChunkQueueにタスクが追加されるまで
            //   ブロッキング待機。タイムアウト付きでデッドロック防止
            // ────────────────────────────────────────────────────────────
            int local_size = worker->local_heap.size;
            GlobalChunkQueue *gq = worker->global->global_chunk_queue;

            if (local_size == 0 && gq && gq->size == 0) {
                // 条件変数でタスク追加を待機（タイムアウト付き）
                struct timespec timeout;
                clock_gettime(CLOCK_REALTIME, &timeout);
                timeout.tv_nsec += 5000000;  // 5ms timeout
                if (timeout.tv_nsec >= 1000000000) {
                    timeout.tv_sec++;
                    timeout.tv_nsec -= 1000000000;
                }

                pthread_mutex_lock(&gq->mutex);
                // 再度チェック（ロック取得中にタスクが追加された可能性）
                if (gq->size == 0 && !worker->global->shutdown && !worker->global->found_win) {
                    pthread_cond_timedwait(&gq->cond, &gq->mutex, &timeout);
                }
                pthread_mutex_unlock(&gq->mutex);
            }
        }
    }

    // HYBRID: Share remaining tasks before exiting
    share_remaining_tasks(worker);

    // Mark worker as inactive
    __sync_fetch_and_sub(&worker->global->worker_state.active_workers, 1);

    if (worker->stats) {
        worker->stats->is_active = false;
        worker->stats->nodes_explored = worker->nodes;
        worker->stats->tasks_processed = worker->tasks_processed;
        worker->stats->tasks_stolen = worker->tasks_stolen;
    }

    debug_log("Worker %d finished: %llu nodes, %llu tasks processed, LocalHeap exports=%llu imports=%llu\n",
           worker->id,
           (unsigned long long)worker->nodes,
           (unsigned long long)worker->tasks_processed,
           (unsigned long long)worker->local_heap.exported_to_global,
           (unsigned long long)worker->local_heap.imported_from_global);

    return NULL;
}

// Real-time monitoring thread
static void* monitor_thread(void *arg) {
    GlobalState *global = (GlobalState*)arg;

    while (!global->shutdown && !global->found_win) {
        sleep(2);

        if (!DEBUG_CONFIG.real_time_monitor) continue;

        debug_log("\n--- Real-time Status (HYBRID) ---\n");
        debug_log("GlobalChunkQueue: %d chunks\n",
               global->global_chunk_queue ? global->global_chunk_queue->size : 0);
        debug_log("Chunks pushed: %llu, popped: %llu\n",
               (unsigned long long)(global->global_chunk_queue ? global->global_chunk_queue->chunks_pushed : 0),
               (unsigned long long)(global->global_chunk_queue ? global->global_chunk_queue->chunks_popped : 0));

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - global->start_time.tv_sec) +
                        (now.tv_nsec - global->start_time.tv_nsec) / 1e9;

        debug_log("Elapsed: %.1fs\n", elapsed);
        debug_log("TT: %llu hits, %llu stores, %llu collisions\n",
               (unsigned long long)global->tt->hits,
               (unsigned long long)global->tt->stores,
               (unsigned long long)global->tt->collisions);

        if (global->found_win) {
            debug_log("*** WIN FOUND - early termination ***\n");
        }
    }

    return NULL;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Main Solver with HYBRID Work Stealing
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Result solve_endgame(uint64_t player, uint64_t opponent, int num_threads,
                    double time_limit, int *best_move, bool use_evaluation) {
    // Initialize CPU feature detection for SIMD acceleration
    check_cpu_features();

    // ────────────────────────────────────────────────────────────
    // [最適化] Zobristハッシュテーブルの初期化（一度だけ）
    // ────────────────────────────────────────────────────────────
    // hash_position()から移動。ワーカースレッド起動前に呼ぶことで、
    // 探索中の毎回の関数呼び出しオーバーヘッドを排除。
    // 詳細は hash_position() 内のコメントを参照。
    // ────────────────────────────────────────────────────────────
    init_zobrist();

    debug_log("\n=== Othello Endgame Solver (HYBRID LocalHeap+GlobalChunk Version) ===\n");
    debug_log("Threads: %d (fixed), Time limit: %.1fs\n", num_threads, time_limit);
    debug_log("Evaluation function: %s\n", use_evaluation ? "ENABLED" : "DISABLED");
    debug_log("SIMD acceleration: Move generation=Scalar, Board symmetry=%s\n",
              cpu_has_avx2 ? "AVX2" : "Scalar");

    int empties = popcount(~(player | opponent));
    debug_log("Empties: %d\n", empties);

    // Initialize global state
    GlobalState global = {0};
    global.tt = tt_create(TT_SIZE_MB);
    global.time_limit = time_limit;
    global.use_evaluation = use_evaluation;
    global.found_win = false;
    global.shutdown = false;
    clock_gettime(CLOCK_MONOTONIC, &global.start_time);
    pthread_mutex_init(&global.stats_mutex, NULL);

    // HYBRID: Initialize GlobalChunkQueue and SharedTaskArray
    global.global_chunk_queue = global_chunk_queue_create();
    global.shared_array = shared_array_create();

    // HYBRID: Initialize WorkerState（ビットマップ方式）
    worker_state_init(&global.worker_state, num_threads);

    // HYBRID: Initialize statistics
    global.total_exports = 0;
    global.total_imports = 0;
    global.global_switches = 0;

    debug_log("HYBRID settings: LocalHeap=%d, ChunkSize=%d, ExportThreshold=%d\n",
              LOCAL_HEAP_CAPACITY, CHUNK_SIZE, LOCAL_EXPORT_THRESHOLD);
    debug_log("HYBRID: GlobalChunkQueue=%d chunks, SharedArray=%d tasks\n",
              GLOBAL_QUEUE_CAPACITY, SHARED_ARRAY_SIZE);

    // Dynamic task spawning settings (use global config variables)
    // For 40-core: use -G 5 -D 4 -S 6 command line options
    global.max_generation = SPAWN_MAX_GENERATION;
    global.min_depth_for_spawn = SPAWN_MIN_DEPTH;
    global.spawn_threshold = -1000;     // Spawn children with priority > -1000
    global.spawn_limit = SPAWN_LIMIT_PER_NODE;
    global.subtasks_spawned = 0;
    global.subtasks_completed = 0;

    debug_log("Spawn settings: max_gen=%d, min_depth=%d, limit=%d\n",
              global.max_generation, global.min_depth_for_spawn, global.spawn_limit);

    uint64_t moves = get_moves(player, opponent);
    if (moves == 0) {
        // HYBRID: Cleanup hybrid resources
        global_chunk_queue_destroy(global.global_chunk_queue);
        shared_array_destroy(global.shared_array);
        tt_free(global.tt);
        pthread_mutex_destroy(&global.stats_mutex);
        return RESULT_UNKNOWN;
    }

    int n_moves = popcount(moves);
    debug_log("Legal moves: %d\n\n", n_moves);

    // Allocate result arrays
    global.n_moves = n_moves;
    global.move_results = calloc(n_moves, sizeof(Result));
    global.move_nodes = calloc(n_moves, sizeof(uint64_t));
    global.move_list = calloc(n_moves, sizeof(int));
    global.move_evals = calloc(n_moves, sizeof(int));

#if ENABLE_EVAL_IMPACT
    // EvalImpact tracking (-e option)
    // 評価関数影響分析用のメモリ確保
    if (DEBUG_CONFIG.track_eval_impact) {
        global.eval_impacts = calloc(n_moves, sizeof(EvalImpact));
        global.move_start_times = calloc(n_moves, sizeof(struct timespec));
    }
#endif

    // Create tasks for each root move
    // 起動フェーズ: SharedTaskArrayに直接投入（ソート不要、同期コスト最小）
    uint64_t moves_copy = moves;
    int idx = 0;

#if ENABLE_EVAL_IMPACT
    // 評価スコアでソートして優先順位を決定（EvalImpact用）
    // 評価関数による手の順序を記録するため、事前にソート
    typedef struct {
        int move;
        int eval;
        int original_idx;
    } MoveEval;
    MoveEval *sorted_moves = malloc(n_moves * sizeof(MoveEval));
    uint64_t moves_temp = moves;
    for (int i = 0; i < n_moves; i++) {
        int move = first_one(moves_temp);
        moves_temp &= moves_temp - 1;
        uint64_t p = player;
        uint64_t o = opponent;
        make_move(&p, &o, move);
        sorted_moves[i].move = move;
        sorted_moves[i].eval = use_evaluation ? -evaluate_position(p, o) : 0;
        sorted_moves[i].original_idx = i;
    }
    // ソート（降順：評価が高い順）
    for (int i = 0; i < n_moves - 1; i++) {
        for (int j = i + 1; j < n_moves; j++) {
            if (sorted_moves[j].eval > sorted_moves[i].eval) {
                MoveEval tmp = sorted_moves[i];
                sorted_moves[i] = sorted_moves[j];
                sorted_moves[j] = tmp;
            }
        }
    }
#endif // ENABLE_EVAL_IMPACT

    debug_log("Initial task distribution to SharedTaskArray:\n");
    while(moves_copy) {
        int move = first_one(moves_copy);
        moves_copy &= moves_copy - 1;

        uint64_t p = player;
        uint64_t o = opponent;
        make_move(&p, &o, move);

        int eval = use_evaluation ? -evaluate_position(p, o) : 0;

        global.move_list[idx] = move;
        global.move_evals[idx] = eval;

#if ENABLE_EVAL_IMPACT
        // EvalImpact初期化
        // 各手の評価関数情報と探索開始時刻を記録
        if (DEBUG_CONFIG.track_eval_impact && global.eval_impacts) {
            global.eval_impacts[idx].move = move;
            global.eval_impacts[idx].eval_score = eval;
            // 評価関数による順序を検索
            for (int i = 0; i < n_moves; i++) {
                if (sorted_moves[i].move == move) {
                    global.eval_impacts[idx].original_order = i;
                    break;
                }
            }
            global.eval_impacts[idx].result = RESULT_UNKNOWN;
            global.eval_impacts[idx].was_cutoff = false;
            clock_gettime(CLOCK_MONOTONIC, &global.move_start_times[idx]);
        }
#endif // ENABLE_EVAL_IMPACT

        Task task = {
            .player = p,
            .opponent = o,
            .root_move = move,
            .priority = eval,  // 起動フェーズでは優先度は使わない
            .eval_score = eval,
            .is_root_task = true,
            .depth = empties - 1,   // After making move, one less empty
            .node_type = NODE_AND,  // Opponent's turn after our move
            .generation = 0         // Root level
        };

        // SharedTaskArrayに投入（ロックフリー、高速）
        shared_array_push(global.shared_array, &task);
        debug_log("  %c%d: eval=%d -> SharedTaskArray\n",
               'a' + (move % 8), 8 - (move / 8), eval);
        idx++;
    }
#if ENABLE_EVAL_IMPACT
    free(sorted_moves);
#endif
    debug_log("\n");

    // Allocate debug structures
    ThreadStats *thread_stats = NULL;
    TreeStats *tree_stats = NULL;

    if (DEBUG_CONFIG.track_threads) {
        thread_stats = calloc(num_threads, sizeof(ThreadStats));
    }
    if (DEBUG_CONFIG.track_tree_stats) {
        tree_stats = calloc(num_threads, sizeof(TreeStats));
    }

    // Create fixed number of worker threads
    Worker *workers = calloc(num_threads, sizeof(Worker));

    for (int i = 0; i < num_threads; i++) {
        workers[i].id = i;
        workers[i].global = &global;
        workers[i].is_busy = false;  // busy_workers追跡用
        workers[i].has_entered_chunk_mode = false;  // check_and_export最適化用
        workers[i].nodes_at_last_export_check = 0;
        if (thread_stats) workers[i].stats = &thread_stats[i];
        if (tree_stats) workers[i].tree_stats = &tree_stats[i];
        node_pool_init(&workers[i].node_pool);
        // HYBRID: Initialize LocalHeap for each worker
        local_heap_init(&workers[i].local_heap);

#if ENABLE_GLOBAL_CHECK_BENCHMARK
        // Global比較統計の初期化
        workers[i].global_check_count = 0;
        workers[i].global_check_true_count = 0;
        workers[i].cumulative_nodes = 0;
        workers[i].nodes_at_last_check = 0;
        workers[i].check_interval_sum = 0;
        workers[i].check_interval_min = 0;
        workers[i].check_interval_max = 0;
#endif
    }

    // Start monitoring thread
    pthread_t monitor;
    if (DEBUG_CONFIG.real_time_monitor) {
        pthread_create(&monitor, NULL, monitor_thread, &global);
    }

    // Launch worker threads
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&workers[i].thread, NULL, worker_thread, &workers[i]);
    }

    // Wait for all tasks to complete or early termination
    while (!global.shutdown && !global.found_win) {
        // Check if all tasks are completed
        if (global.tasks_completed >= n_moves) {
            debug_log("All %d tasks completed.\n", n_moves);
            global.shutdown = true;
            break;
        }

        // Check time limit
        if (time_limit > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - global.start_time.tv_sec) +
                            (now.tv_nsec - global.start_time.tv_nsec) / 1e9;
            if (elapsed >= time_limit) {
                debug_log("Time limit reached (%.1fs). Completed %d/%d tasks.\n",
                       elapsed, global.tasks_completed, n_moves);
                global.shutdown = true;
                break;
            }
        }

        usleep(50000);  // Check every 50ms
    }

    // Signal shutdown
    global.shutdown = true;

    // 待機中のワーカーを起床させる（条件変数待機中の場合）
    if (global.global_chunk_queue) {
        pthread_cond_broadcast(&global.global_chunk_queue->cond);
    }

    // Wait for workers to finish
    for (int i = 0; i < num_threads; i++) {
        pthread_join(workers[i].thread, NULL);
    }

    // Cleanup worker memory pools and LocalHeaps
    for (int i = 0; i < num_threads; i++) {
        node_pool_destroy(&workers[i].node_pool);
        // HYBRID: Destroy LocalHeap
        local_heap_destroy(&workers[i].local_heap);
    }

    if (DEBUG_CONFIG.real_time_monitor) {
        pthread_join(monitor, NULL);
    }

    // Aggregate results
    Result final_result = RESULT_UNKNOWN;
    int final_best_move = -1;
    int best_eval = -1000000;
    int win_count = 0;
    int lose_count = 0;
    int draw_count = 0;
    int unknown_count = 0;

    if (global.found_win) {
        final_result = RESULT_EXACT_WIN;
        final_best_move = global.winning_move;
    } else {
        // Count results
        for (int i = 0; i < n_moves; i++) {
            switch (global.move_results[i]) {
                case RESULT_EXACT_WIN:  win_count++; break;
                case RESULT_EXACT_LOSE: lose_count++; break;
                case RESULT_EXACT_DRAW: draw_count++; break;
                default: unknown_count++; break;
            }
        }

        // Determine final result
        // WIN: If any move leads to WIN
        // DRAW: If no WIN but some move leads to DRAW
        // LOSE: Only if ALL moves are proven LOSE
        // UNKNOWN: Otherwise (incomplete search)
        for (int i = 0; i < n_moves; i++) {
            if (global.move_results[i] == RESULT_EXACT_WIN) {
                final_result = RESULT_EXACT_WIN;
                final_best_move = global.move_list[i];
                break;
            }
            if (global.move_results[i] == RESULT_EXACT_DRAW && final_result != RESULT_EXACT_WIN) {
                final_result = RESULT_EXACT_DRAW;
                if (final_best_move == -1) {
                    final_best_move = global.move_list[i];
                }
            }
            if (global.move_evals[i] > best_eval) {
                best_eval = global.move_evals[i];
                if (final_result == RESULT_UNKNOWN) {
                    final_best_move = global.move_list[i];
                }
            }
        }

        // Only LOSE if all moves are proven LOSE (no UNKNOWN, no DRAW, no WIN)
        if (final_result == RESULT_UNKNOWN && lose_count == n_moves) {
            final_result = RESULT_EXACT_LOSE;
        }
    }

    if (final_best_move == -1 && n_moves > 0) {
        final_best_move = global.move_list[0];
    }

    // Print statistics
    debug_log("\n\n=== Final Statistics ===\n");
    uint64_t total_nodes = 0;

    for (int i = 0; i < n_moves; i++) {
        int move = global.move_list[i];
        char move_str[8];
        snprintf(move_str, sizeof(move_str), "%c%d",
                'a' + (move % 8), 8 - (move / 8));
        const char *res_str = global.move_results[i] == RESULT_EXACT_WIN ? "WIN" :
                             (global.move_results[i] == RESULT_EXACT_LOSE ? "LOSE" : "UNKNOWN");
        debug_log("Move %s -> %s (%llu nodes, eval=%d)\n",
               move_str, res_str,
               (unsigned long long)global.move_nodes[i],
               global.move_evals[i]);
        total_nodes += global.move_nodes[i];
    }

    // Add worker statistics
    debug_log("\n=== Worker Statistics ===\n");
    for (int i = 0; i < num_threads; i++) {
        debug_log("Worker %d: %llu nodes, %llu tasks\n",
               i,
               (unsigned long long)workers[i].nodes,
               (unsigned long long)workers[i].tasks_processed);
        total_nodes += workers[i].nodes;
    }

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - global.start_time.tv_sec) +
                     (end_time.tv_nsec - global.start_time.tv_nsec) / 1e9;

    debug_log("\nTotal: %llu nodes in %.3f seconds (%.0f NPS)\n",
           (unsigned long long)total_nodes, elapsed,
           total_nodes > 0 && elapsed > 0 ? total_nodes / elapsed : 0);
    debug_log("TT: %llu hits, %llu stores, %llu collisions (%.1f%% hit rate)\n",
           (unsigned long long)global.tt->hits,
           (unsigned long long)global.tt->stores,
           (unsigned long long)global.tt->collisions,
           100.0 * global.tt->hits / (global.tt->hits + global.tt->stores + 1));

    debug_log("\n=== Work Stealing Statistics ===\n");
    debug_log("Root tasks: %d, completed: %d\n", n_moves, global.tasks_completed);
    debug_log("Subtasks spawned: %llu, completed: %llu\n",
           (unsigned long long)global.subtasks_spawned,
           (unsigned long long)global.subtasks_completed);
    debug_log("Early termination: %s\n", global.found_win ? "YES (WIN found)" : "NO");

    // HYBRID: Print hybrid statistics
    debug_log("\n=== HYBRID Statistics ===\n");
    uint64_t total_local_pushes = 0, total_local_pops = 0;
    uint64_t total_exported = 0, total_imported = 0;
    for (int i = 0; i < num_threads; i++) {
        total_local_pushes += workers[i].local_heap.local_pushes;
        total_local_pops += workers[i].local_heap.local_pops;
        total_exported += workers[i].local_heap.exported_to_global;
        total_imported += workers[i].local_heap.imported_from_global;
    }
    debug_log("LocalHeap: %llu pushes, %llu pops\n",
           (unsigned long long)total_local_pushes,
           (unsigned long long)total_local_pops);
    debug_log("GlobalChunkQueue: %llu chunks pushed, %llu chunks popped\n",
           (unsigned long long)(global.global_chunk_queue ? global.global_chunk_queue->chunks_pushed : 0),
           (unsigned long long)(global.global_chunk_queue ? global.global_chunk_queue->chunks_popped : 0));
    debug_log("Export/Import: %llu exported, %llu imported\n",
           (unsigned long long)total_exported,
           (unsigned long long)total_imported);
    debug_log("Global switches (TT-hit triggered): %llu\n",
           (unsigned long long)global.global_switches);

#if ENABLE_EVAL_IMPACT
    // EvalImpact統計出力 (-e option)
    if (DEBUG_CONFIG.track_eval_impact && global.eval_impacts) {
        debug_log("\n=== Evaluation Impact Analysis ===\n");
        debug_log("評価関数が探索にどのように影響したかの分析:\n\n");
        debug_log("Move | EvalScore | EvalOrder | Result  |     Nodes |    Time |       NPS | Cutoff\n");
        debug_log("-----|-----------|-----------|---------|-----------|---------|-----------|-------\n");

        // 結果が確定した順序を決定
        int final_order = 0;
        for (int i = 0; i < n_moves; i++) {
            if (global.eval_impacts[i].result != RESULT_UNKNOWN) {
                global.eval_impacts[i].final_order = final_order++;
            } else {
                global.eval_impacts[i].final_order = -1;  // 未確定
            }
        }

        // 早期終了でカットされた手を判定
        if (global.found_win) {
            for (int i = 0; i < n_moves; i++) {
                if (global.eval_impacts[i].result == RESULT_UNKNOWN) {
                    global.eval_impacts[i].was_cutoff = true;
                }
            }
        }

        for (int i = 0; i < n_moves; i++) {
            int move = global.eval_impacts[i].move;
            char move_str[4];
            snprintf(move_str, sizeof(move_str), "%c%d",
                    'a' + (move % 8), 8 - (move / 8));

            const char *result_str =
                global.eval_impacts[i].result == RESULT_EXACT_WIN ? "WIN" :
                (global.eval_impacts[i].result == RESULT_EXACT_LOSE ? "LOSE" :
                (global.eval_impacts[i].result == RESULT_EXACT_DRAW ? "DRAW" : "UNKNOWN"));

            debug_log("  %s | %9d | %9d | %7s | %9llu | %6.3fs | %9.0f | %s\n",
                   move_str,
                   global.eval_impacts[i].eval_score,
                   global.eval_impacts[i].original_order,
                   result_str,
                   (unsigned long long)global.eval_impacts[i].nodes_searched,
                   global.eval_impacts[i].time_spent,
                   global.eval_impacts[i].nps,
                   global.eval_impacts[i].was_cutoff ? "YES" : "NO");
        }

        // 統計サマリー
        debug_log("\n--- Summary ---\n");
        int win_by_top_eval = 0;
        int total_proven = 0;
        double total_time_proven = 0;
        uint64_t total_nodes_proven = 0;

        for (int i = 0; i < n_moves; i++) {
            if (global.eval_impacts[i].result == RESULT_EXACT_WIN) {
                if (global.eval_impacts[i].original_order == 0) {
                    win_by_top_eval = 1;
                }
                total_proven++;
                total_time_proven += global.eval_impacts[i].time_spent;
                total_nodes_proven += global.eval_impacts[i].nodes_searched;
            } else if (global.eval_impacts[i].result == RESULT_EXACT_LOSE ||
                       global.eval_impacts[i].result == RESULT_EXACT_DRAW) {
                total_proven++;
                total_time_proven += global.eval_impacts[i].time_spent;
                total_nodes_proven += global.eval_impacts[i].nodes_searched;
            }
        }

        debug_log("証明済み手数: %d / %d\n", total_proven, n_moves);
        if (global.found_win) {
            debug_log("勝利手は評価関数で最高評価だったか: %s\n",
                   win_by_top_eval ? "YES（評価関数が正しく予測）" : "NO（評価関数が外れた）");
        }
        debug_log("証明に要した総ノード数: %llu\n", (unsigned long long)total_nodes_proven);
        debug_log("証明に要した総時間: %.3fs\n", total_time_proven);
    }
#endif // ENABLE_EVAL_IMPACT

#if ENABLE_GLOBAL_CHECK_BENCHMARK
    // Global比較ベンチマーク統計（スレッドごと）
    debug_log("\n=== Global Check Benchmark (per-thread) ===\n");
    debug_log("Thread | Checks | GlobalBetter | AvgInterval | MinInterval | MaxInterval\n");
    debug_log("-------|--------|--------------|-------------|-------------|------------\n");

    uint64_t total_checks = 0;
    uint64_t total_true = 0;

    for (int i = 0; i < num_threads; i++) {
        uint64_t checks = workers[i].global_check_count;
        uint64_t true_count = workers[i].global_check_true_count;
        uint64_t interval_sum = workers[i].check_interval_sum;
        uint64_t interval_min = workers[i].check_interval_min;
        uint64_t interval_max = workers[i].check_interval_max;

        double avg_interval = (checks > 1) ? (double)interval_sum / (checks - 1) : 0;

        debug_log("   %2d  | %6llu | %12llu | %11.1f | %11llu | %11llu\n",
               i,
               (unsigned long long)checks,
               (unsigned long long)true_count,
               avg_interval,
               (unsigned long long)interval_min,
               (unsigned long long)interval_max);

        total_checks += checks;
        total_true += true_count;
    }

    debug_log("-------|--------|--------------|-------------|-------------|------------\n");
    debug_log(" Total | %6llu | %12llu |             |             |\n",
           (unsigned long long)total_checks,
           (unsigned long long)total_true);

    if (total_checks > 0) {
        debug_log("\nGlobal比較でGlobalが良かった割合: %.2f%%\n",
               100.0 * total_true / total_checks);
        debug_log("平均: %.1f ノードごとに1回Global比較\n",
               (double)total_nodes / total_checks);
    }
#endif

    debug_log("\n=== Result Summary ===\n");
    debug_log("WIN: %d, LOSE: %d, DRAW: %d, UNKNOWN: %d\n",
           win_count, lose_count, draw_count, unknown_count);
    debug_log("Final result: %s\n",
           final_result == RESULT_EXACT_WIN ? "WIN" :
           (final_result == RESULT_EXACT_LOSE ? "LOSE" :
           (final_result == RESULT_EXACT_DRAW ? "DRAW" : "UNKNOWN")));

    if (best_move) *best_move = final_best_move;

    // Populate benchmark result for CSV/JSON output
    g_benchmark_result.empties = empties;
    g_benchmark_result.legal_moves = n_moves;
    strncpy(g_benchmark_result.result,
            final_result == RESULT_EXACT_WIN ? "WIN" :
            (final_result == RESULT_EXACT_LOSE ? "LOSE" :
            (final_result == RESULT_EXACT_DRAW ? "DRAW" : "UNKNOWN")),
            sizeof(g_benchmark_result.result) - 1);
    if (final_best_move >= 0 && final_best_move < 64) {
        snprintf(g_benchmark_result.best_move, sizeof(g_benchmark_result.best_move),
                 "%c%d", 'a' + (final_best_move % 8), 8 - (final_best_move / 8));
    } else {
        strncpy(g_benchmark_result.best_move, "N/A", sizeof(g_benchmark_result.best_move) - 1);
    }
    g_benchmark_result.total_nodes = total_nodes;
    g_benchmark_result.time_sec = elapsed;
    g_benchmark_result.nps = (total_nodes > 0 && elapsed > 0) ? total_nodes / elapsed : 0;
    g_benchmark_result.tt_hits = global.tt->hits;
    g_benchmark_result.tt_stores = global.tt->stores;
    g_benchmark_result.tt_collisions = global.tt->collisions;
    g_benchmark_result.tt_hit_rate = 100.0 * global.tt->hits / (global.tt->hits + global.tt->stores + 1);
    g_benchmark_result.subtasks_spawned = global.subtasks_spawned;
    g_benchmark_result.subtasks_completed = global.subtasks_completed;
    g_benchmark_result.win_count = win_count;
    g_benchmark_result.lose_count = lose_count;
    g_benchmark_result.draw_count = draw_count;
    g_benchmark_result.unknown_count = unknown_count;

    // Store per-worker statistics
    for (int i = 0; i < num_threads && i < MAX_THREADS; i++) {
        g_benchmark_result.worker_nodes[i] = workers[i].nodes;
        g_benchmark_result.worker_tasks[i] = workers[i].tasks_processed;
    }

    // Output to CSV and/or JSON
    output_csv_result(&g_benchmark_result);
    output_json_result(&g_benchmark_result);

    // Cleanup
    if (thread_stats) free(thread_stats);
    if (tree_stats) free(tree_stats);
#if ENABLE_EVAL_IMPACT
    if (global.eval_impacts) free(global.eval_impacts);
    if (global.move_start_times) free(global.move_start_times);
#endif
    free(global.move_results);
    free(global.move_nodes);
    free(global.move_list);
    free(global.move_evals);
    free(workers);
    tt_free(global.tt);
    pthread_mutex_destroy(&global.stats_mutex);

    // HYBRID: Cleanup hybrid resources
    global_chunk_queue_destroy(global.global_chunk_queue);
    shared_array_destroy(global.shared_array);

    return final_result;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// File Parsing and Main
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static bool parse_pos_file(const char *filename, uint64_t *black, uint64_t *white, char *turn) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("Error opening file");
        return false;
    }

    char board_str[128];
    char turn_str[128];

    if (fgets(board_str, sizeof(board_str), f) == NULL) {
        fprintf(stderr, "Error: Cannot read board string from file.\n");
        fclose(f);
        return false;
    }

    if (fgets(turn_str, sizeof(turn_str), f) == NULL) {
        fprintf(stderr, "Error: Cannot read turn string from file.\n");
        fclose(f);
        return false;
    }
    fclose(f);

    *black = 0;
    *white = 0;
    for (int i = 0; i < 64; i++) {
        if (board_str[i] == 'X' || board_str[i] == 'x' || board_str[i] == '*') {
            *black |= (1ULL << i);
        } else if (board_str[i] == 'O' || board_str[i] == 'o') {
            *white |= (1ULL << i);
        }
    }

    if (turn_str[0] == 'B' || turn_str[0] == 'b') {
        *turn = 'B';
    } else if (turn_str[0] == 'W' || turn_str[0] == 'w') {
        *turn = 'W';
    } else {
        fprintf(stderr, "Error: Invalid turn character '%c'. Should be 'B' or 'W'.\n", turn_str[0]);
        return false;
    }

    return true;
}

#ifdef STANDALONE_MAIN
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pos_file> [threads] [time_limit] [eval_dat] [options]\n", argv[0]);
        fprintf(stderr, "\nThis is the WORK STEALING version with dynamic task spawning.\n");
        fprintf(stderr, "Threads parameter now means fixed worker count (not per-move).\n");
        fprintf(stderr, "\nDebug options:\n");
        fprintf(stderr, "  -d <logfile>  Enable debug logging to file\n");
        fprintf(stderr, "  -v            Verbose output to console\n");
        fprintf(stderr, "  -t            Track thread activity\n");
        fprintf(stderr, "  -e            Track evaluation impact\n");
        fprintf(stderr, "  -s            Track search tree statistics\n");
        fprintf(stderr, "  -m            Real-time monitoring\n");
        fprintf(stderr, "  -w            Track work stealing events\n");
        fprintf(stderr, "\nOutput options (for benchmarking and analysis):\n");
        fprintf(stderr, "  -c <csvfile>  Output results to CSV file (append mode)\n");
        fprintf(stderr, "  -j <jsonfile> Output detailed results to JSON file\n");
        fprintf(stderr, "\nDynamic task spawning options (for tuning on many-core systems):\n");
        fprintf(stderr, "  -G <num>      Max generation depth (default: 3, 40-core: 5)\n");
        fprintf(stderr, "  -D <num>      Min depth for spawning (default: 6, 40-core: 4)\n");
        fprintf(stderr, "  -S <num>      Spawn limit per node (default: 3, 40-core: 6)\n");
        fprintf(stderr, "\nExamples:\n");
        fprintf(stderr, "  Basic:   %s test.pos 8 30.0 eval.dat -v -w\n", argv[0]);
        fprintf(stderr, "  40-core: %s test.pos 40 120.0 eval.dat -v -w -G 5 -D 4 -S 6\n", argv[0]);
        fprintf(stderr, "  Bench:   %s test.pos 8 30.0 eval.dat -c results.csv -j result.json\n", argv[0]);
        return 1;
    }

    char *filename = argv[1];
    int num_threads = (argc > 2) ? atoi(argv[2]) : 4;
    double time_limit = (argc > 3) ? atof(argv[3]) : 30.0;
    char *eval_path = (argc > 4) ? argv[4] : "eval/eval.dat";

    // Parse debug options
    bool debug_enabled = false;
    bool verbose = false;
    bool track_threads = false;
    bool track_eval = false;
    bool track_tree = false;
    bool real_time = false;
    bool track_ws = false;
    char *log_file = NULL;
    char *csv_file = NULL;
    char *json_file = NULL;

    // Dynamic task spawning options (40コア最適値をデフォルトに使用)
    int max_generation = DEFAULT_SPAWN_MAX_GENERATION;
    int min_depth_for_spawn = DEFAULT_SPAWN_MIN_DEPTH;
    int spawn_limit = DEFAULT_SPAWN_LIMIT_PER_NODE;

    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            debug_enabled = true;
            verbose = true;
        } else if (strcmp(argv[i], "-t") == 0) {
            debug_enabled = true;
            track_threads = true;
        } else if (strcmp(argv[i], "-e") == 0) {
            debug_enabled = true;
            track_eval = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            debug_enabled = true;
            track_tree = true;
        } else if (strcmp(argv[i], "-m") == 0) {
            debug_enabled = true;
            real_time = true;
        } else if (strcmp(argv[i], "-w") == 0) {
            debug_enabled = true;
            track_ws = true;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            debug_enabled = true;
            log_file = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            csv_file = argv[++i];
        } else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            json_file = argv[++i];
        } else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc) {
            max_generation = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            min_depth_for_spawn = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            spawn_limit = atoi(argv[++i]);
        }
    }

    // Report spawn settings if verbose
    if (verbose) {
        printf("Dynamic task spawning settings:\n");
        printf("  Max generation: %d\n", max_generation);
        printf("  Min depth for spawn: %d\n", min_depth_for_spawn);
        printf("  Spawn limit per node: %d\n\n", spawn_limit);
    }

    if (debug_enabled) {
        debug_init(log_file, verbose, track_threads, track_eval, track_tree, real_time, track_ws);
    }

    // Setup CSV/JSON output
    if (csv_file) {
        DEBUG_CONFIG.output_csv = true;
        strncpy(DEBUG_CONFIG.csv_filename, csv_file, sizeof(DEBUG_CONFIG.csv_filename) - 1);
        if (!DEBUG_CONFIG.enabled) {
            DEBUG_CONFIG.enabled = true;
            pthread_mutex_init(&DEBUG_CONFIG.log_mutex, NULL);
        }
    }
    if (json_file) {
        DEBUG_CONFIG.output_json = true;
        strncpy(DEBUG_CONFIG.json_filename, json_file, sizeof(DEBUG_CONFIG.json_filename) - 1);
        if (!DEBUG_CONFIG.enabled) {
            DEBUG_CONFIG.enabled = true;
            pthread_mutex_init(&DEBUG_CONFIG.log_mutex, NULL);
        }
    }

    // Store filename for benchmark result
    strncpy(g_benchmark_result.filename, filename, sizeof(g_benchmark_result.filename) - 1);
    g_benchmark_result.num_threads = num_threads;
    g_benchmark_result.spawn_max_gen = max_generation;
    g_benchmark_result.spawn_min_depth = min_depth_for_spawn;
    g_benchmark_result.spawn_limit = spawn_limit;

    // Set global spawn configuration from command-line arguments
    SPAWN_MAX_GENERATION = max_generation;
    SPAWN_MIN_DEPTH = min_depth_for_spawn;
    SPAWN_LIMIT_PER_NODE = spawn_limit;

    // Load evaluation weights
    bool use_evaluation = false;
    if (eval_path && strcmp(eval_path, "none") != 0 && access(eval_path, F_OK) == 0) {
        use_evaluation = load_evaluation_weights(eval_path);
    }

    uint64_t black, white;
    char turn_char;

    printf("Loading position from: %s\n", filename);
    if (!parse_pos_file(filename, &black, &white, &turn_char)) {
        return 1;
    }

    uint64_t player = (turn_char == 'B') ? black : white;
    uint64_t opponent = (turn_char == 'B') ? white : black;

    int best_move;
    Result result = solve_endgame(
        player,
        opponent,
        num_threads,
        time_limit,
        &best_move,
        use_evaluation
    );

    printf("\n--- FINAL RESULT ---\n");
    printf("Result: ");
    switch (result) {
        case RESULT_EXACT_WIN:  printf("WIN\n"); break;
        case RESULT_EXACT_LOSE: printf("LOSE\n"); break;
        case RESULT_EXACT_DRAW: printf("DRAW\n"); break;
        default: printf("UNKNOWN\n");
    }

    if (best_move >= 0 && best_move < 64) {
        printf("Best move: %c%d\n",
                'a' + (best_move % 8),
                8 - (best_move / 8));
    }
    printf("══════════════════\n\n");

    // Cleanup
    free_evaluation_weights();
    debug_close();

    return 0;
}
#endif // STANDALONE_MAIN
