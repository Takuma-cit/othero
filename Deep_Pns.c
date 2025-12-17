#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

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
  転置表に要素が散らばり易く、リスト探索が少なくなるので良い。
*/
#define HASH_SIZE 99991

hash_t *hash_table[HASH_SIZE];

// 展開節点数
int node_num = 0;
// 探索節点数
int search_node_num = 0;
// 転置表の保存回数
int store_num = 0;
// 転値表の使用回数
int use_hash_num = 0;

int pns(bitboard black, bitboard white, int color, int depth); // PNSのセットアップ
node_t *create_node(bitboard black, bitboard white, int color, int depth); // 節点作成
void pns_search(node_t *node); // PNSの再帰処理
int is_terminal(node_t *node); // 終端節点の判定
int count_bit(bitboard b); // bit(石数)を数える
int count_puttable(bitboard black, bitboard white, int color); // 置ける場所の数
int puttable(bitboard black, bitboard white, int color);
int can_put(bitboard black, bitboard white, int pos ,int color); // 置けるか判定
int judge(bitboard black, bitboard white); // 勝敗の判定
void judge_node(node_t *node);
int get_color(bitboard black, bitboard white, int pos); // 色の取得
void generate_children(node_t *node); // 子節点を生成
void sort_children(node_t *node);
void quick_sort(node_t *array[], int left, int right,int color);
void update_proof_disproof(node_t *node); // 証明数・反証数を更新
void set_color(bitboard *black, bitboard *white, int pos, int color); // 石を置く
void DPN(node_t *node); // deep proof number の計算
void solved_sort(node_t *array[], int left, int right);
void set_deep(node_t *node);

// 転置表
void init_hash();
int hash_value(bitboard black, bitboard white);
void store_hash(node_t *node);
void update_hash(node_t *node, hash_t *entry);
hash_t *get_hash(node_t *node);

// テスト用
void print_node(node_t *node); // 節点の情報を表示
void print_full_board(bitboard black, bitboard white); // 盤面を表示
void print_bitboard(bitboard b); // bitboardを表示
void print_board(bitboard black, bitboard white);
void print_winner(int result); // 勝敗の表示

int main(){
  puts("----Deep PNS----");
  // 置換表の初期化
  init_hash(hash_table);
  
  // テスト用盤面
  // #40 黒手番
  //bitboard black = 0x0101312303010100;
  //bitboard white = 0x9E7ECEDCFC1E0800;

  // #59
  //bitboard black = 0x00000000010F3331;
  //bitboard white = 0x0000013E3EF00C04;

  //bitboard black = 0xFFFFFFFFFFFF0000;
  //bitboard white = 0x000000000000FF00;

  // 右一列空き
  //bitboard black = 0xFCFCFCFCFCFCFCFC;
  //bitboard white = 0x0202020202020202;

  //bitboard black = 0xFFC0A09088848280;
  //bitboard white = 0x003E5E6E767A7C00;
  //bitboard white = 0x003E5E6E767A7C60;

  // 10マス空き 黒手番 黒必勝盤面
  bitboard black = 0x00000012724A1000;
  bitboard white = 0x3EBDFFED8DB5AF87;

  // 18マス空き
  //bitboard black = 0xFCC0A09088840200;
  //bitboard white = 0x003E5E6E767A7800;

  //bitboard black = 0x003E5E6E767A7C70;
  //bitboard white = 0xFFC0A09088848280;
  
  //bitboard black = 0xFFFEFEFEFEFEF880;
  //bitboard white = 0x0101010101017E; //1マス空き
  //bitboard white = 0x01010101010078; //2マス空き

  //print_full_board(black, white);

  // 初手の手番
  int color = BLACK;
  // 初期盤面からの深さ(空きマス数)
  int depth = 64 - count_bit(black) - count_bit(white);

  int result = 0; //勝敗の結果を保持する変数

  // 測定開始時刻と終了時刻を格納する変数を宣言する
  struct timespec start, end;

  // 測定開始時刻を取得する
  clock_gettime(CLOCK_REALTIME, &start);
  
  //Proof-Number Search 開始
  result = pns(black, white, color, depth);

  // 測定終了時刻を取得する
  clock_gettime(CLOCK_REALTIME, &end);

  // 測定時間を秒単位で計算する
  double elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) * 1e-9;

  printf("経過時間:  %f seconds\n", elapsed);
  printf("展開節点数:%d \n",node_num);
  printf("探索節点数:%d \n",search_node_num);
  print_winner(result);
  printf("転置表の保存数:%d\n",store_num);
  printf("転置表の使用数:%d\n",use_hash_num);

  return 0;
}

