#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${ROOT_DIR:-$HOME/zhangzhisen/RNIC}"
IN_FILE="${IN_FILE:?set IN_FILE to the model weight or test file on 4090-2}"
RESULT_DIR="${RESULT_DIR:-$ROOT_DIR/results}"
MODE="${MODE:-hybrid}"
CHUNK_SIZE="${CHUNK_SIZE:-16777216}"

mkdir -p "$RESULT_DIR"
cd "$ROOT_DIR"

case "$MODE" in
  nic-only)
    exec ./build/rnic_send \
      --input "$IN_FILE" \
      --mode "$MODE" \
      --chunk-size "$CHUNK_SIZE" \
      --lane "nic,10.102.0.239,5001,tcp,eno1np0,1" \
      --results-json "$RESULT_DIR/send-nic-only.json"
    ;;
  rdma-only)
    exec ./build/rnic_send \
      --input "$IN_FILE" \
      --mode "$MODE" \
      --chunk-size "$CHUNK_SIZE" \
      --lane "rdma,192.168.2.248,5002,rc_mlx5+tcp,-,1" \
      --results-json "$RESULT_DIR/send-rdma-only.json"
    ;;
  hybrid)
    exec ./build/rnic_send \
      --input "$IN_FILE" \
      --mode "$MODE" \
      --chunk-size "$CHUNK_SIZE" \
      --lane "nic,10.102.0.239,5001,tcp,eno1np0,30" \
      --lane "rdma,192.168.2.248,5002,rc_mlx5+tcp,-,70" \
      --results-json "$RESULT_DIR/send-hybrid.json"
    ;;
  *)
    echo "unknown MODE=$MODE; expected nic-only, rdma-only, or hybrid" >&2
    exit 2
    ;;
esac
