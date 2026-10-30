#!/usr/bin/env python3
"""Join SimPoint 3.2 selected intervals and weights into instruction windows."""

import argparse
import csv
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--analysis", required=True)
parser.add_argument("--stats", required=True)
parser.add_argument("--interval", required=True, type=int)
parser.add_argument("--warmup", required=True, type=int)
parser.add_argument("--bbv-intervals", required=True, type=int)
parser.add_argument("--preparation-seconds", required=True, type=float)
parser.add_argument("--profile-seconds", required=True, type=float)
parser.add_argument("--cluster-seconds", required=True, type=float)
args = parser.parse_args()
base = Path(args.analysis)

# SimPoint 3.2 formats: 'interval_index cluster_id' and 'weight cluster_id'.
points = {}
for line in (base / "simpoints.txt").read_text().splitlines():
    if not line.strip():
        continue
    location, cluster_id = line.split()[:2]
    if cluster_id in points:
        raise ValueError(
            f"Duplicate cluster ID in simpoints.txt: {cluster_id}"
        )
    points[cluster_id] = int(location)

weights = {}
for line in (base / "weights.txt").read_text().splitlines():
    if not line.strip():
        continue
    weight, cluster_id = line.split()[:2]
    if cluster_id in weights:
        raise ValueError(f"Duplicate cluster ID in weights.txt: {cluster_id}")
    weights[cluster_id] = float(weight)

if not points or points.keys() != weights.keys():
    raise ValueError("Missing or mismatched SimPoint cluster IDs")

rows = []
for cluster_id, interval_idx in points.items():
    if not (0 <= interval_idx < args.bbv_intervals):
        raise ValueError(f"SimPoint interval out of range: {interval_idx}")
    start = interval_idx * args.interval
    rows.append(
        {
            "cluster": cluster_id,
            "interval_index_0based": interval_idx,
            "roi_start_instruction": start,
            "roi_end_instruction_exclusive": start + args.interval,
            "warmup_start_instruction": max(0, start - args.warmup),
            "weight": weights[cluster_id],
        }
    )
rows.sort(key=lambda r: r["interval_index_0based"])

with (base / "windows.tsv").open("w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=rows[0].keys(), delimiter="\t")
    writer.writeheader()
    writer.writerows(rows)

# Optional gem5 stats: distinguishes simulated seconds from host wall seconds.
gem5_stats = {}
stats_path = Path(args.stats)
if stats_path.is_file():
    for line in stats_path.read_text(errors="replace").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] in (
            "simSeconds",
            "hostSeconds",
            "simInsts",
            "simTicks",
        ):
            try:
                gem5_stats[fields[0]] = float(fields[1])
            except ValueError:
                pass

summary = {
    "gem5_stats": gem5_stats,
    "bbv_interval_instructions": args.interval,
    "complete_bbv_intervals": args.bbv_intervals,
    "complete_bbv_covered_instructions_approx": args.bbv_intervals
    * args.interval,
    "requested_warmup_instructions": args.warmup,
    "num_selected_simpoints": len(rows),
    "sum_weights": sum(weights.values()),
    "preparation_wall_seconds": args.preparation_seconds,
    "atomic_profile_wall_seconds": args.profile_seconds,
    "simpoint_cluster_wall_seconds": args.cluster_seconds,
    "approx_atomic_profile_instructions_per_second": (
        args.bbv_intervals * args.interval / args.profile_seconds
        if args.profile_seconds > 0
        else None
    ),
    "warning": "FS mode BBV includes kernel and other guest execution; offsets are CPU-wide since benchmark-start restore, not per process.",
    "windows": rows,
}
(base / "metadata.json").write_text(json.dumps(summary, indent=2) + "\n")
print(f"Wrote {base / 'windows.tsv'}")
print(f"Saved {len(rows)} SimPoints, weight sum={summary['sum_weights']:.5f}")
for r in rows:
    print(
        f"  cluster {r['cluster']}: {r['roi_start_instruction']:,}"
        f" .. {r['roi_end_instruction_exclusive']:,} inst,"
        f" weight={r['weight']:.4f}, warmup_from={r['warmup_start_instruction']:,}"
    )
