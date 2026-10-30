#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check real O3 restore with zero warmup and a short diagnostic window.

This is a restore regression check, not a SimPoint performance measurement.
Each invocation creates a new directory and verifies source checksums.
"""

import argparse
import importlib.util
import json
import subprocess
import sys
import time
import uuid
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))

from checkpoint_restore import prepare_restore_checkpoint  # noqa: E402
from common import (  # noqa: E402
    PipelineError,
    directory_digest,
    executable,
    gem5_command,
    prepare_stage_workdir,
    read_json,
    sha256_file,
    write_json,
)
from o3_batch_common import (  # noqa: E402
    RESULTS,
    measured_metrics,
    stop_process,
    write_markdown,
)


def run(args):
    spec = importlib.util.spec_from_file_location(
        "step_c", SCRIPT_DIR / "03_run_o3.py"
    )
    step_c = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(step_c)
    checkpoint = step_c.validate_checkpoint(args.checkpoint_manifest)
    profile = read_json(Path(checkpoint["profile_manifest"]))
    workload = step_c.checkpoint_workload(profile, checkpoint)
    gem5 = executable(args.gem5, "gem5 binary")
    if args.measurement <= 0 or args.timeout <= 0:
        raise PipelineError("measurement and timeout must be positive")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = (
        RESULTS / "o3_validation" / f"{stamp}_restore_{uuid.uuid4().hex[:8]}"
    )
    output.mkdir(parents=True, exist_ok=False)
    source = Path(checkpoint["checkpoint_path"])
    snapshot = Path(checkpoint["filesystem_snapshot"])
    before = {p.name: sha256_file(p) for p in source.iterdir() if p.is_file()}
    snapshot_before = directory_digest(snapshot)
    record = {
        "status": "running",
        "diagnostic_only": True,
        "checkpoint_manifest": str(args.checkpoint_manifest.resolve()),
        "gem5": str(gem5),
        "gem5_sha256": sha256_file(gem5),
        "warmup_instructions": 0,
        "measurement_instructions": args.measurement,
        "source_checksums_before": before,
        "snapshot_checksum_before": snapshot_before,
    }
    write_json(output / "validation.json", record)
    start = time.monotonic()
    try:
        workdir = output / "workdir"
        prepare_stage_workdir(workload, workdir, source_override=snapshot)
        restore = prepare_restore_checkpoint(
            checkpoint, workload, workdir, output / "restore_checkpoint"
        )
        write_json(output / "checkpoint_restore.json", restore)
        config = {
            "schema_version": 1,
            "workload": workload,
            "stage_workdir": str(workdir),
            "runtime_status_path": str(output / "runtime_status.json"),
            "system": checkpoint["system"],
            "o3": {
                "clock": checkpoint["system"]["clock"],
                "cache": {
                    "l1i_size": "32KiB",
                    "l1d_size": "32KiB",
                    "l2_size": "256KiB",
                },
                "branch_predictor": None,
                "branch_predictor_params": {},
                "cpu_params": {},
                "configuration_module": None,
            },
            "checkpoint_path": restore["restore_checkpoint"],
            "warmup_instructions": 0,
            "measurement_instructions": args.measurement,
        }
        config_path = output / "runtime_config.json"
        write_json(config_path, config)
        command = gem5_command(gem5, output, config_path, "o3")
        command.insert(1, "--listener-mode=off")
        record["command"] = command
        write_json(output / "validation.json", record)
        print(f"Restore validation: {output}", flush=True)
        with (output / "run.log").open("x", encoding="utf-8") as log:
            log.write("COMMAND: " + json.dumps(command) + "\n")
            log.flush()
            process = subprocess.Popen(
                command,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                record["return_code"] = process.wait(timeout=args.timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                stop_process(process)
                raise
        runtime = read_json(output / "runtime_status.json")
        record["runtime"] = runtime
        if record["return_code"] or not runtime.get("complete"):
            raise PipelineError("restore/diagnostic window did not complete")
        if runtime["measurement_total_instruction_delta"] < args.measurement:
            raise PipelineError("diagnostic window is shorter than requested")
        record["diagnostic_metrics"] = measured_metrics(
            output / "stats.txt",
            runtime["measurement_total_instruction_delta"],
        )
        record["status"] = "complete"
    except KeyboardInterrupt:
        record["status"] = "cancelled"
        record["error"] = "interrupted by user"
    except Exception as error:
        record["status"] = "failed"
        record["error"] = str(error)
    finally:
        record["wall_seconds"] = time.monotonic() - start
        record["source_checksums_after"] = {
            p.name: sha256_file(p) for p in source.iterdir() if p.is_file()
        }
        record["snapshot_checksum_after"] = directory_digest(snapshot)
        record["original_files_unchanged"] = (
            record["source_checksums_after"] == before
            and record["snapshot_checksum_after"] == snapshot_before
        )
        if not record["original_files_unchanged"]:
            record["status"] = "failed"
            record["error"] = (
                "source checkpoint or filesystem checksum changed"
            )
        write_json(output / "validation.json", record)
        text = "# Checkpoint restore doğrulaması\n\n"
        text += f"Durum: **{record['status']}**\n\n"
        text += f"Binary: `{gem5}`\n\n"
        text += f"Kaynak checkpoint: `{source}`\n\n"
        text += "Kaynak dosyalar korundu: "
        text += f"{record['original_files_unchanged']}\n\n"
        text += (
            f"0 warmup + {args.measurement} instruction tanılama penceresi. "
        )
        text += "Bu koşu SimPoint CPI sonucu olarak kullanılmaz.\n\n"
        text += "[gem5 log](run.log), [ayrıntılar](validation.json), "
        text += "[VMA yol eşlemesi](checkpoint_restore.json).\n"
        if record.get("error"):
            text += f"\nHata: {record['error']}\n"
        write_markdown(output / "validation.md", text)
    print(f"{record['status']}: {output / 'validation.md'}", flush=True)
    return (
        130
        if record["status"] == "cancelled"
        else int(record["status"] != "complete")
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", required=True, type=Path)
    parser.add_argument("--checkpoint-manifest", required=True, type=Path)
    parser.add_argument("--measurement", type=int, default=10_000)
    parser.add_argument("--timeout", type=float, default=120)
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
