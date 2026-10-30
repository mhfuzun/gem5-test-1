#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Step B: create all pre-warmup SimPoint checkpoints in one Atomic run."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from common import (
    DEFAULT_WARMUP,
    PipelineError,
    canonical_hash,
    executable,
    gem5_command,
    gem5_record,
    load_workload,
    mark_manifest_failed,
    parse_instruction_count,
    prepare_output_directory,
    prepare_stage_workdir,
    read_json,
    run_logged,
    write_json,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", required=True, type=Path)
    parser.add_argument("--profile-manifest", required=True, type=Path)
    parser.add_argument(
        "--warmup", type=parse_instruction_count, default=DEFAULT_WARMUP
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allow-partial-profile", action="store_true")
    parser.add_argument("--allow-gem5-mismatch", action="store_true")
    parser.add_argument("--force", action="store_true")
    return parser


def validate_profile(path: Path, allow_partial: bool) -> dict:
    profile = read_json(path.resolve())
    if profile.get("manifest_type") != "se_simpoint_profile":
        raise PipelineError(f"not a profile manifest: {path}")
    if profile.get("status") != "complete":
        raise PipelineError(f"profile is not complete: {path}")
    if (
        not profile.get("representative_of_full_workload")
        and not allow_partial
    ):
        raise PipelineError(
            "profile is explicitly partial; use --allow-partial-profile only "
            "for smoke testing"
        )
    if not profile.get("selected_points"):
        raise PipelineError("profile contains no selected SimPoints")
    return profile


def run(args: argparse.Namespace) -> Path:
    profile_path = args.profile_manifest.expanduser().resolve()
    profile = validate_profile(profile_path, args.allow_partial_profile)
    gem5 = executable(args.gem5, "gem5 binary")
    gem5_info = gem5_record(gem5)
    profile_commit = profile["tools"]["gem5"].get("repository_commit")
    current_commit = gem5_info.get("repository_commit")
    if (
        profile_commit
        and current_commit
        and profile_commit != current_commit
        and not args.allow_gem5_mismatch
    ):
        raise PipelineError(
            "profiling and checkpoint gem5 commits differ; use the same tree "
            "or explicitly pass --allow-gem5-mismatch"
        )

    source_manifest = Path(profile["workload"]["manifest_path"])
    workload = load_workload(source_manifest)
    if workload["workload_fingerprint"] != profile["workload_fingerprint"]:
        raise PipelineError(
            "workload manifest or source run directory changed after profiling"
        )
    if workload.get("stdin") is not None:
        raise PipelineError(
            "this gem5 revision does not restore the standard-input file "
            "offset from SE checkpoints; stdin workloads are rejected to "
            "prevent a silently shifted instruction stream"
        )

    default_output = (
        profile_path.parent.parent / "checkpoints" / f"warmup_{args.warmup}"
    )
    output_dir = (args.output or default_output).expanduser().resolve()
    prepare_output_directory(output_dir, force=args.force)
    workdir = output_dir / "workdir"
    prepare_stage_workdir(workload, workdir)
    checkpoint_root = output_dir / "checkpoints"
    checkpoint_root.mkdir()
    snapshot_root = output_dir / "filesystem_snapshots"
    snapshot_root.mkdir()
    gem5_output = output_dir / "gem5"
    gem5_output.mkdir()

    interval = profile["interval_instructions"]
    points = []
    for ordinal, selected in enumerate(
        sorted(
            profile["selected_points"],
            key=lambda item: item["interval_index"],
        )
    ):
        simpoint_start = selected["interval_index"] * interval
        actual_warmup = min(args.warmup, simpoint_start)
        checkpoint_instruction = simpoint_start - actual_warmup
        checkpoint_id = (
            f"sp_{ordinal:03d}_interval_{selected['interval_index']:08d}"
        )
        points.append(
            {
                "checkpoint_id": checkpoint_id,
                "ordinal": ordinal,
                "cluster_id": selected["cluster_id"],
                "interval_index": selected["interval_index"],
                "weight": selected["weight"],
                "simpoint_start_instruction": simpoint_start,
                "checkpoint_instruction": checkpoint_instruction,
                "requested_warmup_instructions": args.warmup,
                "actual_warmup_instructions": actual_warmup,
                "checkpoint_stage_workdir": str(workdir),
                "filesystem_snapshot": str(snapshot_root / checkpoint_id),
            }
        )

    runtime_status = output_dir / "runtime_status.json"
    runtime_config = {
        "schema_version": 1,
        "workload": workload,
        "stage_workdir": str(workdir),
        "runtime_status_path": str(runtime_status),
        "system": profile["system"],
        "checkpoint_root": str(checkpoint_root),
        "points": points,
    }
    runtime_config_path = output_dir / "runtime_config.json"
    write_json(runtime_config_path, runtime_config)

    index_manifest = {
        "schema_version": 1,
        "manifest_type": "se_simpoint_checkpoint_set",
        "status": "running",
        "profile_id": profile["profile_id"],
        "profile_manifest": str(profile_path),
        "workload_fingerprint": workload["workload_fingerprint"],
        "requested_warmup_instructions": args.warmup,
        "interval_instructions": interval,
        "system": profile["system"],
        "gem5": gem5_info,
        "points": points,
        "paths": {
            "output_directory": str(output_dir),
            "checkpoint_root": str(checkpoint_root),
            "filesystem_snapshot_root": str(snapshot_root),
            "stage_workdir": str(workdir),
        },
    }
    index_path = output_dir / "checkpoint_set_manifest.json"
    write_json(index_path, index_manifest)

    start_time = time.monotonic()
    command = gem5_command(
        gem5, gem5_output, runtime_config_path, "checkpoint"
    )
    run_logged(command, output_dir / "checkpoint.log")
    elapsed = time.monotonic() - start_time
    if not runtime_status.is_file():
        raise PipelineError("gem5 did not write checkpoint runtime status")
    runtime = read_json(runtime_status)
    if not runtime.get("complete"):
        raise PipelineError(
            "program exited before all checkpoints were created: "
            f"{runtime.get('exit_cause')}"
        )

    observed = {item["checkpoint_id"]: item for item in runtime["created"]}
    manifest_paths = []
    for point in points:
        checkpoint_path = checkpoint_root / point["checkpoint_id"]
        if not (checkpoint_path / "m5.cpt").is_file():
            raise PipelineError(
                f"incomplete checkpoint directory: {checkpoint_path}"
            )
        snapshot_path = Path(point["filesystem_snapshot"])
        if not snapshot_path.is_dir():
            raise PipelineError(
                f"filesystem snapshot is missing: {snapshot_path}"
            )
        if point["checkpoint_id"] not in observed:
            raise PipelineError(
                f"runtime status omitted checkpoint {point['checkpoint_id']}"
            )
        actual_instruction = observed[point["checkpoint_id"]][
            "observed_instruction"
        ]
        instruction_count_offset = (
            actual_instruction - point["checkpoint_instruction"]
        )
        if instruction_count_offset not in (0, 1):
            raise PipelineError(
                f"checkpoint {point['checkpoint_id']} was requested at "
                f"{point['checkpoint_instruction']} but observed at "
                f"{actual_instruction}"
            )
        point["actual_checkpoint_instruction"] = actual_instruction
        point["instruction_count_offset"] = instruction_count_offset
        contract = {
            "profile_id": profile["profile_id"],
            "workload_fingerprint": workload["workload_fingerprint"],
            "system": profile["system"],
            "interval_instructions": interval,
            **point,
        }
        checkpoint_manifest = {
            "schema_version": 1,
            "manifest_type": "se_simpoint_checkpoint",
            "status": "complete",
            **contract,
            "checkpoint_contract_hash": canonical_hash(contract),
            "checkpoint_path": str(checkpoint_path),
            "profile_manifest": str(profile_path),
            "checkpoint_set_manifest": str(index_path),
            "actual_checkpoint_instruction": actual_instruction,
            "instruction_count_offset": instruction_count_offset,
            "gem5": gem5_info,
        }
        manifest_path = checkpoint_path / "checkpoint_manifest.json"
        write_json(manifest_path, checkpoint_manifest)
        manifest_paths.append(str(manifest_path))

    index_manifest.update(
        {
            "status": "complete",
            "runtime": runtime,
            "wall_seconds": elapsed,
            "checkpoint_manifests": manifest_paths,
        }
    )
    write_json(index_path, index_manifest)
    return index_path


def main() -> int:
    args = build_parser().parse_args()
    try:
        output = run(args)
    except (PipelineError, OSError, ValueError, KeyError) as error:
        profile_path = args.profile_manifest.expanduser().resolve()
        default_output = (
            profile_path.parent.parent
            / "checkpoints"
            / f"warmup_{args.warmup}"
        )
        output_dir = (args.output or default_output).expanduser().resolve()
        mark_manifest_failed(
            output_dir / "checkpoint_set_manifest.json", error
        )
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"Checkpoint set complete: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
