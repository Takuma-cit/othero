/**
 * Deep_Pns_benchmark.c
 * 
 * Deep_Pns.cのベンチマーク用修正版
 * - ファイルからポジション読み込み対応
 * - 結果をパース可能なフォーマットで出力
 * - コマンドライン引数でタイムアウト指定可能
 * 
 * 元のアルゴリズム（DeepPN, R=1）は維持
 * 
 * コンパイル:
 *   gcc -O3 -march=native -o deep_pns_benchmark Deep_Pns_benchmark.c -lm
 * 
 * 使用方法:
 *   ./deep_pns_benchmark <position_file> [time_limit_sec]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <signal.h>

// bitboard
typedef uint64_t bitboard;

// 勝敗の評価値
#define WIN 1
#define LOSE -1
#define DRAW 0
#define UNKNOWN -2

// 手番の色に対応する値
#define BLACK 1
#define WHITE -1

// 無限大の値
#define INF 10000000
// 未定義の値
#define UNDEFINED 0
// 方向の配列
const int directions[8] = {-1,1,-9,-7,7,9,-8,8};
// 石の判定で相手のビットボードの両脇を隠す際に使用
#define rightleft_HIDE_BIT 0x7E7E7E7E7E7E7E7E
#define topbottom_HIDE_BIT 0x00FFFFFFFFFFFF00

/*
  RはDPNへのdeep値の影響力
  0のときに単純な深さ優先探索
  1のときに単純な証明数探索になる
 */
#define R 1

typedef struct node {
  int num_node;   // 節点の番号
  bitboard black; // 黒のbitboard
  bitboard white; // 白のbitboard
  int color;      // 手番の色
  int depth;      // 残りの深さ
  float deep;       // 深層値
  int proof;      // 証明数
  int disproof;   // 反証数
  float dpn;        // deep proof-Number
  struct node *parent; // 親ノードへのポインタ
  struct node *child;  // 子ノードへのポインタ
  struct node *next;   // 他の子ノードへのポインタ
} node_t;

// 置換表の定義
typedef struct hash {
  int num_node;
  bitboard black; // 黒の配置
  bitboard white; // 白の配置
  int color;      // 手番
  int proof;      // 証明数
  int disproof;   // 反証数
  float dpn;
  struct hash *next; // 次要素へのポインタ
} hash_t;

/*
  転置表の要素数
  素数ならハッシュ値を計算する際の剰余が被りにくい。
*/
#define HASH_SIZE 999983

hash_t *hash_table[HASH_SIZE];

// 展開節点数
int node_num = 0;
// 探索節点数
int search_node_num = 0;
// 転置表の保存回数
int store_num = 0;
// 転値表の使用回数
int use_hash_num = 0;

// タイムアウト用グローバル変数
volatile int timeout_flag = 0;
double time_limit_sec = 300.0;
struct timespec global_start_time;

// 関数プロトタイプ
int pns(bitboard black, bitboard white, int color, int depth);
node_t *create_node(bitboard black, bitboard white, int color, int depth);
void pns_search(node_t *node);
int is_terminal(node_t *node);
int count_bit(bitboard b);
int count_puttable(bitboard black, bitboard white, int color);
int puttable(bitboard black, bitboard white, int color);
int can_put(bitboard black, bitboard white, int pos ,int color);
int judge(bitboard black, bitboard white);
void judge_node(node_t *node);
int get_color(bitboard black, bitboard white, int pos);
void generate_children(node_t *node);
void sort_children(node_t *node);
void quick_sort(node_t *array[], int left, int right,int color);
void update_proof_disproof(node_t *node);
void set_color(bitboard *black, bitboard *white, int pos, int color);
void DPN(node_t *node);
void solved_sort(node_t *array[], int left, int right);
void set_deep(node_t *node);

// 転置表
void init_hash();
int hash_value(bitboard black, bitboard white);
void store_hash(node_t *node);
void update_hash(node_t *node, hash_t *entry);
hash_t *get_hash(node_t *node);

// タイムアウトチェック（定期的に呼び出し）
int check_timeout() {
    if (timeout_flag) return 1;
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - global_start_time.tv_sec) + 
                     (now.tv_nsec - global_start_time.tv_nsec) * 1e-9;
    
    if (elapsed >= time_limit_sec) {
        timeout_flag = 1;
        return 1;
    }
    return 0;
}

