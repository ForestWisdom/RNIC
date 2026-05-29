#!/usr/bin/env python3
import csv
import json
import re
import statistics
import sys
from pathlib import Path


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def lane_summary(data):
    values = [lane.get("throughput_mib_s", 0.0) for lane in data.get("lanes", [])]
    if not values:
        return 0.0, 0.0, 0.0
    return min(values), max(values), statistics.mean(values)


def collect(root):
    rows = []
    for sender in sorted(root.glob("**/send-*.json")):
        name = sender.name.replace("send-", "").replace(".json", "")
        receiver = sender.with_name(f"recv-{name}.json")
        if not receiver.exists():
            continue
        s = load(sender)
        r = load(receiver)
        s_min, s_max, s_avg = lane_summary(s)
        r_min, r_max, r_avg = lane_summary(r)
        rows.append({
            "case": sender.parent.name,
            "name": name,
            "engine": s.get("engine", ""),
            "register_buffers": s.get("register_buffers", ""),
            "cpu_list": s.get("cpu_list", ""),
            "rdma_lanes": len(s.get("lanes", [])),
            "depth": s.get("depth", ""),
            "sender_total_mib_s": s.get("throughput_mib_s", 0.0),
            "sender_active_mib_s": s.get("active_throughput_mib_s", 0.0),
            "receiver_total_mib_s": r.get("throughput_mib_s", 0.0),
            "receiver_active_mib_s": r.get("active_throughput_mib_s", 0.0),
            "sender_lane_min": s_min,
            "sender_lane_max": s_max,
            "sender_lane_avg": s_avg,
            "receiver_lane_min": r_min,
            "receiver_lane_max": r_max,
            "receiver_lane_avg": r_avg,
            "sha256_ok": r.get("sha256_ok", ""),
        })
    return rows


def base_case(case_name):
    return re.sub(r"-r[0-9]+$", "", case_name)


def write_csv(rows, path):
    if not rows:
        return
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def write_md(rows, path):
    if rows and "sender_active_mib_s" in rows[0]:
        cols = [
            "case", "engine", "register_buffers", "cpu_list", "rdma_lanes", "depth",
            "sender_active_mib_s", "receiver_active_mib_s", "sha256_ok",
        ]
    else:
        cols = [
            "case", "engine", "register_buffers", "cpu_list", "rdma_lanes", "depth",
            "runs", "sender_active_mean_mib_s", "sender_active_stddev_mib_s",
            "receiver_active_mean_mib_s", "receiver_active_stddev_mib_s",
        ]
    with open(path, "w", encoding="utf-8") as f:
        f.write("| " + " | ".join(cols) + " |\n")
        f.write("| " + " | ".join(["---"] * len(cols)) + " |\n")
        for row in rows:
            f.write("| " + " | ".join(str(row.get(c, "")) for c in cols) + " |\n")


def mean_std(values):
    if not values:
        return 0.0, 0.0
    if len(values) == 1:
        return values[0], 0.0
    return statistics.mean(values), statistics.stdev(values)


def aggregate(rows):
    groups = {}
    for row in rows:
        key = (
            base_case(row["case"]),
            row["engine"],
            row["register_buffers"],
            row["cpu_list"],
            row["rdma_lanes"],
            row["depth"],
        )
        groups.setdefault(key, []).append(row)

    out = []
    for key, group_rows in sorted(groups.items()):
        sender_active_mean, sender_active_std = mean_std(
            [float(r["sender_active_mib_s"]) for r in group_rows]
        )
        receiver_active_mean, receiver_active_std = mean_std(
            [float(r["receiver_active_mib_s"]) for r in group_rows]
        )
        sender_total_mean, sender_total_std = mean_std(
            [float(r["sender_total_mib_s"]) for r in group_rows]
        )
        receiver_total_mean, receiver_total_std = mean_std(
            [float(r["receiver_total_mib_s"]) for r in group_rows]
        )
        out.append({
            "case": key[0],
            "engine": key[1],
            "register_buffers": key[2],
            "cpu_list": key[3],
            "rdma_lanes": key[4],
            "depth": key[5],
            "runs": len(group_rows),
            "sender_active_mean_mib_s": sender_active_mean,
            "sender_active_stddev_mib_s": sender_active_std,
            "receiver_active_mean_mib_s": receiver_active_mean,
            "receiver_active_stddev_mib_s": receiver_active_std,
            "sender_total_mean_mib_s": sender_total_mean,
            "sender_total_stddev_mib_s": sender_total_std,
            "receiver_total_mean_mib_s": receiver_total_mean,
            "receiver_total_stddev_mib_s": receiver_total_std,
        })
    return out


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "results")
    rows = collect(root)
    write_csv(rows, root / "summary.csv")
    write_md(rows, root / "summary.md")
    stats = aggregate(rows)
    write_csv(stats, root / "summary_stats.csv")
    write_md(stats, root / "summary_stats.md")
    print(
        f"wrote {len(rows)} rows and {len(stats)} aggregate rows under {root}"
    )


if __name__ == "__main__":
    main()
