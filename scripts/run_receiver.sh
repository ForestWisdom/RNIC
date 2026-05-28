#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${ROOT_DIR:-$HOME/zhangzhisen/RNIC}"
OUT_FILE="${OUT_FILE:-$HOME/zhangzhisen/rnic_received.bin}"
RESULT_DIR="${RESULT_DIR:-$ROOT_DIR/results}"
MODE="${MODE:-hybrid}"
SINK="${SINK:-0}"

sink_args=()
if [ "$SINK" = "1" ]; then
  sink_args+=(--sink)
fi

mkdir -p "$RESULT_DIR"
cd "$ROOT_DIR"

case "$MODE" in
  nic-only)
    exec ./build/rnic_recv \
      --output "$OUT_FILE" \
      --lane "nic,10.102.0.239,5001,tcp,ens11" \
      --results-json "$RESULT_DIR/recv-nic-only.json" \
      "${sink_args[@]}"
    ;;
  rdma-only)
    exec ./build/rnic_recv \
      --output "$OUT_FILE" \
      --lane "rdma,192.168.2.248,5002,rc_mlx5+ud_mlx5,mlx5_0:1" \
      --results-json "$RESULT_DIR/recv-rdma-only.json" \
      "${sink_args[@]}"
    ;;
  hybrid)
    exec ./build/rnic_recv \
      --output "$OUT_FILE" \
      --lane "nic,10.102.0.239,5001,tcp,ens11" \
      --lane "rdma,192.168.2.248,5002,rc_mlx5+ud_mlx5,mlx5_0:1" \
      --results-json "$RESULT_DIR/recv-hybrid.json" \
      "${sink_args[@]}"
    ;;
  *)
    echo "unknown MODE=$MODE; expected nic-only, rdma-only, or hybrid" >&2
    exit 2
    ;;
esac
