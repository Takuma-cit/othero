/**
 * @file othello_endgame_solver_workstealing.c
 * @brief Othello Endgame Solver with Work Stealing Parallelization
 *
 * Work Stealing implementation to solve load imbalance:
 * - Fixed number of worker threads (independent of legal move count)
 * - Global task queue with thread-safe operations
 * - Workers steal tasks when their local work is done
 * - Early termination when WIN is found
 */

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

// --- トランスポジションテーブル関連 ---
#ifndef TT_SIZE_MB
#define TT_SIZE_MB 4096                 // TTサイズ（MB）
#endif

#ifndef TT_LOCK_STRIPES
#define TT_LOCK_STRIPES 1024            // TTストライプロック数（競合軽減用）
#endif

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64              // キャッシュラインサイズ（バイト）
#endif

// --- タスクキュー関連 ---
#ifndef MAX_TASK_QUEUE_SIZE
#define MAX_TASK_QUEUE_SIZE 65536       // タスクキューの最大サイズ
#endif

// --- メモリプール関連 ---
#ifndef NODE_POOL_BLOCK_SIZE
#define NODE_POOL_BLOCK_SIZE 8192       // ノードプールのブロックサイズ
#endif

// --- 証明数/反証数の上限 ---
#ifndef PN_INF
#define PN_INF 100000000                // 証明数の無限大値
#endif

#ifndef DN_INF
#define DN_INF 100000000                // 反証数の無限大値
#endif

// ────────────────────────────────────────────────────────────
// 【実行時変更可能パラメータ】
// アルゴリズム動作の調整用。ここではデフォルト値を定義。
// 実行時にコマンドラインオプションで上書き可能。
// ────────────────────────────────────────────────────────────

// --- タスクスポーン関連 ---
// 実行時に -G, -D, -S オプションで変更可能
#ifndef DEFAULT_SPAWN_MAX_GENERATION
#define DEFAULT_SPAWN_MAX_GENERATION 3  // タスクスポーンの最大世代（-G で変更）
#endif

#ifndef DEFAULT_SPAWN_MIN_DEPTH
#define DEFAULT_SPAWN_MIN_DEPTH 6       // スポーンする最小残り空きマス数（-D で変更）
#endif

#ifndef DEFAULT_SPAWN_LIMIT_PER_NODE
#define DEFAULT_SPAWN_LIMIT_PER_NODE 3  // ノードあたりの最大スポーン数（-S で変更）
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

// 実行時変更可能な変数（コマンドラインオプションで設定）
// デフォルト値は上部の DEFAULT_SPAWN_* で定義
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

// Evaluation impact tracking
typedef struct {
    int move;
    int eval_score;
    int original_order;
    int priority_order;
    uint64_t nodes_searched;
    double time_spent;
    bool proved_win;
    bool proved_loss;
} EvalImpact;

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

    va_list args;
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
        printf("%s ", timestamp);
        vprintf(format, args);
        fflush(stdout);
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
// Constants
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 注: 定数パラメータはファイル上部で定義済み

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

// Thread-safe Task Queue (Heap-based for O(log n) push/pop)
typedef struct {
    Task *heap;         // Binary max-heap array
    int size;
    int capacity;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    bool shutdown;

    // Statistics
    uint64_t total_pushed;
    uint64_t total_popped;
} TaskQueue;

static TaskQueue* taskqueue_create(int capacity) {
    TaskQueue *tq = calloc(1, sizeof(TaskQueue));
    tq->heap = calloc(capacity, sizeof(Task));
    tq->capacity = capacity;
    tq->size = 0;
    tq->shutdown = false;
    pthread_mutex_init(&tq->mutex, NULL);
    pthread_cond_init(&tq->not_empty, NULL);
    return tq;
}

static void taskqueue_free(TaskQueue *tq) {
    pthread_mutex_destroy(&tq->mutex);
    pthread_cond_destroy(&tq->not_empty);
    free(tq->heap);
    free(tq);
}