/*
  Proof-Number Searchを始める関数
  最初の節点の生成と繰り返し処理の関数呼出を行う
  pns_searchが終了したら勝敗を判定する。
 */
int pns(bitboard black, bitboard white, int color, int depth){
  // make node
  node_t *node = create_node(black, white, color, depth);
  
  int proof_limit = 1;
  int disproof_limit = 1;
  float dpn_limit = 0.1;
  while(1){
    // pnsを行う関数
    pns_search(node);
    // 根節点が証明・反証されたら抜ける
    if (is_terminal(node)) {
      break;
    }

    //printf("dpn_limit: %f\n",dpn_limit);
    
    //print_node(node);
  }

  //printf("color:%d ",node->color);
  //print_node(node);
  
  // 根節点の勝敗を判定
  if (node->proof >= INF) {
    if (node->disproof >= INF) {
      return DRAW;
    } else {
      return LOSE;
    }
  } else if (node->disproof >= INF) {
    return WIN;
  } else {
    puts("Invalid");
  }
}

/*
  節点を作る関数
  1節点分の構造体をメモリ確保
  各メンバ変数に初期値を入れておく
 */
node_t *create_node(bitboard black, bitboard white, int color, int depth){
  //printf("%d \n",node_num);
  node_num++;
  //printf("%d \n",node_num);
  // メモリ確保
  node_t *node = (node_t*)malloc(sizeof(node_t));
  // メモリが確保できないとき
  if (node == NULL) {
    puts("メモリが確保できませんでした。");
    exit(1);
  }

  // メンバを設定
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

  //printf("create "); print_node(node);
  //printf("%f\n",(float)1/(60-depth));
  //printf("depth %d\n",depth);
  // 節点を返す
  return node;
}

/*
  pns探索の再帰処理を行う関数
  
  終端節点であれば盤面をジャッジし、証明数と反証数を設定する。
  先手(黒)の勝ちであれば証明数を0、反証数を∞に設定。
  先手(黒)の敗けであれば証明数を∞、反証数を0に設定。
  引き分けであれば証明数を∞、反証数を∞に設定。

  終端節点でなければ子節点を生成し，子ノードに再帰的にPNSを行う．
  証明数・反証数が閾値以上であれば探索を打ち切る．
 */
void pns_search(node_t *node){
  
  //print_board(node->black, node->white);
  //print_node(node);

  // 転置表のポインタ変数を宣言しておく
  hash_t *entry;
  
  search_node_num++;
  // もし終端節点であれば、勝敗の判定をして証明数・反証数を設定。
  if (is_terminal(node)) {
    judge_node(node);
    node->deep = (float)1/(60 - node->depth);
    // 置換表に値を保存
    store_hash(node);
    return;
  }
  // もし終端節点でなければ
  else {
    while(1){
      // 子節点が無ければ作成
      if (node->child == NULL) {
	generate_children(node);
	//print_node(node->child);
      }
        
      // 各子節点が終端か確認
      for (node_t *child = node->child; child != NULL; child = child->next) {
	//printf("child ");print_node(child);
	// 転置表に節点情報があるか調べる
	
	entry = get_hash(child);
	if (entry != NULL) {
	  use_hash_num++;
	  child->proof = entry->proof;
	  child->disproof = entry->disproof;
	  child->dpn = entry->dpn;
	 }
	// 子節点が終端節点か確認
	if (is_terminal(child)) {
	  judge_node(child);
	  child->deep = (float)1/(60-node->depth);
	  child->deep = 1 / (60 - node->depth);
	  //printf("child ");print_node(child);
	}
	//printf("child  ");print_node(child);
      }
      
      /*
	子節点ををソート
        DPN順,解決済み節点は後ろ側へソート
      */
      //printf("ソート前 ");print_node(node->child);
      sort_children(node);
      //printf("ソート後 ");print_node(node->child);

      // deepをDPNが最小の子節点のdeep値に更新
      node->deep = node->child->deep;
      

      float temp = node->dpn;
      int temp_proof = node->proof;
      int temp_disproof = node->disproof;
      //printf("更新前 "); print_node(node);
      update_proof_disproof(node);
      
      //printf("更新後 "); print_node(node);

      // 節点の真偽値が確定したら親節点へ戻る
      if (is_terminal(node)) {
	judge_node(node);
	node->deep = (float)1/(60 - node->depth);
	// dpn
	node->dpn = 100;
	entry = get_hash(node);
	if (entry != NULL) {
	  // 転置表の値を更新
	  update_hash(node,entry);
	} else {
        // 転置表に値を保存
	store_hash(node);
	}
	// 探索打ち切り
	return;
      }

      //print_node(node);

      // dpn
      DPN(node);
      
      // 更新前より深層証明数が大きくなったら親節点へ戻る
      if (node->dpn > temp) {
        entry = get_hash(node);
	if (entry != NULL) {
	  // 転置表の値を更新
	  update_hash(node,entry);
	} else {
        // 転置表に値を保存
	  store_hash(node);
	}
	// 探索打ち切り
	return;
      }
	 
      // DPS値が最小の未解な子節点を探索
      pns_search(node->child);
      //puts("test");
    }
  }
}

