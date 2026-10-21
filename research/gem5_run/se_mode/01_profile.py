#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Step A: run RISC-V SE/Atomic BBV profiling and SimPoint clustering."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from common import (
    DEFAULT_CLOCK,
    DEFAULT_INTERVAL,
    DEFAULT_MEMORY_SIZE,
    PipelineError,
    canonical_hash,
    count_bbv_intervals,
    executable,
    gem5_command,
    gem5_record,
    load_workload,
    mark_manifest_failed,
    parse_instruction_count,
    parse_simpoint_outputs,
    prepare_output_directory,
    prepare_stage_workdir,
    read_json,
    run_logged,
    tool_record,
    write_json,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", required=True, type=Path)
    parser.add_argument("--workload", required=True, type=Path)
    parser.add_argument("--simpoint-bin", required=True, type=Path)
    parser.add_argument(
        "--results-root",
        type=Path,
        default=Path(__file__).with_name("results"),
    )
    parser.add_argument(
        "--interval", type=parse_instruction_count, default=DEFAULT_INTERVAL
    )
    parser.add_argument("--max-k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--clock", default=DEFAULT_CLOCK)
    parser.add_argument("--memory-size", default=DEFAULT_MEMORY_SIZE)
    parser.add_argument(
        "--max-insts",
        type=parse_instruction_count,
        default=0,
        help=(
            "testing-only profile limit; a limited profile is rejected by "
            "default"
        ),
    )
    parser.add_argument(
        "--allow-partial-profile",
        action="store_true",
        help=(
            "cluster a deliberately truncated profile and mark it "
            "non-representative"
        ),
    )
    parser.add_argument("--force", action="store_true")
    return parser


