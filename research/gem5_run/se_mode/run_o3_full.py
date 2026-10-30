#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run one SPEC command from program entry to termination entirely in O3."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from common import (
    PipelineError,
    gem5_command,
    load_workload,
    parse_instruction_count,
    prepare_stage_workdir,
    read_json,
    write_json,
)
from o3_batch_common import (
    add_options,
    discover,
    measured_metrics,
    o3_configuration,
    provenance,
    report_header,
    session_id,
    stop_process,
    test_names,
    write_markdown,
)


def run(args):
    names = test_names([args.test])
    if len(names) != 1:
        raise PipelineError("--test accepts exactly one command")
    selection = discover(names, args.batch_root)[0]
    profile = selection["profile"]
    record = provenance(args)
    configuration = o3_configuration(args, profile["system"])
    # Full O3 starts with original input, never the checkpoint's modified
    # filesystem. Verify it still matches the profiled command/input.
    workload = load_workload(Path(profile["workload"]["manifest_path"]))
    if workload["workload_fingerprint"] != profile["workload_fingerprint"]:
        raise PipelineError("original workload changed since profiling")
    identifier = session_id(args, record)
    output = selection["workload_root"] / "o3_full_runs" / identifier
    config = {
        "schema_version": 1,
        "workload": workload,
        "stage_workdir": str(output / "workdir"),
        "runtime_status_path": str(output / "runtime_status.json"),
        "system": dict(profile["system"], clock=configuration["clock"]),
        "o3": configuration,
        "max_insts": args.max_insts,
    }
    command = gem5_command(
        args.gem5, output, output / "runtime_config.json", "o3_full"
    )
    manifest = {
        "schema_version": 1,
        "manifest_type": "se_o3_full_run",
        "test": args.test,
        "session_id": identifier,
        "status": "planned",
        "provenance": record,
        "o3": configuration,
        "profile_manifest": str(selection["profile_path"]),
        "workload_fingerprint": workload["workload_fingerprint"],
        "system": config["system"],
        "command": command,
        "output": str(output),
        "metrics": None,
    }
    if args.dry_run:
        print(json.dumps(manifest, indent=2))
        return 0
    output.mkdir(parents=True, exist_ok=False)

    def report():
        write_json(output / "run_manifest.json", manifest)
        text = f"# Tam O3: {args.test}\n\n" + report_header(record)
        text += f"\nDurum: **{manifest['status']}**\n\n"
        text += "Program başlangıcından itibaren O3; restore, warmup veya "
        text += "istatistik reset'i uygulanmaz.\n\n"
        text += f"[gem5 log](run.log), [stats](stats.txt), "
        text += "[komut ve parametreler](run_manifest.json).\n\n"
        metrics = manifest.get("metrics")
        if metrics:
            text += "| N (committed inst.) | C (CPU cycle) | CPI | IPC |\n"
            text += "| --- | --- | --- | --- |\n"
            text += (
                "| "
                + " | ".join(
                    f"{metrics[k]:.9g}"
                    for k in ("instructions", "cycles", "cpi", "ipc")
                )
                + " |\n\n"
            )
        else:
            text += "**Tam program sonucu henüz yok.**\n\n"
        if manifest.get("error"):
            text += f"Hata: {manifest['error']}\n\n"
        text += f"Host wall seconds: {manifest.get('wall_seconds', '—')}.\n\n"
        text += "CPI = C/N; IPC = N/C. Bu ölçüm başlangıçtaki soğuk cache ve "
        text += "predictor durumunu ve programın son kısa interval'ini de "
        text += "kapsar. SimPoint sonucu profillenen tam interval'lerin "
        text += "ağırlıklı tahminidir; kapsam farkı karşılaştırmada dikkate "
        text += "alınmalıdır. `--max-insts` ile erken kesilen koşu tam sonuç "
        text += "sayılmaz. Bu bir resmi SPEC skoru değildir.\n"
        write_markdown(output / "report.md", text)

    manifest["status"] = "running"
    report()
    print(f"Full O3 report: {output / 'report.md'}", flush=True)
    start = time.monotonic()
    interrupted = False
    try:
        prepare_stage_workdir(workload, output / "workdir")
        write_json(output / "runtime_config.json", config)
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
                manifest["gem5_return_code"] = process.wait()
            except KeyboardInterrupt:
                stop_process(process)
                raise
        runtime = read_json(output / "runtime_status.json")
        manifest["runtime"] = runtime
        if (
            manifest["gem5_return_code"]
            or not runtime.get("complete")
            or not runtime.get("program_completed")
            or runtime.get("exit_code") != 0
        ):
            raise PipelineError("program did not terminate successfully")
        manifest["metrics"] = measured_metrics(
            output / "stats.txt", runtime["committed_instructions"]
        )
        manifest["status"] = "complete"
    except KeyboardInterrupt:
        interrupted = True
        manifest["status"] = "cancelled"
        manifest["error"] = "interrupted by user"
    except Exception as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
    finally:
        manifest["wall_seconds"] = time.monotonic() - start
        report()
    return 130 if interrupted else int(manifest["status"] != "complete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_options(parser)
    parser.add_argument(
        "--test",
        required=True,
        help="recorded workload ID, e.g. 502.gcc_r.cmd000.cpugcc_r",
    )
    parser.add_argument(
        "--max-insts",
        type=parse_instruction_count,
        default=0,
        help="smoke limit; early exit is never a full result",
    )
    args = parser.parse_args()
    try:
        return run(args)
    except (PipelineError, OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