// Push task to queue using heap (thread-safe, O(log n))
static bool taskqueue_push(TaskQueue *tq, Task *task) {
    pthread_mutex_lock(&tq->mutex);

    if (tq->size >= tq->capacity) {
        pthread_mutex_unlock(&tq->mutex);
        return false;
    }

    // Insert at end and sift up (heap insertion)
    int i = tq->size;
    tq->size++;
    tq->total_pushed++;

    // Sift up: move the new element up until heap property is restored
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (task->priority <= tq->heap[parent].priority) {
            break;
        }
        tq->heap[i] = tq->heap[parent];
        i = parent;
    }
    tq->heap[i] = *task;

    pthread_cond_signal(&tq->not_empty);
    pthread_mutex_unlock(&tq->mutex);
    return true;
}

// Pop highest priority task from queue using heap (thread-safe, O(log n))
static bool taskqueue_pop(TaskQueue *tq, Task *task) {
    pthread_mutex_lock(&tq->mutex);

    if (tq->size == 0) {
        pthread_mutex_unlock(&tq->mutex);
        return false;
    }

    // Root has highest priority
    *task = tq->heap[0];
    tq->size--;
    tq->total_popped++;

    if (tq->size > 0) {
        // Move last element to root and sift down
        Task last = tq->heap[tq->size];
        int i = 0;

        while (i * 2 + 1 < tq->size) {
            int child = i * 2 + 1;
            // Choose the larger child
            if (child + 1 < tq->size && tq->heap[child + 1].priority > tq->heap[child].priority) {
                child++;
            }
            if (last.priority >= tq->heap[child].priority) {
                break;
            }
            tq->heap[i] = tq->heap[child];
            i = child;
        }
        tq->heap[i] = last;
    }

    pthread_mutex_unlock(&tq->mutex);
    return true;
}

// Wait for task with timeout (heap-based, O(log n))
static bool taskqueue_pop_wait(TaskQueue *tq, Task *task, int timeout_ms) {
    pthread_mutex_lock(&tq->mutex);

    if (tq->size == 0 && !tq->shutdown) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += timeout_ms * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&tq->not_empty, &tq->mutex, &ts);
    }

    if (tq->size == 0 || tq->shutdown) {
        pthread_mutex_unlock(&tq->mutex);
        return false;
    }

    // Root has highest priority
    *task = tq->heap[0];
    tq->size--;
    tq->total_popped++;

    if (tq->size > 0) {
        // Move last element to root and sift down
        Task last = tq->heap[tq->size];
        int i = 0;

        while (i * 2 + 1 < tq->size) {
            int child = i * 2 + 1;
            if (child + 1 < tq->size && tq->heap[child + 1].priority > tq->heap[child].priority) {
                child++;
            }
            if (last.priority >= tq->heap[child].priority) {
                break;
            }
            tq->heap[i] = tq->heap[child];
            i = child;
        }
        tq->heap[i] = last;
    }

    pthread_mutex_unlock(&tq->mutex);
    return true;
}

static void taskqueue_shutdown(TaskQueue *tq) {
    pthread_mutex_lock(&tq->mutex);
    tq->shutdown = true;
    pthread_cond_broadcast(&tq->not_empty);
    pthread_mutex_unlock(&tq->mutex);
}