/*
  終端節点の判別をする関数
  
  終端判定の条件は次の通り
  ●証明数と反証数が0:INFもしくはINF:0の組み合わせになっており、証明されている。
  ●残りの深さが0 (空きマス無し)
  ●白と黒がどちらも置けない

  終端節点の場合は、石の数を数えて節点の評価値を更新した後に親節点に終端節点であると返す。
  親節点では子節点から0が返ってくれば非終端節点、1が返ってくれば終端節点と見なす。
 */
int is_terminal(node_t *node){
  // 節点の証明数または反証数がINFを越えている場合は証明されている．
  if ((node->proof >= INF && node->disproof == 0)|| (node->proof == 0 && node->disproof >=INF)) {
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
    //puts("a");
    if (puttable(node->black, node->white, node->color) == 0) {
      //puts("b");
      // 終端節点のため1を返す
      return 1;
    }
  }
  // 非終端節点のため1を返す
  return 0;
}

/*
  bitboardに立っているbitを数える関数
  ビットシフトで1桁ずらし、ビットが立ってるならカウントを繰り返す
  戻り値はビット数
 */
int count_bit(bitboard b) {
  // 石の数
  int count = 0;
  // すべてのビットについて調べる
  for (int i = 0; i < 64; i++) {
    // ビットが立っていればカウントする
    if (b & ((uint64_t)1 << i)) {
      count++;
    }
  }
  return count;
}

/*
  石を置ける場所を数える関数
  石が置けるか判定するcan_put関数を使って全ての位置を判定して数える
 */
int count_puttable(bitboard black, bitboard white, int color){
  // 置ける場所の数を宣言&初期化
  int count = 0;
  // 全ての位置を判定
  for (int i = 0; i < 64; i++){
    if (can_put(black, white, i, color)){
      count++;
    }
  }
  return count;
}

/*
  置ける場所があるかだけ調べるcount_puttableの簡略化版
 */
int puttable(bitboard black, bitboard white, int color){
  // 全ての位置を判定
  for (int i = 0; i < 64; i++){
    if (can_put(black, white, i, color)){
      return 1;
    }
  }
  return 0;
}

/*
  石が置けるか判定する関数
  引数は黒のbitboard、白のbitboard、調べたいマスの位置、手番
  置けないなら0を返し、置けるなら1を返す。
  隣合う石が反転できるかの判定は判定用の値を書き換えて良いbitboardを用意して判定する．
 */