// ファイルからポジション読み込み
int load_position(const char *filename, bitboard *black, bitboard *white, int *color) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("ファイルオープンエラー");
        return -1;
    }
    
    char board_str[128];
    char turn_str[128];
    
    if (fgets(board_str, sizeof(board_str), f) == NULL) {
        fclose(f);
        fprintf(stderr, "盤面読み込みエラー\n");
        return -1;
    }
    if (fgets(turn_str, sizeof(turn_str), f) == NULL) {
        fclose(f);
        fprintf(stderr, "手番読み込みエラー\n");
        return -1;
    }
    fclose(f);
    
    *black = 0;
    *white = 0;
    for (int i = 0; i < 64 && board_str[i] != '\0' && board_str[i] != '\n'; i++) {
        if (board_str[i] == 'X' || board_str[i] == 'x' || board_str[i] == '*') {
            *black |= (1ULL << i);
        } else if (board_str[i] == 'O' || board_str[i] == 'o') {
            *white |= (1ULL << i);
        }
    }
    
    *color = (turn_str[0] == 'B' || turn_str[0] == 'b') ? BLACK : WHITE;
    return 0;
}

int main(int argc, char *argv[]){
    if (argc < 2) {
        fprintf(stderr, "使用方法: %s <position_file> [time_limit_sec]\n", argv[0]);
        fprintf(stderr, "\nこれはDeepPN (R=%d) の逐次版ソルバーです。\n", R);
        return 1;
    }
    
    const char *pos_file = argv[1];
    if (argc > 2) {
        time_limit_sec = atof(argv[2]);
    }
    
    // 置換表の初期化
    init_hash();
    
    // ポジション読み込み
    bitboard black, white;
    int color;
    
    if (load_position(pos_file, &black, &white, &color) != 0) {
        return 1;
    }
    
    // 空きマス数計算
    int depth = 64 - count_bit(black) - count_bit(white);
    
    printf("----DeepPN Benchmark----\n");
    printf("Position: %s\n", pos_file);
    printf("Empties: %d\n", depth);
    printf("Turn: %s\n", color == BLACK ? "Black" : "White");
    printf("TimeLimit: %.1f sec\n", time_limit_sec);
    printf("R parameter: %d\n", R);
    printf("------------------------\n");
    
    // 測定開始
    clock_gettime(CLOCK_MONOTONIC, &global_start_time);

    // PNS開始
    int result = pns(black, white, color, depth);
    
    // 測定終了
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - global_start_time.tv_sec) + 
                     (end_time.tv_nsec - global_start_time.tv_nsec) * 1e-9;
    
    // 結果出力（パース用フォーマット）
    printf("\n--- Results ---\n");
    if (timeout_flag) {
        printf("Result: TIMEOUT\n");
    } else {
        const char *result_str;
        switch(result) {
            case WIN:  result_str = "WIN"; break;
            case LOSE: result_str = "LOSE"; break;
            case DRAW: result_str = "DRAW"; break;
            default:   result_str = "UNKNOWN"; break;
        }
        printf("Result: %s\n", result_str);
    }
    printf("Time: %.6f\n", elapsed);
    printf("Nodes: %d\n", search_node_num);
    printf("ExpandedNodes: %d\n", node_num);
    printf("NPS: %.0f\n", elapsed > 0 ? search_node_num / elapsed : 0);
    printf("TT_stores: %d\n", store_num);
    printf("TT_hits: %d\n", use_hash_num);
    printf("---------------\n");
    
    return 0;
}

int pns(bitboard black, bitboard white, int color, int depth){
    node_t *node = create_node(black, white, color, depth);
    
    while(1){
        if (check_timeout()) {
            return UNKNOWN;
        }
        
        pns_search(node);
        
        if (is_terminal(node)) {
            break;
        }
    }
    
    if (node->proof >= INF) {
        if (node->disproof >= INF) {
            return DRAW;
        } else {
            return LOSE;
        }
    } else if (node->disproof >= INF) {
        return WIN;
    }
    return UNKNOWN;
}

node_t *create_node(bitboard black, bitboard white, int color, int depth){
    node_num++;
    node_t *node = (node_t*)malloc(sizeof(node_t));
    if (node == NULL) {
        fprintf(stderr, "メモリ確保エラー\n");
        exit(1);
    }

    node->num_node = node_num;
    node->black = black;
    node->white = white;
    node->color = color;
    node->depth = depth;
    node->deep = (float)1/(60-depth);
    node->proof = 1;
    node->disproof = 1;
    node->parent = NULL;
    node->child = NULL;
    node->next = NULL;
    DPN(node);

    return node;
}

