#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Combine a complete SimPoint O3 run set into weighted CPI/IPC metrics."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from common import (
    PipelineError,
    canonical_hash,
    read_json,
    write_json,
)


def parse_stats(path: Path) -> dict[str, float]:
    values: dict[str, float] = {}
    contents = path.read_text(encoding="utf-8", errors="replace")
    for line in contents.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[0].startswith("-"):
            continue
        try:
            values[fields[0]] = float(fields[1])
        except ValueError:
            continue
    return values


def unique_suffix(stats: dict[str, float], suffix: str) -> float:
    matches = [value for key, value in stats.items() if key.endswith(suffix)]
    if len(matches) != 1:
        raise PipelineError(
            f"expected one statistic ending in {suffix!r}, found "
            f"{len(matches)}"
        )
    return matches[0]


def run(args: argparse.Namespace) -> Path:
    runs_root = args.runs_root.expanduser().resolve()
    manifests = sorted(runs_root.rglob("run_manifest.json"))
    if not manifests:
        raise PipelineError(f"no run manifests found below {args.runs_root}")
    run_pairs = [(path, read_json(path)) for path in manifests]
    run_pairs = [
        (path, run)
        for path, run in run_pairs
        if run.get("status") == "complete"
    ]
    if not run_pairs:
        raise PipelineError("no complete O3 runs found")

    groups = {}
    group_keys = {}
    for manifest_path, run_manifest in run_pairs:
        candidate_key = {
            "profile_id": run_manifest["profile_id"],
            "o3": run_manifest["o3"],
            "system": run_manifest["system"],
            "measurement_instructions": run_manifest[
                "measurement_instructions"
            ],
            "gem5": run_manifest["gem5"],
        }
        group_id = canonical_hash(candidate_key)[:24]
        groups.setdefault(group_id, []).append((manifest_path, run_manifest))
        group_keys[group_id] = candidate_key
    if args.configuration_id:
        if args.configuration_id not in groups:
            raise PipelineError(
                f"configuration ID not found: {args.configuration_id}; "
                f"available={sorted(groups)}"
            )
        group_id = args.configuration_id
    elif len(groups) != 1:
        raise PipelineError(
            "multiple O3 configurations found; select one with "
            f"--configuration-id. Available: {sorted(groups)}"
        )
    else:
        group_id = next(iter(groups))
    selected = groups[group_id]
    comparison_key = group_keys[group_id]

    checkpoint = read_json(Path(selected[0][1]["checkpoint_manifest"]))
    profile_path = Path(checkpoint["profile_manifest"])
    profile = read_json(profile_path)
    expected_intervals = {
        point["interval_index"] for point in profile["selected_points"]
    }
    actual_intervals = {run["interval_index"] for _, run in selected}
    if actual_intervals != expected_intervals or len(selected) != len(
        actual_intervals
    ):
        missing = sorted(expected_intervals - actual_intervals)
        extra = sorted(actual_intervals - expected_intervals)
        raise PipelineError(
            f"run set is incomplete or duplicated; missing={missing}, "
            f"extra={extra}"
        )

    rows = []
    for manifest_path, run_manifest in sorted(
        selected, key=lambda item: item[1]["interval_index"]
    ):
        stats = parse_stats(Path(run_manifest["paths"]["stats"]))
        instructions = stats.get("simInsts")
        if not instructions:
            raise PipelineError(
                f"simInsts is missing or zero in {manifest_path}"
            )
        cycles = unique_suffix(stats, ".numCycles")
        try:
            mispredictions = unique_suffix(stats, ".mispredicted_0::total")
        except PipelineError:
            mispredictions = unique_suffix(stats, ".bac.branchMisspredict")
        cpi = cycles / instructions
        rows.append(
            {
                "checkpoint_id": run_manifest["checkpoint_id"],
                "interval_index": run_manifest["interval_index"],
                "weight": run_manifest["weight"],
                "instructions": instructions,
                "cycles": cycles,
                "cpi": cpi,
                "ipc": 1.0 / cpi,
                "branch_mispredictions": mispredictions,
                "branch_mpki": mispredictions * 1000.0 / instructions,
                "run_manifest": str(manifest_path),
            }
        )

    weight_sum = sum(row["weight"] for row in rows)
    if abs(weight_sum - 1.0) > 1e-4:
        raise PipelineError(f"selected run weights sum to {weight_sum}, not 1")
    weighted_cpi = sum(row["weight"] * row["cpi"] for row in rows)
    weighted_branch_mpki = sum(
        row["weight"] * row["branch_mpki"] for row in rows
    )
    summary = {
        "schema_version": 1,
        "manifest_type": "se_simpoint_weighted_summary",
        "comparison_id": canonical_hash(comparison_key)[:24],
        "profile_id": comparison_key["profile_id"],
        "configuration": comparison_key,
        "weight_sum": weight_sum,
        "weighted_cpi": weighted_cpi,
        "estimated_ipc": 1.0 / weighted_cpi,
        "weighted_branch_mpki": weighted_branch_mpki,
        "simpoints": rows,
        "disclaimer": (
            "This is a SimPoint estimate for the profiled workload, not an "
            "official SPEC score."
        ),
    }
    output = (
        args.output.expanduser().resolve()
        if args.output
        else args.runs_root.expanduser().resolve() / "weighted_summary.json"
    )
    if output.exists() and not args.force:
        raise PipelineError(f"output already exists: {output}; use --force")
    write_json(output, summary)
    print(
        json.dumps(
            {
                key: summary[key]
                for key in (
                    "comparison_id",
                    "weight_sum",
                    "weighted_cpi",
                    "estimated_ipc",
                    "weighted_branch_mpki",
                )
            },
            indent=2,
        )
    )
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs-root", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--configuration-id")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    try:
        output = run(args)
    except (PipelineError, OSError, KeyError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"Weighted summary: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
