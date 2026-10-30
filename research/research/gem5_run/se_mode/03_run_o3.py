#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Step C: restore, warm O3, then measure committed instructions."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from common import (
    PipelineError,
    canonical_hash,
    executable,
    gem5_command,
    gem5_record,
    load_workload,
    mark_manifest_failed,
    parse_instruction_count,
    parse_key_value,
    prepare_output_directory,
    prepare_stage_workdir,
    read_json,
    run_logged,
    sha256_file,
    write_json,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", required=True, type=Path)
    parser.add_argument("--checkpoint-manifest", required=True, type=Path)
    parser.add_argument(
        "--measurement",
        type=parse_instruction_count,
        help="defaults to the BBV interval stored in the checkpoint",
    )
    parser.add_argument("--clock")
    parser.add_argument("--l1i-size", default="32KiB")
    parser.add_argument("--l1d-size", default="32KiB")
    parser.add_argument("--l2-size", default="256KiB")
    parser.add_argument("--branch-predictor")
    parser.add_argument(
        "--cpu-param", action="append", default=[], metavar="NAME=VALUE"
    )
    parser.add_argument(
        "--bp-param", action="append", default=[], metavar="NAME=VALUE"
    )
    parser.add_argument(
        "--configuration-module",
        type=Path,
        help="optional Python file defining configure(board, processor, ...)",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allow-gem5-mismatch", action="store_true")
    parser.add_argument("--force", action="store_true")
    return parser


def validate_checkpoint(path: Path) -> dict:
    checkpoint = read_json(path.resolve())
    if checkpoint.get("manifest_type") != "se_simpoint_checkpoint":
        raise PipelineError(f"not a checkpoint manifest: {path}")
    if checkpoint.get("status") != "complete":
        raise PipelineError(f"checkpoint is not complete: {path}")
    checkpoint_path = Path(checkpoint["checkpoint_path"])
    if not (checkpoint_path / "m5.cpt").is_file():
        raise PipelineError(f"checkpoint data is missing: {checkpoint_path}")
    contract = {
        key: checkpoint[key]
        for key in (
            "profile_id",
            "workload_fingerprint",
            "system",
            "interval_instructions",
            "checkpoint_id",
            "ordinal",
            "cluster_id",
            "interval_index",
            "weight",
            "simpoint_start_instruction",
            "checkpoint_instruction",
            "actual_checkpoint_instruction",
            "instruction_count_offset",
            "requested_warmup_instructions",
            "actual_warmup_instructions",
            "checkpoint_stage_workdir",
            "filesystem_snapshot",
        )
    }
    if canonical_hash(contract) != checkpoint.get("checkpoint_contract_hash"):
        raise PipelineError("checkpoint manifest contract hash does not match")
    if not Path(checkpoint["filesystem_snapshot"]).is_dir():
        raise PipelineError("checkpoint filesystem snapshot is missing")
    return checkpoint


def run(args: argparse.Namespace) -> Path:
    checkpoint_manifest_path = args.checkpoint_manifest.expanduser().resolve()
    checkpoint = validate_checkpoint(checkpoint_manifest_path)
    profile_path = Path(checkpoint["profile_manifest"])
    profile = read_json(profile_path)
    if profile.get("profile_id") != checkpoint["profile_id"]:
        raise PipelineError("checkpoint and profile IDs do not match")

    source_manifest = Path(profile["workload"]["manifest_path"])
    workload = load_workload(source_manifest)
    if workload["workload_fingerprint"] != checkpoint["workload_fingerprint"]:
        raise PipelineError(
            "workload manifest or source run directory changed after "
            "checkpointing"
        )

    gem5 = executable(args.gem5, "gem5 binary")
    gem5_info = gem5_record(gem5)
    checkpoint_commit = checkpoint["gem5"].get("repository_commit")
    current_commit = gem5_info.get("repository_commit")
    if (
        checkpoint_commit
        and current_commit
        and checkpoint_commit != current_commit
        and not args.allow_gem5_mismatch
    ):
        raise PipelineError(
            "checkpoint and O3 gem5 commits differ; use the same tree or "
            "explicitly pass --allow-gem5-mismatch"
        )

    measurement = args.measurement or checkpoint["interval_instructions"]
    if measurement <= 0:
        raise PipelineError("measurement instruction count must be positive")
    cpu_params = parse_key_value(args.cpu_param, option="--cpu-param")
    bp_params = parse_key_value(args.bp_param, option="--bp-param")
    configuration_module = None
    configuration_module_sha256 = None
    if args.configuration_module:
        configuration_module = args.configuration_module.expanduser().resolve()
        if not configuration_module.is_file():
            raise PipelineError(
                f"configuration module does not exist: {configuration_module}"
            )
        configuration_module_sha256 = sha256_file(configuration_module)

    o3_config = {
        "clock": args.clock or checkpoint["system"]["clock"],
        "cache": {
            "l1i_size": args.l1i_size,
            "l1d_size": args.l1d_size,
            "l2_size": args.l2_size,
        },
        "branch_predictor": args.branch_predictor,
        "branch_predictor_params": bp_params,
        "cpu_params": cpu_params,
        "configuration_module": (
            str(configuration_module) if configuration_module else None
        ),
        "configuration_module_sha256": configuration_module_sha256,
    }
    run_identity = {
        "checkpoint_contract_hash": checkpoint["checkpoint_contract_hash"],
        "measurement_instructions": measurement,
        "o3": o3_config,
        "gem5": gem5_info,
    }
    configuration_id = canonical_hash(run_identity)[:24]
    default_output = (
        profile_path.parent.parent
        / "o3_runs"
        / checkpoint["checkpoint_id"]
        / configuration_id
    )
    output_dir = (args.output or default_output).expanduser().resolve()
    prepare_output_directory(output_dir, force=args.force)
    workdir = output_dir / "workdir"
    prepare_stage_workdir(
        workload,
        workdir,
        source_override=Path(checkpoint["filesystem_snapshot"]),
    )

    runtime_status = output_dir / "runtime_status.json"
    runtime_config = {
        "schema_version": 1,
        "workload": workload,
        "stage_workdir": str(workdir),
        "runtime_status_path": str(runtime_status),
        "system": {
            "clock": o3_config["clock"],
            "memory_size": checkpoint["system"]["memory_size"],
        },
        "o3": o3_config,
        "checkpoint_path": checkpoint["checkpoint_path"],
        "warmup_instructions": checkpoint["actual_warmup_instructions"],
        "measurement_instructions": measurement,
    }
    runtime_config_path = output_dir / "runtime_config.json"
    write_json(runtime_config_path, runtime_config)

    run_manifest = {
        "schema_version": 1,
        "manifest_type": "se_simpoint_o3_run",
        "status": "running",
        "configuration_id": configuration_id,
        "checkpoint_id": checkpoint["checkpoint_id"],
        "checkpoint_manifest": str(checkpoint_manifest_path),
        "checkpoint_contract_hash": checkpoint["checkpoint_contract_hash"],
        "profile_id": checkpoint["profile_id"],
        "workload_fingerprint": workload["workload_fingerprint"],
        "interval_index": checkpoint["interval_index"],
        "weight": checkpoint["weight"],
        "warmup_instructions": checkpoint["actual_warmup_instructions"],
        "measurement_instructions": measurement,
        "measurement_matches_bbv_interval": (
            measurement == checkpoint["interval_instructions"]
        ),
        "o3": o3_config,
        "system": runtime_config["system"],
        "gem5": gem5_info,
        "paths": {
            "output_directory": str(output_dir),
            "stage_workdir": str(workdir),
            "runtime_config": str(runtime_config_path),
            "stats": str(output_dir / "stats.txt"),
            "gem5_config": str(output_dir / "config.json"),
        },
    }
    run_manifest_path = output_dir / "run_manifest.json"
    write_json(run_manifest_path, run_manifest)

    start_time = time.monotonic()
    command = gem5_command(gem5, output_dir, runtime_config_path, "o3")
    result = run_logged(command, output_dir / "run.log", check=False)
    elapsed = time.monotonic() - start_time
    runtime = (
        read_json(runtime_status)
        if runtime_status.is_file()
        else {
            "complete": False,
            "error": "gem5 did not write runtime_status.json",
        }
    )
    complete = result.returncode == 0 and bool(runtime.get("complete"))
    run_manifest.update(
        {
            "status": "complete" if complete else "incomplete",
            "runtime": runtime,
            "wall_seconds": elapsed,
            "gem5_return_code": result.returncode,
            "measurement_exact": runtime.get("measurement_exact", False),
            "measurement_overshoot_instructions": runtime.get(
                "measurement_overshoot_instructions"
            ),
        }
    )
    write_json(run_manifest_path, run_manifest)
    if not complete:
        raise PipelineError(
            "O3 run ended before the measurement completed; see "
            f"{run_manifest_path}"
        )
    if not (output_dir / "stats.txt").is_file():
        error = PipelineError("completed O3 run did not produce stats.txt")
        mark_manifest_failed(run_manifest_path, error)
        raise error
    delta = runtime.get("measurement_total_instruction_delta")
    if delta is not None and delta < measurement:
        error = PipelineError(
            f"measurement instruction delta is {delta}, expected {measurement}"
        )
        mark_manifest_failed(run_manifest_path, error)
        raise error
    return run_manifest_path


def main() -> int:
    args = build_parser().parse_args()
    try:
        output = run(args)
    except (PipelineError, OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"O3 measurement complete: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