int can_put(bitboard black, bitboard white, int pos ,int color){
  // 石が有れば置けない
  if (get_color(black, white, pos) != 0){
    //printf("pos: %d\n",pos);
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
    //printf("next: %d, pos: %d, directions: %d\n",next,pos,directions[i]);
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

/*
  勝敗を判定する関数
  黒と白のbitboardに立っているビット数をcount_bit関数で数える．
  黒が多いなら1、白が多いなら-1、同点なら0を返す
 */
int judge(bitboard black, bitboard white) {
  // 黒と白の石の数を数える
  int black_count = count_bit(black);
  int white_count = count_bit(white);
  // 石の数が多い方が勝ち
  if (black_count > white_count) {
    return BLACK;
  } else if (black_count < white_count) {
    return WHITE;
  } else if (black_count == white_count){
    return DRAW;
  } else {
    return -2;
  }
}

void judge_node(node_t *node){
  if ((node->proof >= INF && node->disproof==0) || (node->proof == 0 && node->disproof >= INF))
    return;
  
  // 盤面をジャッジして勝敗を求める
  switch (judge(node->black, node->white)) {
    // 節点の値に応じて証明数・反証数を設定
  case BLACK: // 黒の勝ちの場合
    node->proof = 0;
    node->disproof = INF;
    return;
  case WHITE: // 黒の敗けの場合
    node->proof = INF;
    node->disproof = 0;
    return;
  case DRAW: // 引き分け
    //print_board(node->black,node->white);
      node->proof = INF;
      node->disproof = 0;

    return;
  default: // 異常
    printf("異常終了\n");
    exit(1);
  }
}


/*
  石の色を取得する関数
  最上位ビットからpos番目の位置にあるbitを参照する
  黒なら1、白なら-1、空なら0を返す
*/
int get_color(bitboard black, bitboard white, int pos){
  pos = 63 - pos;
  int b_bit = (black >> pos) & 1;
  int w_bit = (white >> pos) & 1;
  if (b_bit == 1){ // 黒のビットボードにビットが立ってるなら黒
    return BLACK;
  } else if (w_bit == 1){ // 白のビットボードにビットが立ってるなら白
    return WHITE;
  } else{ // どちらにもビットが立ってないなら空
    return 0;
  }
}

/*
  節点の証明数と反証明数を更新する関数
  節点が終端節点の場合は更新することが無いので更新しない
  節点の証明数・反証数は以下のように更新
  黒の手番(OR節点)
   証明数は子節点の証明数の最小値
   反証数は子節点の反証数の合計
  白の手番(AND節点)
   証明数は子節点の証明数の合計
   反証数は子節点の反証数の最小値
 */
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

// Deep Proof-Numberを計算する関数
void DPN(node_t *node){
  float temp;
  if (node->color == BLACK){
    if (node->disproof >= INF)
      temp = 0.001;
    else
      temp = (float)1/node->disproof;
    node->dpn = (1 - temp) * R + node->deep * (1 - R);
    //print_node(node);
  } else if (node->color == WHITE) {
    if (node->proof >= INF)
      temp = 0.001;
    else
      temp = (float)1/node->proof;
    node->dpn = (1 - temp) * R + node->deep * (1 - R);
  } else {
    printf("node->color is invalit");
    exit(1);
  }
}

/*
  子節点を生成する関数
  受け取った節点の盤面から着手後の盤面を生成
  生成した盤面、次の手番、1減らした残りの深さを登録した子節点を生成
 */
void generate_children(node_t *node){
  // 置ける場所があれば、試す
  int color;
  for (int i = 0; i < 64; i++) {
    color = node->color;
    // 置けるか判定する
    if (can_put(node->black, node->white, i, node->color)) {
      // 置く前の盤面をコピー
      bitboard new_black = node->black;
      bitboard new_white = node->white;
      
      // 石を置く
      set_color(&new_black, &new_white, i, color);

      // 生成した盤面を子ノードに登録
      node_t *child = create_node(new_black, new_white, color*(-1), node->depth-1);
      // 子節点の親節点を登録
      child->parent = node;
      // 子節点をリストに登録
      child->next = node->child;
      node->child = child;
      
      //printf("node->child "); print_node(node->child);
    }
  }
}

/*
  子節点をソートする関数
  ソート用にポインタを並べた配列を準備
 */
void sort_children(node_t *node){
  // ソート用の配列
  node_t *array[60];
  // 要素数を数える変数
  int size = 0;
  // 子節点の先頭のポインタ変数
  node_t *child = node->child;
  // 配列に子節点のポインタを格納
  while (child != NULL) {
    array[size] = child;
    size++;
    child = child->next;
  }
  //for (int i = 0; i < size; i++){
  //printf("child "); print_node(array[i]);
  //}
  quick_sort(array, 0, size-1, node->color);
  //print_node(array[0]);
  solved_sort(array, 0, size-1);
  //print_node(array[0]);

  node->child = array[0];
  //printf("c_sorted ");
  //print_node(node->child);
  for (int i = 0; i < size - 1; i++) {
    array[i]->next = array[i+1];
    //printf("child%d ",i); print_node(array[i]);
  }
  //printf("child%d ",size-1); print_node(array[size-1]);
  array[size-1]->next = NULL;
}

// 子節点を解かれた節点と未解の節点に分ける関数。未解を左側に寄せる。
void solved_sort(node_t *array[], int left, int right){
  int count=1;
  int i;
  node_t *temp;
  while (count){
    count = 0;
    for(i = 0; i < right; i++){
      if((array[i]->proof == 0 || array[i]->disproof == 0) &&
	 (array[i+1]->proof == 0 || array[i+1]->disproof == 0) != 1){
	temp = array[i];
	array[i] = array[i+1];
	array[i+1] = temp;
	count++;
      }
    }
  }
}

void quick_sort(node_t *array[], int left, int right,int color){
  if (left >= right) {
    return;
  }

  float pivot = array[(left + right)/2]->dpn;
  int i = left;
  int j = right;
  while (i < j) {
    while (array[i]->dpn < pivot) {
      i++;
    }
    while (array[j]->dpn > pivot) {
      j--;
    }
    if (i <= j) {
      if (array[i]->dpn != array[j]->dpn){
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

// 石を置いて相手の石を反転する関数
void set_color(bitboard *black, bitboard *white, int pos, int color) {
  int index = 63-pos;

  // pos番目(index桁目)の対応するビットボードにビットを立たせる
  if (color == BLACK) {
    *black |= ((uint64_t)1 << (index));
    //*white &= ~(1 << (index));
  } else if (color == WHITE) {
    //*black &= ~(1 << (index));
    *white |= ((uint64_t)1 << (index));
  } 
  

  // ひっくり返す石の色
  int opp = -color;
  // ひっくり返す石の位置を記憶するビットボード
  bitboard flip = 0;

  int dir, next, next_color; // for文内で使う変数を宣言
  bitboard self_board, opponent_board;
  
  // 8方向について調べる
  for (int i = 0; i < 8; i++) {
    // 方向
    dir = directions[i];
    // 隣の位置
    next = pos + dir;
    // 隣が範囲外なら飛ばす
    if (next < 0 || next > 63)
      continue;
    // 判定用のボードに情報を入れる
    if (color == BLACK) {
      self_board = *black;
      opponent_board = *white;
    } else {
      self_board = *white;
      opponent_board = *black;
    }

    // 斜めと縦の探索では、相手のビットボードの上下両端を空きマスにする．
    if (i > 1) {
      opponent_board = opponent_board & topbottom_HIDE_BIT;
    }
    // 横と斜めの探索では，相手のビットボードの両端の縦1列を空きマスにする．
    if (i < 6) {
     opponent_board = opponent_board & rightleft_HIDE_BIT;
    } 
    
    // 隣の石の色
    if (color == BLACK) {
      next_color = get_color(self_board, opponent_board, next);
    } else {
      next_color = get_color(opponent_board, self_board, next);
    }
    
    // 隣が相手の石でなければスキップ
    if (next_color != opp) {
      continue;
    }
    
    // flipボードの初期化
    flip = 0;
    // 隣の石を仮にひっくり返す
    // next番目のビットに1を立てて，その値とflipの論理和(OR)を求める．
    flip |= ((uint64_t)1 << (63-next));
    // さらにその隣を調べる
    while (1) {
      // その隣の位置
      next += dir;
      // その隣の石の色
      if (color == 1) {
	next_color = get_color(self_board, opponent_board, next);
      } else {
	next_color = get_color(opponent_board, self_board, next);
      }
      // その隣が自分の石ならひっくり返す
      if (next_color == color) {
	break;
      }
      // その隣が空ならひっくり返さない
      if (next_color == 0) {
	flip = 0;
	break;
      }
      
      // その隣が相手の石なら仮にひっくり返す
      flip |= ((uint64_t)1 << (63-next));
    }
    // ひっくり返す石があればビットボードを更新する
    if (flip) {
      *black ^= flip;
      *white ^= flip;
    }
  } 
}

void print_node(node_t *node){
  printf("nodeNum:%d proof:%d disproof:%d depth:%d deep:%f dpn:%f color:%d\n", node->num_node, node->proof, node->disproof, node->depth, node->deep, node->dpn, node->color);
  //printf("proof   : %d\n",node->proof);
  //printf("disproof: %d\n",node->disproof);
}

// 各bitboardと盤面を表示
void print_full_board(bitboard black, bitboard white){
  puts("--黒のbitboard--");
  print_bitboard(black);

  puts("--白のbitboard--");
  print_bitboard(white);
  
  print_board(black,white);
}

/*
  ビットボードを表示する関数
  行番号を数字，列番号をアルファベットで表す．
  ビットの桁が大きい順から左上に表示していき，最後の桁が右下にくる．
  例:2x2の0110
    a b
  1 0 1
  2 1 0
 */
void print_bitboard(bitboard b){
  
  // 盤面の上端に座標を表示する
  printf("  a b c d e f g h\n");
  
  // 8行分のループを回す
  for (int i = 0; i < 8; i++) {
    
    // 行番号を表示する
    printf("%d ", i + 1);
    
    // 8列分のループを回す
    for (int j = 0; j < 8; j++) {
      // bitboardの最上位ビットを取り出す
      // ビットが1ならば1、0ならば0を表示する
      if (b >> 63 == 1) {
	printf("1 ");
      } else {
	printf("0 ");
      }
      
      // bitboardを左に1ビットずらす
      b = b << 1;
    }
    
    // 改行する
    printf("\n");
  }
}

// 盤面を表示
void print_board(bitboard black, bitboard white){
  puts("--盤面の状態--");
  for(int i = 0; i < 64; i++){
    int color = get_color(black, white, i);
    if(color == BLACK){
      printf(" B");

    } else if(color == WHITE){
      printf(" W");
    } else{
      printf(" .");
    }
    // 行末なら改行
    if(i % 8 == 7){
      puts(" ");
    }
  }
}

void print_winner(int result){
  switch ( result ){
  case 1:
    puts("BLACK");
    break;
  case -1:
    puts("WHITE");
    break;
  case 0:
    puts("Draw");
    break;
  default:
    puts("Invalid");
  }
}

// 転置表の初期化
void init_hash(){
  for (int i = 0; i < HASH_SIZE; i++) {
    // 転置表の全ての要素をNULLにする
    hash_table[i] = NULL;
  }
}

/*
  ハッシュキーを計算する関数
  ビットボードハッシュ法を用いる
  ビットボードの下位32bitと上位32bitの排他的論理和を使う
*/
int hash_value(bitboard black, bitboard white){
  double hash = (double)(black ^ (black >> 32));
  hash += (double)(white ^ (white >> 32));
  
  // ハッシュテーブルのサイズで剰余を取る
  hash = fmod(hash,HASH_SIZE);
  
  return hash;
}

/*
  転置表に値を保存する関数
  ハッシュ値を計算した後にメモリを確保し、各値を保存する。
  保存した構造帯のメモリ番地を転置表の先頭に追加する．
 */
void store_hash(node_t *node){
  // ハッシュ値を計算する
  int hash = hash_value(node->black, node->white);
  // 転置表のメモリを確保
  hash_t *entry = (hash_t*)malloc(sizeof(hash_t));
  // 転置表に各種値を保存
  entry->num_node = node->num_node;
  entry->black = node->black;
  entry->white = node->white;
  entry->color = node->color;
  entry->proof = node->proof;
  entry->disproof = node->disproof;
  entry->dpn = node->dpn;

  //entry->proof_limit = proof_limit;
  //entry->disproof_limit = disproof_limit;
  

  // 転置表の先頭に追加
  entry->next = hash_table[hash];
  hash_table[hash] = entry;
  store_num++;
}

// 古い転置表の値を更新する関数
void update_hash(node_t *node, hash_t *entry){
  // 転置表に各種値を保存
  entry->num_node = node->num_node;
  entry->black = node->black;
  entry->white = node->white;
  entry->color = node->color;
  entry->proof = node->proof;
  entry->disproof = node->disproof;
  entry->dpn = node->dpn;

  // 保存回数カウント
  store_num++;
}

/*
  転置表から該当するリストを探して返す関数
  ハッシュ値を計算した後、転置表の該当するハッシュ値のリスト構造を探索する．
  ヒットすれば転置表の値を返す．
  ヒットしなければNULLを返す．
 */
hash_t *get_hash(node_t *node){
  // ハッシュ値を計算する
  int hash = hash_value(node->black, node->white);
  // 転置表の要素を探索する
  for (hash_t *entry = hash_table[hash]; entry != NULL; entry = entry->next) {
    // ビットボードが等しければ要素を返す
    if (entry->black == node->black && entry->white == node->white && entry->color == node->color) {
      return entry;
    }
  }
  // 転置表に無ければNULL
  return NULL;
}
