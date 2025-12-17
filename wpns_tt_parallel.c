/**
 * @file wpns_tt_parallel.c
 * @brief TT-Parallel Weak Proof Number Search for Othello Endgame
 *
 * Lazy SMP style parallelization:
 * - Multiple threads independently search from the root
 * - Shared transposition table for communication
 * - NO work-stealing: each thread does its own complete search
 * - Parallelism comes from TT sharing between threads
 *
 * Based on sequential wpns.c from soturon
 *
 * Usage: ./wpns_tt_parallel <position_file> <num_threads> <time_limit> [eval_file] [-v]
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>

// ============================================================
// Configuration
// ============================================================

#ifndef MAX_THREADS
#define MAX_THREADS 1024
#endif

#ifndef TT_SIZE_MB
#define TT_SIZE_MB 4096
#endif

#ifndef TT_LOCK_STRIPES
#define TT_LOCK_STRIPES 65536
#endif

// ============================================================
// Type definitions
// ============================================================

typedef uint64_t bitboard;

// Game result values
#define WIN 1
#define LOSE -1
#define DRAW 0
#define UNKNOWN -2

// Player colors
#define BLACK 1
#define WHITE -1

// Infinity for proof/disproof numbers
#define PN_INF 100000000

// Directions for move generation (for flip_discs diagonal handling)
static const int diag_directions[4] = {7, 9, -7, -9};

// ============================================================
// Node structure (thread-local)
// ============================================================

typedef struct node {
    bitboard black;
    bitboard white;
    int color;
    int depth;
    int proof;
    int disproof;
    struct node *parent;
    struct node *child;
    struct node *next;
} node_t;

// ============================================================
// Transposition Table (shared between threads)
// ============================================================

typedef struct tt_entry {
    bitboard black;
    bitboard white;
    int8_t color;
    int8_t valid;
    int32_t proof;
    int32_t disproof;
} tt_entry_t;

// Global TT
static tt_entry_t *g_tt = NULL;
static size_t g_tt_size = 0;
static pthread_spinlock_t *g_tt_locks = NULL;

// ============================================================
// Global state
// ============================================================

static atomic_bool g_solved = false;
static atomic_int g_result = UNKNOWN;
static atomic_ullong g_total_nodes = 0;
static atomic_ullong g_tt_hits = 0;
static atomic_ullong g_tt_stores = 0;

// Timing
static double g_time_limit = 300.0;
static struct timespec g_start_time;

// Verbosity
static bool g_verbose = false;

// ============================================================
// Function prototypes
// ============================================================

// Core search
static int wpns_search_thread(bitboard black, bitboard white, int color, int depth, int thread_id);
static node_t *create_node(bitboard black, bitboard white, int color, int depth);
static void free_node(node_t *node);
static void pns_search(node_t *node, int proof_limit, int disproof_limit, int thread_id);
static int is_terminal(node_t *node);
static void judge_node(node_t *node);
static void generate_children(node_t *node);
static void sort_children(node_t *node);
static void update_proof_disproof(node_t *node);

// Board operations
static int count_bit(bitboard b);
static int puttable(bitboard black, bitboard white, int color);
static int can_put(bitboard black, bitboard white, int pos, int color);
static int judge(bitboard black, bitboard white);
static int get_color(bitboard black, bitboard white, int pos);
static void set_color(bitboard *black, bitboard *white, int pos, int color);

// Transposition table
static void tt_init(size_t size_mb);
static void tt_free(void);
static uint64_t tt_hash(bitboard black, bitboard white);
static void tt_store(bitboard black, bitboard white, int color, int proof, int disproof);
static bool tt_lookup(bitboard black, bitboard white, int color, int *proof, int *disproof);

// Utility
static double get_elapsed_time(void);
static int read_position_file(const char *filename, bitboard *black, bitboard *white, int *color);

// ============================================================
// Main
// ============================================================

typedef struct {
    bitboard black;
    bitboard white;
    int color;
    int depth;
    int thread_id;
} thread_args_t;

static void *worker_thread(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;

    int result = wpns_search_thread(args->black, args->white, args->color, args->depth, args->thread_id);

    // If this thread found a solution, set global result
    if (result != UNKNOWN) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&g_solved, &expected, true)) {
            atomic_store(&g_result, result);
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <position_file> <num_threads> <time_limit> [eval_file] [-v]\n", argv[0]);
        return 1;
    }

    const char *pos_file = argv[1];
    int num_threads = atoi(argv[2]);
    g_time_limit = atof(argv[3]);

    // Parse optional arguments
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
        }
    }

    // Clamp thread count
    if (num_threads < 1) num_threads = 1;
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    // Read position
    bitboard black, white;
    int color;
    if (read_position_file(pos_file, &black, &white, &color) != 0) {
        fprintf(stderr, "Error: Failed to read position file: %s\n", pos_file);
        return 1;
    }

    int depth = 64 - count_bit(black) - count_bit(white);

    if (g_verbose) {
        printf("WPNS TT-Parallel Solver\n");
        printf("Position: %s\n", pos_file);
        printf("Threads: %d\n", num_threads);
        printf("Time limit: %.1f sec\n", g_time_limit);
        printf("Empty squares: %d\n", depth);
        printf("Player: %s\n", color == BLACK ? "BLACK" : "WHITE");
    }

    // Initialize transposition table
    tt_init(TT_SIZE_MB);

    // Start timing
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);

    // Create worker threads
    pthread_t threads[MAX_THREADS];
    thread_args_t thread_args[MAX_THREADS];

    for (int i = 0; i < num_threads; i++) {
        thread_args[i].black = black;
        thread_args[i].white = white;
        thread_args[i].color = color;
        thread_args[i].depth = depth;
        thread_args[i].thread_id = i;

        if (pthread_create(&threads[i], NULL, worker_thread, &thread_args[i]) != 0) {
            fprintf(stderr, "Error: Failed to create thread %d\n", i);
            return 1;
        }
    }

    // Wait for all threads
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    double elapsed = get_elapsed_time();
    unsigned long long total_nodes = atomic_load(&g_total_nodes);
    unsigned long long tt_hits = atomic_load(&g_tt_hits);
    unsigned long long tt_stores = atomic_load(&g_tt_stores);
    int result = atomic_load(&g_result);

    // Output result in compatible format
    const char *result_str;
    switch (result) {
        case WIN:  result_str = "WIN"; break;
        case LOSE: result_str = "LOSE"; break;
        case DRAW: result_str = "DRAW"; break;
        default:   result_str = "UNKNOWN"; break;
    }

    unsigned long long nps = (elapsed > 0) ? (unsigned long long)(total_nodes / elapsed) : 0;

    printf("Total: %llu nodes in %.3f sec (%llu NPS)\n", total_nodes, elapsed, nps);
    printf("Result: %s\n", result_str);
    printf("TT hits: %llu, TT stores: %llu\n", tt_hits, tt_stores);

    if (g_verbose) {
        double tt_hit_rate = (tt_hits + tt_stores > 0) ?
            (100.0 * tt_hits / (tt_hits + tt_stores)) : 0.0;
        printf("TT hit rate: %.2f%%\n", tt_hit_rate);
    }

    tt_free();

    return (result == UNKNOWN) ? 1 : 0;
}

// ============================================================
// Core Search Implementation
// ============================================================

static int wpns_search_thread(bitboard black, bitboard white, int color, int depth, int thread_id) {
    // Each thread does independent search with different initial limits
    // Stagger the starting limits to promote diversity
    int base_limit = 1 + (thread_id % 4);

    node_t *root = create_node(black, white, color, depth);

    int proof_limit = base_limit;
    int disproof_limit = base_limit;

    while (!atomic_load(&g_solved) && get_elapsed_time() < g_time_limit) {
        pns_search(root, proof_limit, disproof_limit, thread_id);

        if (is_terminal(root)) {
            break;
        }

        if (root->proof >= proof_limit) {
            proof_limit = root->proof + 1;
        }
        if (root->disproof >= disproof_limit) {
            disproof_limit = root->disproof + 1;
        }

        // Prevent overflow
        if (proof_limit > PN_INF) proof_limit = PN_INF;
        if (disproof_limit > PN_INF) disproof_limit = PN_INF;
    }

    // Result interpretation (fixed to match hybrid solver semantics)
    // - proof >= PN_INF: the current player's position is proven winning (WIN)
    // - disproof >= PN_INF: the current player's position is proven losing (LOSE)
    int result = UNKNOWN;
    if (root->proof >= PN_INF) {
        if (root->disproof >= PN_INF) {
            result = DRAW;
        } else {
            result = WIN;
        }
    } else if (root->disproof >= PN_INF) {
        result = LOSE;
    }

    free_node(root);
    return result;
}

static node_t *create_node(bitboard black, bitboard white, int color, int depth) {
    node_t *node = (node_t *)malloc(sizeof(node_t));
    if (!node) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        exit(1);
    }

    node->black = black;
    node->white = white;
    node->color = color;
    node->depth = depth;
    node->proof = 1;
    node->disproof = 1;
    node->parent = NULL;
    node->child = NULL;
    node->next = NULL;

    return node;
}

static void free_node(node_t *node) {
    if (!node) return;

    // Free children recursively
    node_t *child = node->child;
    while (child) {
        node_t *next = child->next;
        free_node(child);
        child = next;
    }

    free(node);
}

static void pns_search(node_t *node, int proof_limit, int disproof_limit, int thread_id) {
    // Check for early termination
    if (atomic_load(&g_solved) || get_elapsed_time() >= g_time_limit) {
        return;
    }

    atomic_fetch_add(&g_total_nodes, 1);

    // Try to get from TT
    int tt_proof, tt_disproof;
    if (tt_lookup(node->black, node->white, node->color, &tt_proof, &tt_disproof)) {
        node->proof = tt_proof;
        node->disproof = tt_disproof;

        if (node->proof >= proof_limit || node->disproof >= disproof_limit) {
            return;
        }
    }

    // Terminal node check
    if (is_terminal(node)) {
        judge_node(node);
        tt_store(node->black, node->white, node->color, node->proof, node->disproof);
        return;
    }

    // Non-terminal node
    while (!atomic_load(&g_solved) && get_elapsed_time() < g_time_limit) {
        // Generate children if needed
        if (node->child == NULL) {
            generate_children(node);
        }

        // Update children from TT
        for (node_t *child = node->child; child != NULL; child = child->next) {
            int cp, cd;
            if (tt_lookup(child->black, child->white, child->color, &cp, &cd)) {
                child->proof = cp;
                child->disproof = cd;
            }

            if (is_terminal(child)) {
                judge_node(child);
            }
        }

        int old_proof = node->proof;
        int old_disproof = node->disproof;

        update_proof_disproof(node);

        // If values changed, store and return
        if (node->proof != old_proof || node->disproof != old_disproof) {
            tt_store(node->black, node->white, node->color, node->proof, node->disproof);
            return;
        }

        // Check limits
        if (node->proof >= proof_limit || node->disproof >= disproof_limit) {
            tt_store(node->black, node->white, node->color, node->proof, node->disproof);
            return;
        }

        // Check if solved
        if (is_terminal(node)) {
            judge_node(node);
            tt_store(node->black, node->white, node->color, node->proof, node->disproof);
            return;
        }

        // Sort and recurse on best child
        sort_children(node);
        pns_search(node->child, proof_limit, disproof_limit, thread_id);
    }
}

// Check if game is over (both players pass or no empties)
static int is_game_over(bitboard black, bitboard white) {
    int depth = 64 - count_bit(black) - count_bit(white);
    if (depth == 0) return 1;

    // Check if both players must pass
    if (!puttable(black, white, BLACK) && !puttable(black, white, WHITE)) {
        return 1;
    }
    return 0;
}

// Get the actual color to play (handles pass)
static int get_active_color(bitboard black, bitboard white, int color) {
    if (puttable(black, white, color)) {
        return color;
    }
    // Current player must pass, return opponent
    return -color;
}

static int is_terminal(node_t *node) {
    // Already proven/disproven
    if ((node->proof >= PN_INF && node->disproof == 0) ||
        (node->proof == 0 && node->disproof >= PN_INF)) {
        return 1;
    }

    // Check if game is over
    if (is_game_over(node->black, node->white)) {
        return 1;
    }

    return 0;
}

static void judge_node(node_t *node) {
    if ((node->proof >= PN_INF && node->disproof == 0) ||
        (node->proof == 0 && node->disproof >= PN_INF)) {
        return;
    }

    int result = judge(node->black, node->white);
    switch (result) {
        case BLACK:  // Black wins
            node->proof = 0;
            node->disproof = PN_INF;
            break;
        case WHITE:  // Black loses
        case DRAW:   // Treat as loss for black (we search for black win)
            node->proof = PN_INF;
            node->disproof = 0;
            break;
    }
}

static void generate_children(node_t *node) {
    // Get the actual color that can play (handles pass)
    int active_color = get_active_color(node->black, node->white, node->color);

    for (int i = 0; i < 64; i++) {
        if (can_put(node->black, node->white, i, active_color)) {
            bitboard new_black = node->black;
            bitboard new_white = node->white;
            set_color(&new_black, &new_white, i, active_color);

            // Next player is opponent of active color
            int next_color = -active_color;
            node_t *child = create_node(new_black, new_white, next_color, node->depth - 1);
            child->parent = node;
            child->next = node->child;
            node->child = child;
        }
    }
}

static void sort_children(node_t *node) {
    // Count children
    int count = 0;
    for (node_t *c = node->child; c != NULL; c = c->next) {
        count++;
    }

    if (count <= 1) return;

    // Use active color to determine sort order
    int active_color = get_active_color(node->black, node->white, node->color);

    // Simple bubble sort (children count is small)
    bool swapped;
    do {
        swapped = false;
        node_t **pp = &node->child;
        while (*pp && (*pp)->next) {
            node_t *a = *pp;
            node_t *b = a->next;

            bool need_swap;
            if (active_color == BLACK) {
                // OR node: sort by proof (ascending - smallest first)
                need_swap = (a->proof > b->proof);
            } else {
                // AND node: sort by disproof (ascending - smallest first)
                need_swap = (a->disproof > b->disproof);
            }

            if (need_swap) {
                a->next = b->next;
                b->next = a;
                *pp = b;
                swapped = true;
            }
            pp = &(*pp)->next;
        }
    } while (swapped);
}

// Weak proof number update (from original wpns.c)
static void update_proof_disproof(node_t *node) {
    if (is_terminal(node)) {
        return;
    }

    if (node->child == NULL) {
        return;
    }

    // Use active color to determine OR/AND node type
    // (handles pass situations correctly)
    int active_color = get_active_color(node->black, node->white, node->color);

    int branch = 0;
    node_t *child = node->child;

    node->proof = child->proof;
    node->disproof = child->disproof;

    if (active_color == BLACK) {
        // OR node (BLACK is trying to prove a win)
        for (child = child->next; child != NULL; child = child->next) {
            if (child->proof < node->proof) {
                node->proof = child->proof;
            }
            if (child->disproof > node->disproof) {
                node->disproof = child->disproof;
            }
            if (!(child->proof == 0 || child->disproof == 0)) {
                branch++;
            }
        }
        node->disproof += branch;
    } else {
        // AND node (WHITE is trying to disprove BLACK's win)
        for (child = child->next; child != NULL; child = child->next) {
            if (child->proof > node->proof) {
                node->proof = child->proof;
            }
            if (child->disproof < node->disproof) {
                node->disproof = child->disproof;
            }
            if (!(child->proof == 0 || child->disproof == 0)) {
                branch++;
            }
        }
        node->proof += branch;
    }

    if (node->proof > PN_INF) node->proof = PN_INF;
    if (node->disproof > PN_INF) node->disproof = PN_INF;
}

// ============================================================
// Board Operations (using hybrid solver bitboard approach)
// ============================================================

static int count_bit(bitboard b) {
    return __builtin_popcountll(b);
}

// Bitboard-based move generation (from hybrid solver - exact copy)
static inline uint64_t get_moves(uint64_t P, uint64_t O) {
    uint64_t mask, moves, flip_l, flip_r, pre_l, pre_r;
    uint64_t flip_h, flip_v, flip_d1, flip_d2;

    // Horizontal (left/right) - mask excludes edge columns
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

    // Vertical (up/down) - mask excludes edge rows
    mask = O & 0x00ffffffffffff00ULL;
    flip_h = mask & (P << 8);
    flip_v = mask & (P >> 8);
    flip_h |= mask & (flip_h << 8);
    flip_v |= mask & (flip_v >> 8);
    flip_h |= mask & ((flip_h & (mask << 8)) << 16);
    flip_v |= mask & ((flip_v & (mask >> 8)) >> 16);
    flip_h |= mask & (flip_h << 8);
    flip_v |= mask & (flip_v >> 8);

    // Diagonal - mask excludes edge rows and columns
    mask = O & 0x007e7e7e7e7e7e00ULL;
    flip_d1 = mask & (P << 9);
    flip_d2 = mask & (P << 7);
    flip_d1 |= mask & (flip_d1 << 9);
    flip_d2 |= mask & (flip_d2 << 7);
    flip_d1 |= mask & ((flip_d1 & (mask << 9)) << 18);
    flip_d2 |= mask & ((flip_d2 & (mask << 7)) << 14);
    flip_d1 |= mask & (flip_d1 << 9);
    flip_d2 |= mask & (flip_d2 << 7);

    // Accumulate opposite diagonal directions (key fix!)
    flip_d1 |= mask & (P >> 9);
    flip_d2 |= mask & (P >> 7);
    flip_d1 |= mask & (flip_d1 >> 9);
    flip_d2 |= mask & (flip_d2 >> 7);
    flip_d1 |= mask & ((flip_d1 & (mask >> 9)) >> 18);
    flip_d2 |= mask & ((flip_d2 & (mask >> 7)) >> 14);
    flip_d1 |= mask & (flip_d1 >> 9);
    flip_d2 |= mask & (flip_d2 >> 7);

    // Calculate all moves including both diagonal directions
    moves = (flip_l << 1) | (flip_r >> 1) |
            (flip_h << 8) | (flip_v >> 8) |
            (flip_d1 << 9) | (flip_d1 >> 9) |
            (flip_d2 << 7) | (flip_d2 >> 7);

    return moves & ~(P | O);
}

static int puttable(bitboard black, bitboard white, int color) {
    uint64_t player = (color == BLACK) ? black : white;
    uint64_t opponent = (color == BLACK) ? white : black;
    return get_moves(player, opponent) != 0;
}

static int can_put(bitboard black, bitboard white, int pos, int color) {
    uint64_t player = (color == BLACK) ? black : white;
    uint64_t opponent = (color == BLACK) ? white : black;
    uint64_t moves = get_moves(player, opponent);
    return (moves >> pos) & 1;
}

static int judge(bitboard black, bitboard white) {
    int black_count = count_bit(black);
    int white_count = count_bit(white);

    if (black_count > white_count) return BLACK;
    if (black_count < white_count) return WHITE;
    return DRAW;
}

static int get_color(bitboard black, bitboard white, int pos) {
    // Use direct pos (hybrid convention: string pos i → bit i)
    if ((black >> pos) & 1) return BLACK;
    if ((white >> pos) & 1) return WHITE;
    return 0;
}

// Flip discs function (from hybrid solver)
static uint64_t flip_discs(uint64_t P, uint64_t O, int pos) {
    uint64_t flip = 0;
    uint64_t move_bit = 1ULL << pos;
    int x = pos & 7;
    int y = pos >> 3;

    // Horizontal right
    if (x < 6) {
        uint64_t mask = 0x7eULL << (y * 8);
        uint64_t outflank = ((0x80ULL << (y * 8)) - move_bit) & (P|O) & mask;
        if (outflank) {
            uint64_t boundary = outflank & -outflank;
            if (P & boundary) flip |= (boundary - move_bit) & mask;
        }
    }
    // Horizontal left
    if (x > 1) {
        uint64_t mask = 0x7eULL << (y * 8);
        uint64_t outflank = (move_bit - 1) & (P|O) & mask;
        if (outflank) {
            uint64_t boundary = 1ULL << (63 - __builtin_clzll(outflank));
            if (P & boundary) flip |= (move_bit - boundary - 1) & mask;
        }
    }
    // Vertical down
    if (y < 6) {
        uint64_t mask = 0x00ffffffffffff00ULL & (0x0101010101010101ULL << x);
        uint64_t outflank = ((0x8000000000000000ULL >> (7 - x)) - move_bit) & (P|O) & mask;
        if (outflank) {
            uint64_t boundary = outflank & -outflank;
            if (P & boundary) flip |= (boundary - move_bit) & mask;
        }
    }
    // Vertical up
    if (y > 1) {
        uint64_t mask = 0x00ffffffffffff00ULL & (0x0101010101010101ULL << x);
        uint64_t outflank = (move_bit - 1) & (P|O) & mask;
        if (outflank) {
            uint64_t boundary = 1ULL << (63 - __builtin_clzll(outflank));
            if (P & boundary) flip |= (move_bit - boundary - 1) & mask;
        }
    }
    // Diagonals
    for (int d = 0; d < 4; d++) {
        int dir = diag_directions[d];
        int p = pos + dir;
        uint64_t line = 0;
        while (p >= 0 && p < 64) {
            int px = p & 7, py = p >> 3;
            int dx = px - x, dy = py - y;
            if ((dx < 0 ? -dx : dx) != (dy < 0 ? -dy : dy)) break;
            if (O & (1ULL << p)) { line |= 1ULL << p; p += dir; }
            else if (P & (1ULL << p)) { flip |= line; break; }
            else break;
        }
    }
    return flip;
}

static void set_color(bitboard *black, bitboard *white, int pos, int color) {
    uint64_t player = (color == BLACK) ? *black : *white;
    uint64_t opponent = (color == BLACK) ? *white : *black;

    uint64_t flipped = flip_discs(player, opponent, pos);

    // Apply the move
    if (color == BLACK) {
        *black = player | (1ULL << pos) | flipped;
        *white = opponent ^ flipped;
    } else {
        *white = player | (1ULL << pos) | flipped;
        *black = opponent ^ flipped;
    }
}

// ============================================================
// Transposition Table
// ============================================================

static void tt_init(size_t size_mb) {
    g_tt_size = (size_mb * 1024 * 1024) / sizeof(tt_entry_t);

    g_tt = (tt_entry_t *)calloc(g_tt_size, sizeof(tt_entry_t));
    if (!g_tt) {
        fprintf(stderr, "Error: Failed to allocate TT (%zu MB)\n", size_mb);
        exit(1);
    }

    g_tt_locks = (pthread_spinlock_t *)malloc(TT_LOCK_STRIPES * sizeof(pthread_spinlock_t));
    if (!g_tt_locks) {
        fprintf(stderr, "Error: Failed to allocate TT locks\n");
        exit(1);
    }

    for (size_t i = 0; i < TT_LOCK_STRIPES; i++) {
        pthread_spin_init(&g_tt_locks[i], PTHREAD_PROCESS_PRIVATE);
    }

    if (g_verbose) {
        printf("TT initialized: %zu entries (%.1f MB)\n", g_tt_size,
               (double)(g_tt_size * sizeof(tt_entry_t)) / (1024 * 1024));
    }
}

static void tt_free(void) {
    if (g_tt) {
        free(g_tt);
        g_tt = NULL;
    }

    if (g_tt_locks) {
        for (size_t i = 0; i < TT_LOCK_STRIPES; i++) {
            pthread_spin_destroy(&g_tt_locks[i]);
        }
        free((void *)g_tt_locks);
        g_tt_locks = NULL;
    }
}

static uint64_t tt_hash(bitboard black, bitboard white) {
    // Simple hash combining both boards
    uint64_t h = black ^ (white * 0x9E3779B97F4A7C15ULL);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return h;
}

static void tt_store(bitboard black, bitboard white, int color, int proof, int disproof) {
    uint64_t hash = tt_hash(black, white);
    size_t idx = hash % g_tt_size;
    size_t lock_idx = idx % TT_LOCK_STRIPES;

    pthread_spin_lock(&g_tt_locks[lock_idx]);

    tt_entry_t *entry = &g_tt[idx];

    // Always replace (or use more sophisticated replacement strategy)
    entry->black = black;
    entry->white = white;
    entry->color = color;
    entry->proof = proof;
    entry->disproof = disproof;
    entry->valid = 1;

    pthread_spin_unlock(&g_tt_locks[lock_idx]);

    atomic_fetch_add(&g_tt_stores, 1);
}

static bool tt_lookup(bitboard black, bitboard white, int color, int *proof, int *disproof) {
    uint64_t hash = tt_hash(black, white);
    size_t idx = hash % g_tt_size;
    size_t lock_idx = idx % TT_LOCK_STRIPES;

    pthread_spin_lock(&g_tt_locks[lock_idx]);

    tt_entry_t *entry = &g_tt[idx];

    bool found = false;
    if (entry->valid &&
        entry->black == black &&
        entry->white == white &&
        entry->color == color) {
        *proof = entry->proof;
        *disproof = entry->disproof;
        found = true;
    }

    pthread_spin_unlock(&g_tt_locks[lock_idx]);

    if (found) {
        atomic_fetch_add(&g_tt_hits, 1);
    }

    return found;
}

// ============================================================
// Utility Functions
// ============================================================

static double get_elapsed_time(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_start_time.tv_sec) +
           (now.tv_nsec - g_start_time.tv_nsec) * 1e-9;
}

static int read_position_file(const char *filename, bitboard *black, bitboard *white, int *color) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    *black = 0;
    *white = 0;
    *color = BLACK;

    // First line: 64 character board representation
    // Format: X=black, O=white, -=empty (single line)
    if (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);

        // Check if it's the 64-character format
        // Use hybrid convention: string pos i → bit i
        if (len >= 64) {
            for (int pos = 0; pos < 64; pos++) {
                char c = line[pos];
                if (c == 'X' || c == 'x' || c == '*') {
                    *black |= (1ULL << pos);
                } else if (c == 'O' || c == 'o' || c == '0') {
                    *white |= (1ULL << pos);
                }
                // '-' or '.' means empty
            }

            // Read player color from second line
            if (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "Black", 5) == 0 || strncmp(line, "black", 5) == 0) {
                    *color = BLACK;
                } else if (strncmp(line, "White", 5) == 0 || strncmp(line, "white", 5) == 0) {
                    *color = WHITE;
                }
            }

            fclose(fp);
            return 0;
        }

        // Try hex format: black_hex white_hex color
        unsigned long long b, w;
        int c;
        if (sscanf(line, "%llx %llx %d", &b, &w, &c) == 3) {
            *black = b;
            *white = w;
            *color = c;
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return -1;
}