static int taskqueue_size(TaskQueue *tq) {
    pthread_mutex_lock(&tq->mutex);
    int size = tq->size;
    pthread_mutex_unlock(&tq->mutex);
    return size;
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

static int board_unique(uint64_t player, uint64_t opponent,
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
#if defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX2__
    // Use AVX2 version if available and CPU supports it
    if (cpu_has_avx2) {
        return get_moves_avx2(P, O);
    }
#endif
#endif
    // Fallback to scalar version
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

static int evaluate_position(uint64_t player, uint64_t opponent) {
    if (!EVAL_WEIGHT) return 0;

    int empties = popcount(~(player | opponent));
    int ply = 60 - empties;

    if (ply >= (int)EVAL_N_PLY) ply = EVAL_N_PLY - 1;

    uint16_t features[EVAL_N_FEATURE];
    for (int i = 0; i < (int)EVAL_N_FEATURE; i++) {
        features[i] = compute_feature(player, opponent, i);
    }

    int sum = 0;
    int16_t *weights = EVAL_WEIGHT[ply][0];

    for (int i = 0; i < (int)EVAL_N_FEATURE; i++) {
        if (features[i] < EVAL_N_WEIGHT) {
            sum += weights[features[i]];
        }
    }

    return sum / 128;
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
        EVAL_WEIGHT[ply] = calloc(2, sizeof(int16_t*));
        EVAL_WEIGHT[ply][0] = calloc(EVAL_N_WEIGHT, sizeof(int16_t));
        EVAL_WEIGHT[ply][1] = calloc(EVAL_N_WEIGHT, sizeof(int16_t));
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
                    EVAL_WEIGHT[ply][1][j] = w[k];
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
                free(EVAL_WEIGHT[ply][1]);
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
// 注: TT_LOCK_STRIPES, CACHE_LINE_SIZE はファイル上部で定義済み

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
    init_zobrist();

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

    struct DFPNNode **children;
    int n_children;
    int depth;

    uint64_t visits;
} DFPNNode;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Memory Pool for DFPNNode (Arena Allocator)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 注: NODE_POOL_BLOCK_SIZE はファイル上部で定義済み

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

// Global state for work stealing
typedef struct {
    TaskQueue *task_queue;
    TranspositionTable *tt;

    // Results per root move
    Result *move_results;
    uint64_t *move_nodes;
    int *move_list;
    int *move_evals;
    int n_moves;

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

    // Statistics
    WorkStealingStats ws_stats;
    pthread_mutex_t stats_mutex;
} GlobalState;

typedef struct {
    pthread_t thread;
    int id;
    GlobalState *global;

    // Local statistics
    uint64_t nodes;
    uint64_t tasks_processed;
    uint64_t tasks_stolen;

    // Memory pool for node allocation (per-worker, no locking needed)
    NodePool node_pool;

    ThreadStats *stats;
    TreeStats *tree_stats;
} Worker;

static void dfpn_solve_node(Worker *worker, DFPNNode *node);

static DFPNNode* select_best_child_with_priority(DFPNNode *node) {
    if (!node->children || node->n_children == 0) return NULL;

    PriorityQueue *pq = pq_create(node->n_children);

    if (node->type == NODE_OR) {
        for (int i = 0; i < node->n_children; i++) {
            int priority = (PN_INF - node->children[i]->pn) + node->children[i]->eval_score;
            pq_push(pq, i, priority);
        }
    } else {
        for (int i = 0; i < node->n_children; i++) {
            int priority = (DN_INF - node->children[i]->dn) - node->children[i]->eval_score;
            pq_push(pq, i, priority);
        }
    }

    int best_idx = pq_pop(pq);
    pq_free(pq);

    return (best_idx >= 0) ? node->children[best_idx] : NULL;
}

static void update_pn_dn(DFPNNode *node) {
    if (node->children == NULL || node->n_children == 0) {
        return;
    }
    if (node->type == NODE_OR) {
        uint32_t min_pn = PN_INF;
        uint64_t sum_dn = 0;
        for (int i = 0; i < node->n_children; i++) {
            if (node->children[i]->pn < min_pn) {
                min_pn = node->children[i]->pn;
            }
            sum_dn += node->children[i]->dn;
            if (sum_dn >= DN_INF) sum_dn = DN_INF;
        }
        node->pn = min_pn;
        node->dn = (uint32_t)sum_dn;
    } else {
        uint64_t sum_pn = 0;
        uint32_t min_dn = DN_INF;
        for (int i = 0; i < node->n_children; i++) {
            sum_pn += node->children[i]->pn;
            if (sum_pn >= PN_INF) sum_pn = PN_INF;
            if (node->children[i]->dn < min_dn) {
                min_dn = node->children[i]->dn;
            }
        }
        node->pn = (uint32_t)sum_pn;
        node->dn = min_dn;
    }

    if (node->pn == 0) node->result = RESULT_EXACT_WIN;
    if (node->dn == 0) node->result = RESULT_EXACT_LOSE;
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

    PriorityQueue *pq = pq_create(n_moves);
    uint64_t moves_copy = moves;

    while(moves_copy) {
        int move = first_one(moves_copy);
        moves_copy &= moves_copy - 1;

        uint64_t p = node->player;
        uint64_t o = node->opponent;
        make_move(&p, &o, move);

        int priority = 0;
        if (worker->global->use_evaluation) {
            priority = -evaluate_position(p, o);
        }

        pq_push(pq, move, priority);
    }

    for (int i = 0; i < n_moves; i++) {
        int move = pq_pop(pq);

        uint64_t p = node->player;
        uint64_t o = node->opponent;
        make_move(&p, &o, move);

        DFPNNode *child = node_pool_alloc(&worker->node_pool);
        child->player = p;
        child->opponent = o;
        child->type = (node->type == NODE_OR) ? NODE_AND : NODE_OR;
        child->depth = node->depth - 1;
        child->pn = 1;
        child->dn = 1;

        if (worker->global->use_evaluation) {
            child->eval_score = -evaluate_position(p, o);
        }

        node->children[i] = child;
    }

    pq_free(pq);
}

static int get_final_score(uint64_t P, uint64_t O) {
    int p_count = popcount(P);
    int o_count = popcount(O);
    int empty = 64 - p_count - o_count;

    if (p_count > o_count) return p_count - o_count + empty;
    else if (o_count > p_count) return o_count - p_count - empty;
    else return 0;
}

static void dfpn_solve_node(Worker *worker, DFPNNode *node) {
    worker->nodes++;

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
        // If already solved, return immediately
        if (node->result != RESULT_UNKNOWN) {
            if (node->pn == 0 || node->dn == 0) return;
        }
        node->eval_score = eval_score;
    }

    if (node->children == NULL) {
        expand_node_with_evaluation(worker, node);
        if (node->n_children == 0) {
            int score = get_final_score(node->player, node->opponent);
            if (node->type == NODE_OR) {
                 node->result = score > 0 ? RESULT_EXACT_WIN : (score < 0 ? RESULT_EXACT_LOSE : RESULT_EXACT_DRAW);
            } else {
                 node->result = score > 0 ? RESULT_EXACT_LOSE : (score < 0 ? RESULT_EXACT_WIN : RESULT_EXACT_DRAW);
            }

            if (node->result == RESULT_EXACT_WIN) node->pn = 0; else node->pn = PN_INF;
            if (node->result == RESULT_EXACT_LOSE) node->dn = 0; else node->dn = DN_INF;
            if (node->result == RESULT_EXACT_DRAW) { node->pn = PN_INF; node->dn = PN_INF; }

            tt_store(worker->global->tt, key, node->depth, node->pn, node->dn, node->result, node->eval_score);
            if (worker->stats) worker->stats->tt_stores++;
            return;
        }
    }

    while(node->pn > 0 && node->dn > 0 &&
          node->pn < node->threshold_pn && node->dn < node->threshold_dn) {
        if (worker->global->found_win || worker->global->shutdown) return;

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

// Spawn child tasks for promising children
// Returns number of tasks spawned
static int spawn_child_tasks(Worker *worker, DFPNNode *node, Task *parent_task) {
    if (!node->children || node->n_children == 0) return 0;

    // Check if we should spawn subtasks
    int generation = parent_task->generation;
    if (generation >= worker->global->max_generation) return 0;
    if (node->depth < worker->global->min_depth_for_spawn) return 0;

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
    int spawn_limit = worker->global->spawn_limit;  // Use configurable limit

    for (int i = 0; i < node->n_children && spawned < spawn_limit; i++) {
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

        if (taskqueue_push(worker->global->task_queue, &subtask)) {
            spawned++;
            __sync_fetch_and_add(&worker->global->subtasks_spawned, 1);

            if (DEBUG_CONFIG.track_work_stealing) {
                debug_log("Worker %d spawned subtask gen=%d for root=%c%d, priority=%d, depth=%d\n",
                       worker->id, generation + 1,
                       'a' + (parent_task->root_move % 8), 8 - (parent_task->root_move / 8),
                       subtask.priority, child->depth);
            }
        }
    }

    free(children_prio);
    return spawned;
}

// Process a single task with dynamic task spawning
// Uses iterative threshold widening and spawns subtasks for parallelism
static void process_task(Worker *worker, Task *task) {
    worker->tasks_processed++;

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

    // Set thresholds to infinity - no artificial cutoff
    // Priority ordering and time limit control the search
    root->threshold_pn = PN_INF;
    root->threshold_dn = PN_INF;

    // Perform the search (TT probe is done inside dfpn_solve_node)
    uint64_t key = hash_position(p, o);
    dfpn_solve_node(worker, root);

    // Spawn child tasks for parallel exploration if not yet proven
    if (root->children != NULL && root->pn > 0 && root->dn > 0 &&
        !worker->global->found_win && !worker->global->shutdown) {
        int spawned = spawn_child_tasks(worker, root, task);
        if (spawned > 0 && DEBUG_CONFIG.verbose) {
            debug_log("Worker %d spawned %d subtasks\n", worker->id, spawned);
        }
    }

    Result result = RESULT_UNKNOWN;
    if (root->pn == 0) {
        result = (root->type == NODE_OR) ? RESULT_EXACT_WIN : RESULT_EXACT_LOSE;
    } else if (root->dn == 0) {
        result = (root->type == NODE_OR) ? RESULT_EXACT_LOSE : RESULT_EXACT_WIN;
    }

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
                }
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
                }
            }
        }

        // Atomic increment of tasks_completed
        atomic_add_int(&worker->global->tasks_completed, 1);
    } else {
        // Subtask completed (atomic increment)
        __sync_fetch_and_add(&worker->global->subtasks_completed, 1);

        // If subtask found a definitive result, update root task result via TT
        // LOCK-FREE: Use atomic operations for result updates
        if (result == RESULT_EXACT_WIN || result == RESULT_EXACT_LOSE) {
            // Find the index for this root move
            int move_idx = -1;
            for (int i = 0; i < worker->global->n_moves; i++) {
                if (worker->global->move_list[i] == task->root_move) {
                    move_idx = i;
                    break;
                }
            }

            if (move_idx >= 0) {
                // For subtasks, WIN at even generation means our move leads to win
                if (result == RESULT_EXACT_WIN && (task->generation % 2 == 0)) {
                    // Try to upgrade from UNKNOWN to WIN using CAS
                    Result expected = RESULT_UNKNOWN;
                    if (__sync_bool_compare_and_swap(&worker->global->move_results[move_idx],
                                                      expected, RESULT_EXACT_WIN)) {
                        // Successfully upgraded - try to set found_win
                        if (atomic_cas_bool(&worker->global->found_win, false, true)) {
                            __atomic_store_n(&worker->global->winning_move, task->root_move, __ATOMIC_RELEASE);
                            debug_log("Worker %d: subtask found WIN for root move %c%d!\n",
                                   worker->id,
                                   'a' + (task->root_move % 8), 8 - (task->root_move / 8));
                        }
                    }
                }
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
}

// Worker thread function with work stealing
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

    debug_log("Worker %d started (Work Stealing mode)\n", worker->id);

    while (!worker->global->shutdown && !worker->global->found_win) {
        Task task;

        // Try to get a task from the queue
        if (taskqueue_pop_wait(worker->global->task_queue, &task, 100)) {
            worker->tasks_stolen++;

            if (DEBUG_CONFIG.track_work_stealing) {
                // Lock-free atomic increment for debug stats
                __sync_fetch_and_add(&worker->global->ws_stats.tasks_stolen, 1);
            }

            // Reset node count for this task
            uint64_t nodes_before = worker->nodes;
            worker->nodes = 0;

            process_task(worker, &task);

            // Accumulate total nodes
            worker->nodes += nodes_before;
        } else {
            // No task available, check if we should exit
            if (worker->global->shutdown || worker->global->found_win) {
                break;
            }

            // Check if all tasks are done
            if (taskqueue_size(worker->global->task_queue) == 0) {
                // Small delay before checking again
                usleep(1000);
            }
        }
    }

    if (worker->stats) {
        worker->stats->is_active = false;
        worker->stats->nodes_explored = worker->nodes;
        worker->stats->tasks_processed = worker->tasks_processed;
        worker->stats->tasks_stolen = worker->tasks_stolen;
    }

    debug_log("Worker %d finished: %llu nodes, %llu tasks processed\n",
           worker->id,
           (unsigned long long)worker->nodes,
           (unsigned long long)worker->tasks_processed);

    return NULL;
}

// Real-time monitoring thread
static void* monitor_thread(void *arg) {
    GlobalState *global = (GlobalState*)arg;

    while (!global->shutdown && !global->found_win) {
        sleep(2);

        if (!DEBUG_CONFIG.real_time_monitor) continue;

        debug_log("\n--- Real-time Status (Work Stealing) ---\n");
        debug_log("Task queue size: %d\n", taskqueue_size(global->task_queue));
        debug_log("Tasks pushed: %llu, popped: %llu\n",
               (unsigned long long)global->task_queue->total_pushed,
               (unsigned long long)global->task_queue->total_popped);

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
// Main Solver with Work Stealing
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Result solve_endgame(uint64_t player, uint64_t opponent, int num_threads,
                    double time_limit, int *best_move, bool use_evaluation) {
    // Initialize CPU feature detection for SIMD acceleration
    check_cpu_features();

    debug_log("\n=== Othello Endgame Solver (Work Stealing Version) ===\n");
    debug_log("Threads: %d (fixed), Time limit: %.1fs\n", num_threads, time_limit);
    debug_log("Evaluation function: %s\n", use_evaluation ? "ENABLED" : "DISABLED");

    int empties = popcount(~(player | opponent));
    debug_log("Empties: %d\n", empties);

    // Initialize global state
    GlobalState global = {0};
    global.task_queue = taskqueue_create(MAX_TASK_QUEUE_SIZE);
    global.tt = tt_create(TT_SIZE_MB);
    global.time_limit = time_limit;
    global.use_evaluation = use_evaluation;
    global.found_win = false;
    global.shutdown = false;
    clock_gettime(CLOCK_MONOTONIC, &global.start_time);
    pthread_mutex_init(&global.stats_mutex, NULL);

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
        taskqueue_free(global.task_queue);
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

    // Create tasks for each root move
    PriorityQueue *initial_order = pq_create(n_moves);
    uint64_t moves_copy = moves;
    int idx = 0;

    while(moves_copy) {
        int move = first_one(moves_copy);
        moves_copy &= moves_copy - 1;

        uint64_t p = player;
        uint64_t o = opponent;
        make_move(&p, &o, move);

        int eval = use_evaluation ? -evaluate_position(p, o) : 0;
        pq_push(initial_order, move, eval);

        global.move_list[idx] = move;
        global.move_evals[idx] = eval;
        idx++;
    }

    // Push tasks in priority order
    debug_log("Move ordering by evaluation:\n");
    for (int i = 0; i < n_moves; i++) {
        int move = pq_pop(initial_order);

        uint64_t p = player;
        uint64_t o = opponent;
        make_move(&p, &o, move);

        int eval = use_evaluation ? -evaluate_position(p, o) : 0;

        Task task = {
            .player = p,
            .opponent = o,
            .root_move = move,
            .priority = eval + (n_moves - i) * 1000,  // Priority includes order
            .eval_score = eval,
            .is_root_task = true,
            .depth = empties - 1,   // After making move, one less empty
            .node_type = NODE_AND,  // Opponent's turn after our move
            .generation = 0         // Root level
        };

        taskqueue_push(global.task_queue, &task);
        debug_log("  %c%d: eval=%d, priority=%d\n",
               'a' + (move % 8), 8 - (move / 8), eval, task.priority);
    }
    debug_log("\n");

    pq_free(initial_order);

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
        if (thread_stats) workers[i].stats = &thread_stats[i];
        if (tree_stats) workers[i].tree_stats = &tree_stats[i];
        node_pool_init(&workers[i].node_pool);
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
    taskqueue_shutdown(global.task_queue);

    // Wait for workers to finish
    for (int i = 0; i < num_threads; i++) {
        pthread_join(workers[i].thread, NULL);
    }

    // Cleanup worker memory pools
    for (int i = 0; i < num_threads; i++) {
        node_pool_destroy(&workers[i].node_pool);
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
    debug_log("Total tasks processed: %llu\n", (unsigned long long)global.task_queue->total_popped);
    debug_log("Early termination: %s\n", global.found_win ? "YES (WIN found)" : "NO");

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
    free(global.move_results);
    free(global.move_nodes);
    free(global.move_list);
    free(global.move_evals);
    free(workers);
    taskqueue_free(global.task_queue);
    tt_free(global.tt);
    pthread_mutex_destroy(&global.stats_mutex);

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

    // Dynamic task spawning options (defaults for 4-core systems)
    int max_generation = 3;
    int min_depth_for_spawn = 6;
    int spawn_limit = 3;

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
