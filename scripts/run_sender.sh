#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${ROOT_DIR:-$HOME/zhangzhisen/RNIC}"
RESULT_DIR="${RESULT_DIR:-$ROOT_DIR/results}"
MODE="${MODE:-hybrid}"
CHUNK_SIZE="${CHUNK_SIZE:-16777216}"
DEPTH="${DEPTH:-16}"
VERIFY="${VERIFY:-chunk}"
SOURCE="${SOURCE:-file}"
SIZE="${SIZE:-0}"
RDMA_LANES="${RDMA_LANES:-1}"

source_args=()
if [ "$SOURCE" = "file" ]; then
  IN_FILE="${IN_FILE:?set IN_FILE to the model weight or test file on 4090-2}"
  source_args+=(--input "$IN_FILE")
elif [ "$SOURCE" = "zero" ]; then
  source_args+=(--source zero --size "$SIZE")
else
  echo "unknown SOURCE=$SOURCE; expected file or zero" >&2
  exit 2
fi

rdma_sender_lanes=()
for ((i = 0; i < RDMA_LANES; i++)); do
  rdma_sender_lanes+=(--lane "rdma${i},192.168.2.248,$((5002 + i)),rc_mlx5+ud_mlx5,mlx5_0:1,1")
done

mkdir -p "$RESULT_DIR"
cd "$ROOT_DIR"

case "$MODE" in
  nic-only)
    exec ./build/rnic_send \
      "${source_args[@]}" \
      --mode "$MODE" \
      --chunk-size "$CHUNK_SIZE" \
      --depth "$DEPTH" \
      --verify "$VERIFY" \
      --lane "nic,10.102.0.239,5001,tcp,eno1np0,1" \
      --results-json "$RESULT_DIR/send-nic-only.json"
    ;;
  rdma-only)
    exec ./build/rnic_send \
      "${source_args[@]}" \
      --mode "$MODE" \
      --chunk-size "$CHUNK_SIZE" \
      --depth "$DEPTH" \
      --verify "$VERIFY" \
      "${rdma_sender_lanes[@]}" \
      --results-json "$RESULT_DIR/send-rdma-only.json"
    ;;
  hybrid)
    exec ./build/rnic_send \
      "${source_args[@]}" \
      --mode "$MODE" \
      --chunk-size "$CHUNK_SIZE" \
      --depth "$DEPTH" \
      --verify "$VERIFY" \
      --lane "nic,10.102.0.239,5001,tcp,eno1np0,15" \
      --lane "rdma,192.168.2.248,5002,rc_mlx5+ud_mlx5,mlx5_0:1,85" \
      --results-json "$RESULT_DIR/send-hybrid.json"
    ;;
  *)
    echo "unknown MODE=$MODE; expected nic-only, rdma-only, or hybrid" >&2
    exit 2
    ;;
esac