void pns_search(node_t *node){
    hash_t *entry;
    
    search_node_num++;
    
    // タイムアウトチェック（1000ノードごと）
    if ((search_node_num & 0x3FF) == 0) {
        if (check_timeout()) return;
    }
    
    if (is_terminal(node)) {
        judge_node(node);
        node->deep = (float)1/(60 - node->depth);
        store_hash(node);
        return;
    }
    
    while(1){
        if (check_timeout()) return;
        
        if (node->child == NULL) {
            generate_children(node);
        }
            
        for (node_t *child = node->child; child != NULL; child = child->next) {
            entry = get_hash(child);
            if (entry != NULL) {
                use_hash_num++;
                child->proof = entry->proof;
                child->disproof = entry->disproof;
                child->dpn = entry->dpn;
            }
            if (is_terminal(child)) {
                judge_node(child);
                child->deep = (float)1/(60-node->depth);
            }
        }
        
        sort_children(node);
        node->deep = node->child->deep;
        
        update_proof_disproof(node);

        if (is_terminal(node)) {
            judge_node(node);
            node->deep = (float)1/(60 - node->depth);
            node->dpn = 100;
            entry = get_hash(node);
            if (entry != NULL) {
                update_hash(node, entry);
            } else {
                store_hash(node);
            }
            return;
        }
        
        // 最良の子ノードを探索
        pns_search(node->child);
        
        if (check_timeout()) return;
    }
}

int is_terminal(node_t *node){
    // 節点の証明数または反証数がINFを越えている場合は証明されている．
    if ((node->proof >= INF && node->disproof == 0) || (node->proof == 0 && node->disproof >= INF)) {
        // 節点下の終端節点が証明されているため1を返す
        return 1;
    }
    // 残りの深さが0の場合
    if (node->depth == 0) {
        return 1;
    }

    // 白と黒がどちらも置けない盤面の場合
    if (puttable(node->black, node->white, node->color) == 0) {
        node->color *= -1; // パス
        if (puttable(node->black, node->white, node->color) == 0) {
            // 終端節点のため1を返す
            return 1;
        }
    }
    // 非終端節点のため0を返す
    return 0;
}

int count_bit(bitboard b){
    return __builtin_popcountll(b);
}

int puttable(bitboard black, bitboard white, int color){
    // 全ての位置を判定
    for (int i = 0; i < 64; i++){
        if (can_put(black, white, i, color)){
            return 1;
        }
    }
    return 0;
}

int count_puttable(bitboard black, bitboard white, int color){
    int count = 0;
    for (int i = 0; i < 64; i++) {
        if (can_put(black, white, i, color)) {
            count++;
        }
    }
    return count;
}

int can_put(bitboard black, bitboard white, int pos, int color){
    // 石が有れば置けない
    if (get_color(black, white, pos) != 0){
        return 0;
    }

    // 相手の色
    int opp = -color;
    // 隣の位置
    int next;
    // 隣の石の色
    int next_color;

    // 判定用のボード
    bitboard self_board, opponent_board;

    // 8方向を調べる
    for (int i = 0; i < 8; i++) {
        // 判定用のボードに情報を入れる
        if (color == BLACK) {
            self_board = black;
            opponent_board = white;
        } else {
            self_board = white;
            opponent_board = black;
        }
        // 斜めと縦の探索では、相手のビットボードの上下両端を空きマスにする．
        if (i > 1) {
            opponent_board = opponent_board & topbottom_HIDE_BIT;
        }
        // 横と斜めの探索では，相手のビットボードの両端の縦1列を空きマスにする．
        if (i < 6) {
            opponent_board = opponent_board & rightleft_HIDE_BIT;
        }
        // 隣の位置を取得
        next = pos + directions[i];
        // 隣の位置が範囲外ならスキップ
        if (next < 0 || next > 63)
            continue;
        // 隣の石の色を取得
        if (color == BLACK) {
            next_color = get_color(self_board, opponent_board, next);
        } else {
            next_color = get_color(opponent_board, self_board, next);
        }
        // 隣の石が相手の色でなければスキップ
        if (next_color != opp)
            continue;
        // 更に隣を調べる
        while (1) {
            // その隣の位置
            next += directions[i];
            // 範囲外チェック
            if (next < 0 || next > 63) {
                break;
            }
            // その隣の石の色
            if (color == 1) {
                next_color = get_color(self_board, opponent_board, next);
            } else {
                next_color = get_color(opponent_board, self_board, next);
            }
            // その隣が自分の石なら置ける
            if (next_color == color) {
                return 1;
            }
            // その隣が空なら置けない
            if (next_color == 0) {
                break;
            }
        }
    }
    // どの方向もひっくり返せないから置けない
    return 0;
}

