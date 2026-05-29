#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${ROOT_DIR:-$HOME/zhangzhisen/RNIC}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT_DIR/results/$RUN_ID}"
SIZE="${SIZE:-8589934592}"
RDMA_LANES="${RDMA_LANES:-8}"
DEPTH="${DEPTH:-16}"
CPU_LIST="${CPU_LIST:-0-7}"
REPEAT="${REPEAT:-1}"
REMOTE="${REMOTE:-Cambricon-1}"
OUT_PREFIX="${OUT_PREFIX:-$HOME/zhangzhisen/rnic_bench_recv}"
TEST_FILE="${TEST_FILE:-$HOME/zhangzhisen/rnic_bench_64m.bin}"

mkdir -p "$RESULT_ROOT"
cd "$ROOT_DIR"

cleanup() {
  rm -f "$TEST_FILE"
  ssh "$REMOTE" "rm -f '${OUT_PREFIX}'*.bin '$HOME/zhangzhisen/rnic_bench_64m.bin'" >/dev/null 2>&1 || true
}
trap cleanup EXIT

collect_env_pair() {
  local case_dir="$1"
  mkdir -p "$case_dir"
  ./scripts/collect_env.sh "$case_dir/env-sender.json"
  ssh "$REMOTE" "mkdir -p '$case_dir' && cd '$ROOT_DIR' && MODE='${MODE:-}' RDMA_LANES='${RDMA_LANES:-}' DEPTH='${DEPTH:-}' ENGINE='${ENGINE:-}' REGISTER_BUFFERS='${REGISTER_BUFFERS:-}' CPU_LIST='${CPU_LIST:-}' VERIFY='${VERIFY:-}' SOURCE='${SOURCE:-}' SINK='${SINK:-}' ./scripts/collect_env.sh '$case_dir/env-receiver.json'"
  scp "$REMOTE:$case_dir/env-receiver.json" "$case_dir/env-receiver.json" >/dev/null
}

run_case() {
  local name="$1"
  local engine="$2"
  local register_buffers="$3"
  local cpu_list="$4"
  local repeat="$5"
  local case_dir="$RESULT_ROOT/${name}-r${repeat}"
  mkdir -p "$case_dir"

  MODE=rdma-only RDMA_LANES="$RDMA_LANES" DEPTH="$DEPTH" ENGINE="$engine" \
    REGISTER_BUFFERS="$register_buffers" CPU_LIST="$cpu_list" VERIFY=none SOURCE=zero SINK=1 \
    collect_env_pair "$case_dir"

  ssh "$REMOTE" "cd '$ROOT_DIR' && RESULT_DIR='$case_dir' SINK=1 MODE=rdma-only RDMA_LANES='$RDMA_LANES' DEPTH='$DEPTH' VERIFY=none ENGINE='$engine' REGISTER_BUFFERS='$register_buffers' CPU_LIST='$cpu_list' OUT_FILE='${OUT_PREFIX}-${name}-r${repeat}.bin' ./scripts/run_receiver.sh" &
  local recv_pid=$!
  sleep 2
  RESULT_DIR="$case_dir" MODE=rdma-only RDMA_LANES="$RDMA_LANES" DEPTH="$DEPTH" \
    VERIFY=none SOURCE=zero SIZE="$SIZE" ENGINE="$engine" \
    REGISTER_BUFFERS="$register_buffers" CPU_LIST="$cpu_list" ./scripts/run_sender.sh
  wait "$recv_pid"
  scp "$REMOTE:$case_dir/recv-rdma-only.json" "$case_dir/recv-rdma-only.json" >/dev/null
}

run_correctness() {
  local case_dir="$RESULT_ROOT/correctness"
  mkdir -p "$case_dir"
  dd if=/dev/zero of="$TEST_FILE" bs=1M count=64 status=none
  ssh "$REMOTE" "mkdir -p '$HOME/zhangzhisen'"
  ssh "$REMOTE" "cd '$ROOT_DIR' && RESULT_DIR='$case_dir' SINK=0 MODE=rdma-only RDMA_LANES=1 DEPTH=16 VERIFY=both ENGINE=tag REGISTER_BUFFERS=1 CPU_LIST=0 OUT_FILE='${OUT_PREFIX}-correctness.bin' ./scripts/run_receiver.sh" &
  local recv_pid=$!
  sleep 2
  RESULT_DIR="$case_dir" MODE=rdma-only RDMA_LANES=1 DEPTH=16 VERIFY=both \
    SOURCE=file IN_FILE="$TEST_FILE" ENGINE=tag REGISTER_BUFFERS=1 CPU_LIST=0 ./scripts/run_sender.sh
  wait "$recv_pid"
  scp "$REMOTE:$case_dir/recv-rdma-only.json" "$case_dir/recv-rdma-only.json" >/dev/null
}

run_correctness

for r in $(seq 1 "$REPEAT"); do
  run_case "tag-baseline" tag 0 "" "$r"
  run_case "tag-memh" tag 1 "" "$r"
  run_case "tag-memh-affinity" tag 1 "$CPU_LIST" "$r"
  run_case "rma-memh-affinity" rma 1 "$CPU_LIST" "$r"
done

./scripts/summarize_results.py "$RESULT_ROOT"
echo "results: $RESULT_ROOT"