def run(args: argparse.Namespace) -> Path:
    if args.interval <= 0:
        raise PipelineError("--interval must be positive")
    if args.max_k <= 0:
        raise PipelineError("--max-k must be positive")
    if args.max_insts and not args.allow_partial_profile:
        raise PipelineError(
            "--max-insts truncates the workload; add --allow-partial-profile "
            "only for an explicit smoke test"
        )

    gem5 = executable(args.gem5, "gem5 binary")
    simpoint = executable(args.simpoint_bin, "SimPoint binary")
    workload = load_workload(args.workload)
    profile_dir = (
        args.results_root.expanduser().resolve()
        / workload["workload_id"]
        / "profile"
    )
    prepare_output_directory(profile_dir, force=args.force)
    workdir = profile_dir / "workdir"
    prepare_stage_workdir(workload, workdir)

    runtime_status = profile_dir / "runtime_status.json"
    runtime_config = {
        "schema_version": 1,
        "workload": workload,
        "stage_workdir": str(workdir),
        "runtime_status_path": str(runtime_status),
        "system": {"clock": args.clock, "memory_size": args.memory_size},
        "interval": args.interval,
        "max_insts": args.max_insts,
        "allow_partial_profile": args.allow_partial_profile,
    }
    runtime_config_path = profile_dir / "runtime_config.json"
    write_json(runtime_config_path, runtime_config)

    gem5_info = gem5_record(gem5)
    simpoint_info = tool_record(simpoint)
    simpoint_info["interface"] = "SimPoint 3.2-compatible CLI"
    profile_identity = {
        "workload_fingerprint": workload["workload_fingerprint"],
        "interval": args.interval,
        "system": runtime_config["system"],
        "gem5": gem5_info,
        "max_k": args.max_k,
        "seed": args.seed,
    }
    manifest = {
        "schema_version": 1,
        "manifest_type": "se_simpoint_profile",
        "profile_id": canonical_hash(profile_identity)[:24],
        "status": "running",
        "representative_of_full_workload": False,
        "workload": workload,
        "workload_fingerprint": workload["workload_fingerprint"],
        "interval_instructions": args.interval,
        "system": runtime_config["system"],
        "clustering": {
            "max_k_requested": args.max_k,
            "seed": args.seed,
        },
        "tools": {"gem5": gem5_info, "simpoint": simpoint_info},
        "paths": {
            "profile_directory": str(profile_dir),
            "stage_workdir": str(workdir),
            "runtime_config": str(runtime_config_path),
        },
    }
    manifest_path = profile_dir / "profile_manifest.json"
    write_json(manifest_path, manifest)

    start_time = time.monotonic()
    command = gem5_command(gem5, profile_dir, runtime_config_path, "profile")
    run_logged(command, profile_dir / "profile.log")
    profile_seconds = time.monotonic() - start_time
    if not runtime_status.is_file():
        raise PipelineError("gem5 did not write runtime_status.json")
    runtime = read_json(runtime_status)
    if not runtime.get("program_completed") and not args.allow_partial_profile:
        raise PipelineError(
            "profiling ended before the workload completed: "
            f"{runtime.get('exit_cause')}"
        )

    bbv_path = profile_dir / "simpoint.bb.gz"
    if not bbv_path.is_file() or bbv_path.stat().st_size == 0:
        raise PipelineError(f"gem5 BBV output is missing or empty: {bbv_path}")
    interval_count = count_bbv_intervals(bbv_path)
    if interval_count < 2:
        raise PipelineError(
            f"only {interval_count} complete BBV interval(s); at least two "
            "are required"
        )
    actual_max_k = min(args.max_k, interval_count)

    simpoints_path = profile_dir / "simpoints.txt"
    weights_path = profile_dir / "weights.txt"
    cluster_command = [
        str(simpoint),
        "-loadFVFile",
        str(bbv_path),
        "-inputVectorsGzipped",
        "-maxK",
        str(actual_max_k),
        "-seedkm",
        str(args.seed),
        "-seedproj",
        str(args.seed + 1),
        "-saveSimpoints",
        simpoints_path.name,
        "-saveSimpointWeights",
        weights_path.name,
    ]
    clustering_start = time.monotonic()
    run_logged(
        cluster_command,
        profile_dir / "clustering.log",
        cwd=profile_dir,
    )
    clustering_seconds = time.monotonic() - clustering_start
    if not simpoints_path.is_file() or not weights_path.is_file():
        raise PipelineError(
            "SimPoint did not produce simpoints.txt and weights.txt"
        )
    selected = parse_simpoint_outputs(
        simpoints_path, weights_path, interval_count=interval_count
    )
    for point in selected:
        point["start_instruction"] = point["interval_index"] * args.interval

    manifest.update(
        {
            "status": "complete",
            "representative_of_full_workload": bool(
                runtime["program_completed"]
            ),
            "runtime": runtime,
            "complete_bbv_intervals": interval_count,
            "bbv_covered_instructions": interval_count * args.interval,
            "selected_points": selected,
            "weight_sum": sum(point["weight"] for point in selected),
            "timing_seconds": {
                "atomic_profile": profile_seconds,
                "clustering": clustering_seconds,
            },
        }
    )
    manifest["clustering"].update(
        {
            "max_k_used": actual_max_k,
            "selected_count": len(selected),
            "simpoints_file": str(simpoints_path),
            "weights_file": str(weights_path),
        }
    )
    manifest["paths"].update(
        {
            "bbv": str(bbv_path),
            "simpoints": str(simpoints_path),
            "weights": str(weights_path),
        }
    )
    write_json(manifest_path, manifest)
    return manifest_path


def main() -> int:
    args = build_parser().parse_args()
    try:
        manifest_path = run(args)
    except (PipelineError, OSError, ValueError) as error:
        try:
            workload_id = read_json(args.workload)["workload_id"]
            failed_manifest = (
                args.results_root.expanduser().resolve()
                / workload_id
                / "profile"
                / "profile_manifest.json"
            )
            mark_manifest_failed(failed_manifest, error)
        except (PipelineError, OSError, KeyError):
            pass
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"Profile complete: {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