int judge(bitboard black, bitboard white){
    int black_count = count_bit(black);
    int white_count = count_bit(white);
    
    if (black_count > white_count) return WIN;
    if (white_count > black_count) return LOSE;
    return DRAW;
}

void judge_node(node_t *node){
    if (node->depth <= 0) {
        int result = judge(node->black, node->white);
        if (node->color == BLACK) {
            if (result == WIN) {
                node->proof = 0;
                node->disproof = INF;
            } else if (result == LOSE) {
                node->proof = INF;
                node->disproof = 0;
            } else {
                node->proof = INF;
                node->disproof = INF;
            }
        } else {
            if (result == LOSE) {
                node->proof = 0;
                node->disproof = INF;
            } else if (result == WIN) {
                node->proof = INF;
                node->disproof = 0;
            } else {
                node->proof = INF;
                node->disproof = INF;
            }
        }
    }
}

int get_color(bitboard black, bitboard white, int pos){
    int index = 63 - pos;
    bitboard bit = 1ULL << index;
    
    if (black & bit) return BLACK;
    if (white & bit) return WHITE;
    return 0;
}

void generate_children(node_t *node){
    node_t *first_child = NULL;
    node_t *last_child = NULL;

    int next_color = -node->color;
    int can_move = 0;

    for (int i = 0; i < 64; i++) {
        if (can_put(node->black, node->white, i, node->color)) {
            can_move = 1;

            bitboard new_black = node->black;
            bitboard new_white = node->white;
            set_color(&new_black, &new_white, i, node->color);

            node_t *child = create_node(new_black, new_white, next_color, node->depth - 1);
            child->parent = node;
            
            if (first_child == NULL) {
                first_child = child;
                last_child = child;
            } else {
                last_child->next = child;
                last_child = child;
            }
        }
    }
    
    // パス
    if (!can_move) {
        if (!puttable(node->black, node->white, next_color)) {
            // 両者パス = ゲーム終了
            node->depth = 0;
            judge_node(node);
        } else {
            // 相手に手番を渡す
            node_t *child = create_node(node->black, node->white, next_color, node->depth);
            child->parent = node;
            first_child = child;
        }
    }
    
    node->child = first_child;
}

void update_proof_disproof(node_t *node){
    // 節点が終端節点の場合は何もしない
    if (is_terminal(node)){
        return;
    }
    // 節点が終端節点でない場合
    else {
        node_t *child = node->child;

        // 証明数と反証明数を先頭の子節点の値に設定
        node->proof = child->proof;
        node->disproof = child->disproof;

        // 黒の手番(ORノード)ならば
        if (node->color == BLACK) {
            // 子ノードを繰り返し参照
            for (child = child->next; child != NULL; child = child->next) {
                // 子ノードの証明数が今より小さいとき，最小の証明数を更新
                if (child->proof < node->proof) {
                    node->proof = child->proof;
                }
                // 子ノードの反証明数を合計
                node->disproof += child->disproof;
            }
        }
        // 白の手番(ANDノード)ならば
        else {
            for (child = child->next; child != NULL; child = child->next) {
                // 子ノードの証明数を合計
                node->proof += child->proof;
                // 子ノードの反証数が小さいなら，最小の反証数を更新
                if (child->disproof < node->disproof) {
                    node->disproof = child->disproof;
                }
            }
        }
        if (node->proof > INF) {
            node->proof = INF;
        }
        if (node->disproof > INF) {
            node->disproof = INF;
        }
    }
}

void DPN(node_t *node){
    // Deep Proof Number計算（オリジナル版のアルゴリズム）
    float temp;
    if (node->color == BLACK){
        if (node->disproof >= INF)
            temp = 0.001;
        else
            temp = (float)1/node->disproof;
        node->dpn = (1 - temp) * R + node->deep * (1 - R);
    } else if (node->color == WHITE) {
        if (node->proof >= INF)
            temp = 0.001;
        else
            temp = (float)1/node->proof;
        node->dpn = (1 - temp) * R + node->deep * (1 - R);
    } else {
        printf("node->color is invalid\n");
        exit(1);
    }
}

void set_deep(node_t *node){
    node->deep = (float)1/(60 - node->depth);
}

