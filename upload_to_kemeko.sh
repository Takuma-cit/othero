#!/bin/bash
# upload_to_kemeko.sh
# kemekoマシンにファイルをアップロードするスクリプト
#
# 使用方法:
#   ./upload_to_kemeko.sh

REMOTE_USER="maelab"
REMOTE_HOST="202.17.18.122"
REMOTE_DIR="~/othello_test"

echo "=============================================="
echo "Upload to kemeko (768 cores)"
echo "=============================================="
echo ""

# アップロードするファイル一覧
FILES=(
    "othello_endgame_solver_hybrid_no_eval.c"
    "wpns_tt_parallel.c"
    "run_test.sh"
    "test_cpu_usage.sh"
    "run_scaling_test.sh"
    "monitor_cpu.sh"
)

# テストポジションディレクトリ
POSITIONS_DIR="test_positions"

echo "=== Files to upload ==="
for f in "${FILES[@]}"; do
    if [ -f "$f" ]; then
        echo "  [OK] $f"
    else
        echo "  [MISSING] $f"
    fi
done

if [ -d "$POSITIONS_DIR" ]; then
    echo "  [OK] $POSITIONS_DIR/"
else
    echo "  [MISSING] $POSITIONS_DIR/"
fi
echo ""

echo "=== Creating remote directory ==="
echo "Running: ssh ${REMOTE_USER}@${REMOTE_HOST} 'mkdir -p ${REMOTE_DIR}'"
ssh ${REMOTE_USER}@${REMOTE_HOST} "mkdir -p ${REMOTE_DIR}"

if [ $? -ne 0 ]; then
    echo "ERROR: Failed to create remote directory"
    echo "Make sure you can connect to kemeko from the current network."
    exit 1
fi
echo "Done."
echo ""

echo "=== Uploading files ==="
for f in "${FILES[@]}"; do
    if [ -f "$f" ]; then
        echo "Uploading: $f"
        scp "$f" ${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}/
    fi
done

echo ""
echo "=== Uploading test positions ==="
if [ -d "$POSITIONS_DIR" ]; then
    scp -r "$POSITIONS_DIR" ${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}/
    echo "Done."
fi

echo ""
echo "=== Setting permissions ==="
ssh ${REMOTE_USER}@${REMOTE_HOST} "chmod +x ${REMOTE_DIR}/*.sh"

echo ""
echo "=============================================="
echo "Upload complete!"
echo "=============================================="
echo ""
echo "To run tests on kemeko:"
echo "  1. ssh ${REMOTE_USER}@${REMOTE_HOST}"
echo "  2. cd ${REMOTE_DIR}"
echo ""
echo "  ★ メインベンチマーク（推奨）:"
echo "  tsp ./run_test.sh --kemeko"
echo ""
echo "  ★ CPU使用率テスト:"
echo "  tsp ./test_cpu_usage.sh 768 120 20   # 768スレッド、120秒、20空き"
echo ""
echo "To check job status: tsp"
echo "To see output: tsp -c [job_id]"
