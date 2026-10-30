#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Schedule every selected SPEC SimPoint checkpoint as one independent job."""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
import threading
import time
from concurrent.futures import (
    ThreadPoolExecutor,
    as_completed,
)
from pathlib import Path

from common import (
    PipelineError,
    read_json,
    write_json,
)
from o3_batch_common import (
    SCRIPT_DIR,
    TERMS,
    add_options,
    discover,
    measured_metrics,
    o3_configuration,
    provenance,
    report_header,
    session_id,
    step_b_run_ids,
    stop_process,
    test_names,
    validate_points,
    weighted_result,
    write_markdown,
)


def checkpoint_jobs(selection, args):
    profile = selection["profile"]
    root = selection["workload_root"]
    run_id = selection["step_b_run_id"]
    warmup = int(run_id.removeprefix("warmup_"))
    paths = [
        root / stage / run_id / "checkpoint_set_manifest.json"
        for stage in ("checkpoints", "o2_checkpoint")
    ]
    paths = [p for p in paths if p.is_file()]
    if len(paths) != 1:
        raise PipelineError(
            f"{selection['test']}: expected one {run_id} "
            f"checkpoint set, found {paths}"
        )
    checkpoint_set = read_json(paths[0])
    if (
        checkpoint_set.get("status") != "complete"
        or checkpoint_set.get("manifest_type") != "se_simpoint_checkpoint_set"
        or checkpoint_set.get("profile_id") != profile["profile_id"]
    ):
        raise PipelineError(
            f"checkpoint set incomplete or mismatched: {paths[0]}"
        )
    spec = importlib.util.spec_from_file_location(
        "step_c", SCRIPT_DIR / "03_run_o3.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    jobs = []
    for manifest in checkpoint_set["checkpoint_manifests"]:
        checkpoint = module.validate_checkpoint(Path(manifest))
        module.checkpoint_workload(profile, checkpoint)
        if (
            checkpoint["profile_id"] != profile["profile_id"]
            or checkpoint["workload_fingerprint"]
            != profile["workload_fingerprint"]
            or Path(checkpoint["profile_manifest"]).resolve()
            != selection["profile_path"].resolve()
        ):
            raise PipelineError(f"checkpoint/profile mismatch: {manifest}")
        if (
            checkpoint["interval_instructions"]
            != profile["interval_instructions"]
            or checkpoint["system"] != profile["system"]
            or checkpoint["requested_warmup_instructions"] != warmup
        ):
            raise PipelineError(
                f"checkpoint measurement contract mismatch: {manifest}"
            )
        checkpoint_id = checkpoint["checkpoint_id"]
        if Path(checkpoint_id).name != checkpoint_id or checkpoint_id in (
            ".",
            "..",
        ):
            raise PipelineError(f"unsafe checkpoint ID: {checkpoint_id}")
        jobs.append(
            {
                "test": selection["test"],
                "step_b_run_id": run_id,
                "checkpoint_id": checkpoint_id,
                "checkpoint_manifest": str(Path(manifest).resolve()),
                "cluster_id": checkpoint["cluster_id"],
                "interval_index": checkpoint["interval_index"],
                "weight": checkpoint["weight"],
                "warmup_instructions": checkpoint[
                    "actual_warmup_instructions"
                ],
                "measurement_instructions": checkpoint[
                    "interval_instructions"
                ],
                "checkpoint_gem5": checkpoint["gem5"],
                "status": "queued",
            }
        )
    if len({j["checkpoint_id"] for j in jobs}) != len(jobs):
        raise PipelineError("duplicate checkpoint output IDs")
    # Validate full cluster coverage before launching any work.
    validate_points(jobs, profile)
    return jobs


def step_command(job, args, output):
    command = [
        sys.executable,
        str(SCRIPT_DIR / "03_run_o3.py"),
        "--gem5",
        str(args.gem5.expanduser().resolve()),
        "--checkpoint-manifest",
        job["checkpoint_manifest"],
        "--output",
        str(output),
        "--l1i-size",
        args.l1i_size,
        "--l1d-size",
        args.l1d_size,
        "--l2-size",
        args.l2_size,
    ]
    for name in ("clock", "branch_predictor", "configuration_module"):
        if getattr(args, name):
            command.extend(
                ["--" + name.replace("_", "-"), str(getattr(args, name))]
            )
    for name in ("cpu_param", "bp_param"):
        for value in getattr(args, name):
            command.extend(["--" + name.replace("_", "-"), value])
    if args.allow_gem5_mismatch:
        command.append("--allow-gem5-mismatch")
    return command


class Workers:
    """Track only this invocation's child sessions for safe cancellation."""

    def __init__(self):
        self.lock = threading.Lock()
        self.processes = set()
        self.cancelled = False

    def cancel(self):
        with self.lock:
            self.cancelled = True
            processes = list(self.processes)
        for process in processes:
            stop_process(process)

    def run(self, job):
        start = time.monotonic()
        try:
            with Path(job["driver_log"]).open("x", encoding="utf-8") as log:
                log.write("COMMAND: " + json.dumps(job["command"]) + "\n")
                log.flush()
                with self.lock:
                    if self.cancelled:
                        job["status"] = "cancelled"
                        return job
                    process = subprocess.Popen(
                        job["command"],
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        start_new_session=True,
                    )
                    self.processes.add(process)
                    job["status"] = "running"
                    job["driver_pid"] = process.pid
                try:
                    job["return_code"] = process.wait()
                finally:
                    with self.lock:
                        self.processes.discard(process)
            if job["return_code"]:
                raise PipelineError(
                    f"Step C exit {job['return_code']}; "
                    f"see {job['driver_log']}"
                )
            manifest_path = Path(job["output"]) / "run_manifest.json"
            manifest = read_json(manifest_path)
            if manifest.get("status") != "complete" or not manifest.get(
                "measurement_matches_bbv_interval"
            ):
                raise PipelineError("measurement incomplete or wrong interval")
            runtime = manifest["runtime"]
            observed = runtime["measurement_total_instruction_delta"]
            if observed < job["measurement_instructions"]:
                raise PipelineError("measurement is shorter than requested")
            job["metrics"] = measured_metrics(
                Path(manifest["paths"]["stats"]), observed
            )
            job["measurement_overshoot_instructions"] = (
                observed - job["measurement_instructions"]
            )
            job["run_manifest"] = str(manifest_path)
            job["status"] = "complete"
        except Exception as error:
            job["status"] = "failed"
            job["error"] = str(error)
        finally:
            job["wall_seconds"] = time.monotonic() - start
        return job


def workload_report(selection, jobs, record, configuration):
    summary = weighted_result(jobs, selection["profile"])
    status = (
        "complete"
        if summary
        else (
            "failed"
            if any(j["status"] == "failed" for j in jobs)
            else "incomplete"
        )
    )
    data = {
        "schema_version": 1,
        "manifest_type": "se_o3_batch_workload",
        "test": selection["test"],
        "step_b_run_id": selection["step_b_run_id"],
        "status": status,
        "profile_manifest": str(selection["profile_path"]),
        "provenance": record,
        "o3": configuration,
        "summary": summary,
        "jobs": jobs,
    }
    lines = [
        f"# O3 SimPoint: {selection['test']}\n",
        f"\nStep B koşusu: `{selection['step_b_run_id']}`\n\n",
        report_header(record),
        f"\nDurum: **{status}**\n",
        "\n| Cluster | Interval | Weight | Warmup | N | C | CPI | IPC | "
        "w*CPI | Overshoot | Durum | Log |\n",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | "
        "--- | --- |\n",
    ]
    for job in jobs:
        metrics = job.get("metrics")
        values = [
            str(job[k])
            for k in (
                "cluster_id",
                "interval_index",
                "weight",
                "warmup_instructions",
            )
        ]
        values += (
            [
                f"{metrics[k]:.9g}"
                for k in ("instructions", "cycles", "cpi", "ipc")
            ]
            + [f"{job['weight'] * metrics['cpi']:.9g}"]
            if metrics
            else ["—"] * 5
        )
        values += [
            str(job.get("measurement_overshoot_instructions", "—")),
            job["status"],
            f"[driver]({job['driver_log']})",
        ]
        lines.append("| " + " | ".join(values) + " |\n")
        if job.get("error"):
            lines.append(f"\n`{job['checkpoint_id']}`: {job['error']}\n\n")
    if summary:
        lines.append(
            f"\n**Toplam ağırlıklı CPI: {summary['weighted_cpi']:.9g}**\n\n"
            f"**Tahmini IPC: {summary['estimated_ipc']:.9g}**\n\n"
            f"Ağırlık toplamı: {summary['weight_sum']:.12g}. "
            f"BBV kapsamı: {summary['bbv_covered_instructions']}; "
            "profil instruction toplamı: "
            f"{summary['profile_total_instructions']}. "
            "Tam programı temsil eden profil: "
            f"{summary['representative_of_full_workload']}.\n"
        )
    else:
        lines.append(
            "\n**Toplam yok: bütün checkpoint'ler tamamlanmalıdır.**\n"
        )
    lines.append(TERMS)
    return data, "".join(lines)


def run(args):
    if args.jobs <= 0:
        raise PipelineError("--jobs must be positive")
    workloads = discover(test_names(args.tests), args.batch_root)
    run_ids = step_b_run_ids(args.run_ids)
    selections = [
        dict(workload, step_b_run_id=run_id)
        for workload in workloads
        for run_id in run_ids
    ]
    record = provenance(args)
    identifier = session_id(args, record)
    jobs = []
    for selection in selections:
        selection["o3"] = o3_configuration(
            args, selection["profile"]["system"]
        )
        selection["output"] = (
            selection["workload_root"]
            / "o3_runs"
            / selection["step_b_run_id"]
            / identifier
        )
        selected_jobs = checkpoint_jobs(selection, args)
        for job in selected_jobs:
            old = job["checkpoint_gem5"].get("repository_commit")
            new = record["gem5"].get("repository_commit")
            if old and new and old != new and not args.allow_gem5_mismatch:
                raise PipelineError(
                    "checkpoint/gem5 commits differ; review compatibility "
                    "and pass --allow-gem5-mismatch explicitly"
                )
            output = selection["output"] / job["checkpoint_id"]
            job["output"] = str(output)
            job["driver_log"] = str(
                selection["output"] / f"{job['checkpoint_id']}.driver.log"
            )
            job["command"] = step_command(job, args, output)
        jobs.extend(selected_jobs)
    plan = {
        "session_id": identifier,
        "jobs_limit": args.jobs,
        "step_b_run_ids": run_ids,
        "provenance": record,
        "jobs": jobs,
    }
    if args.dry_run:
        print(json.dumps(plan, indent=2))
        return 0
    # Exclusive reservation; never reuse or remove an earlier session.
    batch_output = selections[0]["batch_root"] / "o3_batches" / identifier
    batch_output.mkdir(parents=True, exist_ok=False)
    for selection in selections:
        selection["output"].mkdir(parents=True, exist_ok=False)

    def reports():
        workloads = []
        for selection in selections:
            data, markdown = workload_report(
                selection,
                [
                    j
                    for j in jobs
                    if j["test"] == selection["test"]
                    and j["step_b_run_id"] == selection["step_b_run_id"]
                ],
                record,
                selection["o3"],
            )
            report_path = selection["output"] / "report.md"
            write_json(selection["output"] / "report.json", data)
            write_markdown(report_path, markdown)
            workloads.append(
                {
                    "test": selection["test"],
                    "step_b_run_id": selection["step_b_run_id"],
                    "status": data["status"],
                    "summary": data["summary"],
                    "report": str(report_path),
                }
            )
        status = (
            "complete"
            if all(w["status"] == "complete" for w in workloads)
            else (
                "failed"
                if any(j["status"] == "failed" for j in jobs)
                else "incomplete"
            )
        )
        write_json(
            batch_output / "batch_report.json",
            dict(plan, status=status, workloads=workloads),
        )
        markdown = "# O3 batch\n\n" + report_header(record)
        markdown += f"\nDurum: **{status}**; jobs: {args.jobs}; "
        markdown += f"checkpoint işi: {len(jobs)}.\n\n"
        for workload in workloads:
            summary = workload["summary"]
            total = (
                f"CPI={summary['weighted_cpi']:.9g}, "
                f"IPC={summary['estimated_ipc']:.9g}"
                if summary
                else "Toplam yok"
            )
            markdown += (
                f"- [{workload['test']} / {workload['step_b_run_id']}]"
                f"({workload['report']}): "
                f"{workload['status']}; {total}\n"
            )
        write_markdown(batch_output / "batch_report.md", markdown + TERMS)

    reports()
    print(f"Batch report: {batch_output / 'batch_report.md'}", flush=True)
    workers = Workers()
    pool = ThreadPoolExecutor(max_workers=args.jobs)
    interrupted = False
    try:
        futures = [pool.submit(workers.run, job) for job in jobs]
        for future in as_completed(futures):
            job = future.result()
            reports()
            print(
                f"{job['test']} {job['step_b_run_id']} "
                f"{job['checkpoint_id']}: {job['status']}",
                flush=True,
            )
    except KeyboardInterrupt:
        interrupted = True
        workers.cancel()
    finally:
        pool.shutdown(wait=True)
        for job in jobs:
            if job["status"] in ("queued", "running"):
                job["status"] = "cancelled"
        reports()
    return (
        130
        if interrupted
        else int(any(j["status"] != "complete" for j in jobs))
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_options(parser)
    parser.add_argument(
        "--tests",
        nargs="+",
        required=True,
        help="recorded workload IDs, e.g. 502.gcc_r.cmd000.cpugcc_r",
    )
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument(
        "--run-ids",
        "--run-id",
        dest="run_ids",
        nargs="+",
        required=True,
        help="Step B directory IDs, e.g. warmup_50000000",
    )
    parser.add_argument("--allow-gem5-mismatch", action="store_true")
    args = parser.parse_args()
    try:
        return run(args)
    except (PipelineError, OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