void sort_children(node_t *node){
    node_t *array[60];
    int size = 0;
    
    node_t *child = node->child;
    while (child != NULL) {
        array[size++] = child;
        child = child->next;
    }
    
    if (size <= 1) return;
    
    quick_sort(array, 0, size-1, node->color);
    solved_sort(array, 0, size-1);
    
    node->child = array[0];
    for (int i = 0; i < size - 1; i++) {
        array[i]->next = array[i+1];
    }
    array[size-1]->next = NULL;
}

void solved_sort(node_t *array[], int left, int right){
    int count = 1;
    node_t *temp;
    
    while (count) {
        count = 0;
        for (int i = 0; i < right; i++) {
            int solved_i = (array[i]->proof == 0 || array[i]->disproof == 0);
            int solved_i1 = (array[i+1]->proof == 0 || array[i+1]->disproof == 0);
            
            if (solved_i && !solved_i1) {
                temp = array[i];
                array[i] = array[i+1];
                array[i+1] = temp;
                count++;
            }
        }
    }
}

void quick_sort(node_t *array[], int left, int right, int color){
    if (left >= right) return;
    
    float pivot = array[(left + right)/2]->dpn;
    int i = left;
    int j = right;
    
    while (i < j) {
        while (array[i]->dpn < pivot) i++;
        while (array[j]->dpn > pivot) j--;
        
        if (i <= j) {
            if (i != j) {
                node_t *temp = array[i];
                array[i] = array[j];
                array[j] = temp;
            }
            i++;
            j--;
        }
    }
    
    quick_sort(array, left, j, color);
    quick_sort(array, i, right, color);
}

void set_color(bitboard *black, bitboard *white, int pos, int color) {
    int index = 63-pos;

    if (color == BLACK) {
        *black |= ((uint64_t)1 << index);
    } else {
        *white |= ((uint64_t)1 << index);
    }
    
    int opp = -color;
    bitboard flip = 0;
    
    for (int i = 0; i < 8; i++) {
        int dir = directions[i];
        int next = pos + dir;
        
        if (next < 0 || next > 63) continue;
        
        bitboard self_board = (color == BLACK) ? *black : *white;
        bitboard opponent_board = (color == BLACK) ? *white : *black;
        
        if (i > 1) {
            opponent_board = opponent_board & topbottom_HIDE_BIT;
        }
        if (i < 6) {
            opponent_board = opponent_board & rightleft_HIDE_BIT;
        }
        
        int next_color;
        if (color == BLACK) {
            next_color = get_color(self_board, opponent_board, next);
        } else {
            next_color = get_color(opponent_board, self_board, next);
        }
        
        if (next_color != opp) continue;
        
        flip = 0;
        flip |= ((uint64_t)1 << (63-next));
        
        while (1) {
            next += dir;
            if (next < 0 || next > 63) {
                flip = 0;
                break;
            }
            
            if (color == BLACK) {
                next_color = get_color(self_board, opponent_board, next);
            } else {
                next_color = get_color(opponent_board, self_board, next);
            }
            
            if (next_color == color) break;
            if (next_color == 0) {
                flip = 0;
                break;
            }
            
            flip |= ((uint64_t)1 << (63-next));
        }
        
        if (flip) {
            *black ^= flip;
            *white ^= flip;
        }
    }
}

void init_hash(){
    for (int i = 0; i < HASH_SIZE; i++) {
        hash_table[i] = NULL;
    }
}

int hash_value(bitboard black, bitboard white){
    uint64_t hash = (black ^ (black >> 32)) + (white ^ (white >> 32));
    return hash % HASH_SIZE;
}

void store_hash(node_t *node){
    int hash = hash_value(node->black, node->white);
    
    hash_t *entry = (hash_t*)malloc(sizeof(hash_t));
    entry->num_node = node->num_node;
    entry->black = node->black;
    entry->white = node->white;
    entry->color = node->color;
    entry->proof = node->proof;
    entry->disproof = node->disproof;
    entry->dpn = node->dpn;
    entry->next = hash_table[hash];
    hash_table[hash] = entry;
    store_num++;
}

void update_hash(node_t *node, hash_t *entry){
    entry->num_node = node->num_node;
    entry->proof = node->proof;
    entry->disproof = node->disproof;
    entry->dpn = node->dpn;
    store_num++;
}

hash_t *get_hash(node_t *node){
    int hash = hash_value(node->black, node->white);
    
    for (hash_t *entry = hash_table[hash]; entry != NULL; entry = entry->next) {
        if (entry->black == node->black && 
            entry->white == node->white && 
            entry->color == node->color) {
            return entry;
        }
    }
    return NULL;
}
