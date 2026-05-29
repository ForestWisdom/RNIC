#!/usr/bin/env bash
set -euo pipefail

OUT="${1:?usage: collect_env.sh OUT.json}"
RNIC_DEV="${RNIC_DEV:-mlx5_0}"
RNIC_NETDEV="${RNIC_NETDEV:-}"
UCX_ROOT="${UCX_ROOT:-/usr/local/ucx}"

if [ -z "$RNIC_NETDEV" ] && command -v ibdev2netdev >/dev/null 2>&1; then
  RNIC_NETDEV="$(ibdev2netdev 2>/dev/null | awk -v dev="$RNIC_DEV" '$1 == dev && $3 == "==>" {print $4; exit}')"
fi

json_escape() {
  python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'
}

hostname_v="$(hostname)"
kernel_v="$(uname -r)"
git_commit="$(git rev-parse HEAD 2>/dev/null || true)"
ucx_version="$("$UCX_ROOT/bin/ucx_info" -v 2>/dev/null | head -5 | tr '\n' ';' || true)"
numa_node="$(cat "/sys/class/infiniband/$RNIC_DEV/device/numa_node" 2>/dev/null || true)"
ibdev_map="$(ibdev2netdev 2>/dev/null || true)"
speed=""
if [ -n "$RNIC_NETDEV" ] && command -v ethtool >/dev/null 2>&1; then
  speed="$(ethtool "$RNIC_NETDEV" 2>/dev/null | awk -F': ' '/Speed:/ {print $2; exit}')"
fi

cat >"$OUT" <<JSON
{
  "hostname": $(printf '%s' "$hostname_v" | json_escape),
  "kernel": $(printf '%s' "$kernel_v" | json_escape),
  "git_commit": $(printf '%s' "$git_commit" | json_escape),
  "ucx_version": $(printf '%s' "$ucx_version" | json_escape),
  "ucx_root": $(printf '%s' "$UCX_ROOT" | json_escape),
  "rnic_dev": $(printf '%s' "$RNIC_DEV" | json_escape),
  "rnic_netdev": $(printf '%s' "$RNIC_NETDEV" | json_escape),
  "rnic_speed": $(printf '%s' "$speed" | json_escape),
  "numa_node": $(printf '%s' "$numa_node" | json_escape),
  "ibdev2netdev": $(printf '%s' "$ibdev_map" | json_escape),
  "env": {
    "MODE": $(printf '%s' "${MODE:-}" | json_escape),
    "RDMA_LANES": $(printf '%s' "${RDMA_LANES:-}" | json_escape),
    "DEPTH": $(printf '%s' "${DEPTH:-}" | json_escape),
    "ENGINE": $(printf '%s' "${ENGINE:-}" | json_escape),
    "REGISTER_BUFFERS": $(printf '%s' "${REGISTER_BUFFERS:-}" | json_escape),
    "CPU_LIST": $(printf '%s' "${CPU_LIST:-}" | json_escape),
    "VERIFY": $(printf '%s' "${VERIFY:-}" | json_escape),
    "SOURCE": $(printf '%s' "${SOURCE:-}" | json_escape),
    "SINK": $(printf '%s' "${SINK:-}" | json_escape),
    "UCX_TLS": $(printf '%s' "${UCX_TLS:-}" | json_escape),
    "UCX_NET_DEVICES": $(printf '%s' "${UCX_NET_DEVICES:-}" | json_escape)
  }
}
JSON
