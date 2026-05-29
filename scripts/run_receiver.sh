#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${ROOT_DIR:-$HOME/zhangzhisen/RNIC}"
OUT_FILE="${OUT_FILE:-$HOME/zhangzhisen/rnic_received.bin}"
RESULT_DIR="${RESULT_DIR:-$ROOT_DIR/results}"
MODE="${MODE:-hybrid}"
SINK="${SINK:-0}"
DEPTH="${DEPTH:-16}"
VERIFY="${VERIFY:-chunk}"
RDMA_LANES="${RDMA_LANES:-1}"
ENGINE="${ENGINE:-tag}"
REGISTER_BUFFERS="${REGISTER_BUFFERS:-0}"
CPU_LIST="${CPU_LIST:-}"

sink_args=()
if [ "$SINK" = "1" ]; then
  sink_args+=(--sink)
fi

extra_args=(--engine "$ENGINE")
if [ "$REGISTER_BUFFERS" = "1" ]; then
  extra_args+=(--register-buffers)
fi
if [ -n "$CPU_LIST" ]; then
  extra_args+=(--cpu-list "$CPU_LIST")
fi

rdma_receiver_lanes=()
for ((i = 0; i < RDMA_LANES; i++)); do
  rdma_receiver_lanes+=(--lane "rdma${i},192.168.2.248,$((5002 + i)),rc_mlx5+ud_mlx5,mlx5_0:1")
done

mkdir -p "$RESULT_DIR"
cd "$ROOT_DIR"

case "$MODE" in
  nic-only)
    exec ./build/rnic_recv \
      --output "$OUT_FILE" \
      --lane "nic,10.102.0.239,5001,tcp,ens11" \
      --depth "$DEPTH" \
      --verify "$VERIFY" \
      "${extra_args[@]}" \
      --results-json "$RESULT_DIR/recv-nic-only.json" \
      "${sink_args[@]}"
    ;;
  rdma-only)
    exec ./build/rnic_recv \
      --output "$OUT_FILE" \
      "${rdma_receiver_lanes[@]}" \
      --depth "$DEPTH" \
      --verify "$VERIFY" \
      "${extra_args[@]}" \
      --results-json "$RESULT_DIR/recv-rdma-only.json" \
      "${sink_args[@]}"
    ;;
  hybrid)
    exec ./build/rnic_recv \
      --output "$OUT_FILE" \
      --lane "nic,10.102.0.239,5001,tcp,ens11" \
      --lane "rdma,192.168.2.248,5002,rc_mlx5+ud_mlx5,mlx5_0:1" \
      --depth "$DEPTH" \
      --verify "$VERIFY" \
      "${extra_args[@]}" \
      --results-json "$RESULT_DIR/recv-hybrid.json" \
      "${sink_args[@]}"
    ;;
  *)
    echo "unknown MODE=$MODE; expected nic-only, rdma-only, or hybrid" >&2
    exit 2
    ;;
esac
