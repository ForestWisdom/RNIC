#!/usr/bin/env bash
set -euo pipefail

IN_FILE="${IN_FILE:?set IN_FILE to the model weight or test file on 4090-2}"
OUT_PREFIX="${OUT_PREFIX:-$HOME/zhangzhisen/rnic_received}"
ROOT_DIR="${ROOT_DIR:-$HOME/zhangzhisen/RNIC}"

for mode in nic-only rdma-only hybrid; do
  echo "== $mode =="
  ssh Cambricon-1 \
    "ROOT_DIR='$ROOT_DIR' MODE='$mode' OUT_FILE='${OUT_PREFIX}-${mode}.bin' '$ROOT_DIR/scripts/run_receiver.sh'" &
  recv_pid=$!
  sleep 3
  MODE="$mode" IN_FILE="$IN_FILE" "$ROOT_DIR/scripts/run_sender.sh"
  wait "$recv_pid"
done
