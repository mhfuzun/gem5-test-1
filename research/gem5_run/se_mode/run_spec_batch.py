#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Run workload creation, SimPoint profiling, and checkpoints in parallel."""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import shlex
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import (
    Mapping,
    Sequence,
)
from concurrent.futures import (
    ThreadPoolExecutor,
    as_completed,
)
from datetime import datetime
from pathlib import Path
from typing import (
    Any,
)

from common import (
    DEFAULT_CLOCK,
    DEFAULT_INTERVAL,
    DEFAULT_MEMORY_SIZE,
    DEFAULT_WARMUP,
    PipelineError,
    executable,
    gem5_invocation,
    gem5_record,
    load_workload,
    parse_instruction_count,
    read_json,
    sha256_file,
    write_json,
)
from spec_workload import (
    command_lines,
    create_manifests,
    parse_spec_command,
)

SCRIPT_DIR = Path(__file__).resolve().parent
PROFILE_SCRIPT = SCRIPT_DIR / "01_profile.py"
CHECKPOINT_SCRIPT = SCRIPT_DIR / "02_checkpoint.py"
MAX_JOBS = 12
PRINT_LOCK = threading.Lock()


def parse_assignment(value: str, option: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            f"{option} expects BENCHMARK=VALUE, got {value!r}"
        )
    benchmark, assigned = value.split("=", 1)
    if not benchmark or not assigned:
        raise argparse.ArgumentTypeError(
            f"{option} expects BENCHMARK=VALUE, got {value!r}"
        )
    return benchmark, assigned


def assignment_map(values: Sequence[str], option: str) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for value in values:
        benchmark, assigned = parse_assignment(value, option)
        if benchmark in parsed:
            raise PipelineError(
                f"duplicate {option} assignment for {benchmark}"
            )
        parsed[benchmark] = assigned
    return parsed


def resolve_bundle_gem5(bundle: Path) -> Path:
    """Resolve gem5.fast from an extracted bundle or bundle-out directory."""
    resolved = bundle.expanduser().resolve()
    if resolved.is_file():
        if resolved.name != "gem5.fast":
            raise PipelineError(
                f"--bundle file must be gem5.fast, got: {resolved}"
            )
        candidates = [resolved]
    elif resolved.is_dir():
        direct = resolved / "bin" / "gem5.fast"
        candidates = [direct] if direct.is_file() else []
        if not candidates:
            candidates = sorted(
                path
                for path in resolved.glob("*/bin/gem5.fast")
                if not path.parts[-3].startswith(".")
            )
    else:
        raise PipelineError(f"bundle path does not exist: {resolved}")

    if not candidates:
        raise PipelineError(
            f"no extracted */bin/gem5.fast found under {resolved}"
        )
    if len(candidates) > 1:
        rendered = "\n  ".join(str(path) for path in candidates)
        raise PipelineError(
            "multiple gem5.fast bundles found; pass one extracted bundle "
            f"directory explicitly:\n  {rendered}"
        )
    binary = executable(candidates[0], "bundled gem5.fast")
    invocation = gem5_invocation(binary)
    if len(invocation) != 2:
        raise PipelineError(
            "portable bundle is missing its executable run-gem5 launcher: "
            f"{binary.parent.parent}"
        )
    return binary


def load_spec_runs_catalog(
    spec_runs_root: Path, size: str
) -> tuple[Path, dict[str, str]]:
    root = spec_runs_root.expanduser().resolve()
    if not root.is_dir():
        raise PipelineError(f"spec-runs root does not exist: {root}")
    size_root = root / size
    if not size_root.is_dir():
        raise PipelineError(
            f"spec-runs input size directory does not exist: {size_root}"
        )
    manifest_path = size_root / "manifest.tsv"
    if not manifest_path.is_file():
        raise PipelineError(
            f"spec-runs manifest does not exist: {manifest_path}"
        )
    lines = manifest_path.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines()
    if not lines or lines[0].split("\t") != ["benchmark", "run_dir"]:
        raise PipelineError(
            f"invalid spec-runs manifest header: {manifest_path}"
        )
    catalog: dict[str, str] = {}
    for line_number, line in enumerate(lines[1:], start=2):
        if not line.strip():
            continue
        fields = line.split("\t")
        if len(fields) != 2 or not all(fields):
            raise PipelineError(
                f"malformed spec-runs manifest row "
                f"{manifest_path}:{line_number}"
            )
        benchmark, run_dir = fields
        if benchmark in catalog:
            raise PipelineError(
                f"duplicate benchmark {benchmark!r} in {manifest_path}"
            )
        catalog[benchmark] = run_dir
    return size_root, catalog


def resolve_run_directory(
    benchmark: str,
    size_root: Path,
    catalog: Mapping[str, str],
    overrides: Mapping[str, str],
) -> Path:
    if benchmark in overrides:
        run_dir = Path(overrides[benchmark]).expanduser().resolve()
        if not run_dir.is_dir():
            raise PipelineError(
                f"run directory for {benchmark} does not exist: {run_dir}"
            )
        return run_dir
    if benchmark not in catalog:
        raise PipelineError(
            f"{benchmark} is not listed in {size_root / 'manifest.tsv'}"
        )
    benchmark_root = (size_root / benchmark).resolve()
    run_dir = (benchmark_root / catalog[benchmark]).resolve()
    try:
        run_dir.relative_to(benchmark_root)
    except ValueError as error:
        raise PipelineError(
            f"run_dir for {benchmark} escapes its spec-runs directory: "
            f"{catalog[benchmark]!r}"
        ) from error
    if not run_dir.is_dir():
        raise PipelineError(
            f"exported run directory for {benchmark} does not exist: "
            f"{run_dir}"
        )
    return run_dir


def selected_manifest_paths(
    benchmark: str,
    run_dir: Path,
    output_dir: Path,
    command_index: int | None,
) -> list[Path]:
    commands_path = run_dir / "speccmds.cmd"
    if not commands_path.is_file():
        raise PipelineError(
            f"SPEC command file does not exist: {commands_path}"
        )
    parsed = [
        parse_spec_command(line, run_dir)
        for line in command_lines(commands_path)
    ]
    if command_index is None:
        selected = list(enumerate(parsed))
    else:
        if not 0 <= command_index < len(parsed):
            raise PipelineError(
                f"command index {command_index} for {benchmark} is outside "
                f"0..{len(parsed) - 1}"
            )
        selected = [(command_index, parsed[command_index])]
    paths = []
    for index, command in selected:
        stem = Path(command["binary"]).name.split("_base", 1)[0]
        workload_id = f"{benchmark}.cmd{index:03d}.{stem}"
        paths.append(output_dir / f"{workload_id}.json")
    return paths


def create_workloads(
    args: argparse.Namespace,
    benchmark: str,
    run_dir: Path,
    output_dir: Path,
    command_index: int | None,
) -> list[Path]:
    expected = selected_manifest_paths(
        benchmark, run_dir, output_dir, command_index
    )
    if args.dry_run:
        return expected

    manifest_args = argparse.Namespace(
        run_dir=run_dir,
        commands=None,
        benchmark=benchmark,
        output_dir=output_dir,
        command_index=command_index,
        env=args.env,
        sysroot=args.sysroot,
        redirect=args.redirect,
        force=True,
    )
    captured = io.StringIO()
    with contextlib.redirect_stdout(captured):
        create_manifests(manifest_args)
    log_path = args.output_root / "driver_logs" / benchmark / "manifest.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(captured.getvalue(), encoding="utf-8")
    missing = [path for path in expected if not path.is_file()]
    if missing:
        raise PipelineError(
            "workload generator omitted expected manifest(s): "
            + ", ".join(str(path) for path in missing)
        )
    return expected


def stage_command(
    command: Sequence[str], log_path: Path
) -> subprocess.CompletedProcess[None]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as output:
        output.write("COMMAND: " + json.dumps(list(command)) + "\n")
        output.flush()
        return subprocess.run(
            list(command),
            stdout=output,
            stderr=subprocess.STDOUT,
            check=False,
        )


def announce(message: str) -> None:
    with PRINT_LOCK:
        print(message, flush=True)


def profile_resume_errors(
    manifest: Mapping[str, Any],
    workload: Mapping[str, Any],
    args: argparse.Namespace,
    gem5_sha256: str,
) -> list[str]:
    errors = []
    checks = {
        "status": (manifest.get("status"), "complete"),
        "workload fingerprint": (
            manifest.get("workload_fingerprint"),
            workload["workload_fingerprint"],
        ),
        "interval": (manifest.get("interval_instructions"), args.interval),
        "clock": (manifest.get("system", {}).get("clock"), args.clock),
        "memory size": (
            manifest.get("system", {}).get("memory_size"),
            args.memory_size,
        ),
        "max-k": (
            manifest.get("clustering", {}).get("max_k_requested"),
            args.max_k,
        ),
        "seed": (manifest.get("clustering", {}).get("seed"), args.seed),
        "gem5 SHA-256": (
            manifest.get("tools", {}).get("gem5", {}).get("sha256"),
            gem5_sha256,
        ),
    }
    for label, (actual, expected) in checks.items():
        if actual != expected:
            errors.append(
                f"{label}: existing={actual!r}, requested={expected!r}"
            )
    if args.max_insts:
        committed = manifest.get("runtime", {}).get("committed_instructions")
        if not isinstance(committed, int) or not (
            args.max_insts <= committed <= args.max_insts + 16
        ):
            errors.append(
                "partial-profile instruction limit does not match "
                f"--max-insts {args.max_insts}"
            )
    elif not manifest.get("representative_of_full_workload"):
        errors.append("existing profile is not a complete-workload profile")
    return errors


def checkpoint_resume_errors(
    manifest: Mapping[str, Any],
    profile_path: Path,
    args: argparse.Namespace,
    gem5_sha256: str,
) -> list[str]:
    errors = []
    checks = {
        "status": (manifest.get("status"), "complete"),
        "profile manifest": (
            str(Path(str(manifest.get("profile_manifest", ""))).resolve()),
            str(profile_path.resolve()),
        ),
        "warmup": (
            manifest.get("requested_warmup_instructions"),
            args.warmup,
        ),
        "gem5 SHA-256": (
            manifest.get("gem5", {}).get("sha256"),
            gem5_sha256,
        ),
    }
    for label, (actual, expected) in checks.items():
        if actual != expected:
            errors.append(
                f"{label}: existing={actual!r}, requested={expected!r}"
            )
    checkpoint_manifests = manifest.get("checkpoint_manifests")
    if not isinstance(checkpoint_manifests, list) or not checkpoint_manifests:
        errors.append("checkpoint manifest list is empty")
    else:
        for value in checkpoint_manifests:
            path = Path(str(value))
            if not path.is_file():
                errors.append(f"checkpoint manifest is missing: {path}")
                continue
            checkpoint = read_json(path)
            checkpoint_path = Path(str(checkpoint.get("checkpoint_path", "")))
            if (
                checkpoint.get("status") != "complete"
                or not (checkpoint_path / "m5.cpt").is_file()
            ):
                errors.append(f"checkpoint is incomplete: {path}")
    return errors


def ensure_reusable(path: Path, errors: Sequence[str], stage: str) -> None:
    if errors:
        details = "\n  ".join(errors)
        raise PipelineError(
            f"existing {stage} cannot be resumed from {path}:\n  {details}\n"
            "Use --force to replace that workload's generated stages."
        )


def run_workload(
    manifest_path: Path,
    args: argparse.Namespace,
    gem5: Path,
    gem5_sha256: str,
) -> dict[str, Any]:
    started = time.monotonic()
    workload = load_workload(manifest_path)
    workload_id = workload["workload_id"]
    benchmark = workload.get("benchmark", workload_id.split(".", 1)[0])
    result_root = args.output_root / "results"
    driver_root = args.output_root / "driver_logs" / workload_id
    profile_path = (
        result_root / workload_id / "profile" / "profile_manifest.json"
    )
    checkpoint_path = (
        result_root
        / workload_id
        / "checkpoints"
        / f"warmup_{args.warmup}"
        / "checkpoint_set_manifest.json"
    )
    result: dict[str, Any] = {
        "benchmark": benchmark,
        "workload_id": workload_id,
        "workload_manifest": str(manifest_path),
        "status": "running",
        "profile": {"status": "pending", "manifest": str(profile_path)},
        "checkpoint": {
            "status": "pending",
            "manifest": str(checkpoint_path),
        },
    }
    try:
        if workload.get("stdin") is not None:
            raise PipelineError(
                "Step B cannot safely restore a standard-input file offset "
                "with this gem5 revision; refusing to spend time on Step A "
                "for an unusable checkpoint pipeline"
            )
        if profile_path.exists() and args.resume and not args.force:
            profile = read_json(profile_path)
            ensure_reusable(
                profile_path,
                profile_resume_errors(profile, workload, args, gem5_sha256),
                "profile",
            )
            result["profile"]["status"] = "reused"
            announce(f"[REUSE A] {workload_id}")
        else:
            command = [
                sys.executable,
                str(PROFILE_SCRIPT),
                "--gem5",
                str(gem5),
                "--workload",
                str(manifest_path),
                "--simpoint-bin",
                str(args.simpoint_bin),
                "--results-root",
                str(result_root),
                "--interval",
                str(args.interval),
                "--max-k",
                str(args.max_k),
                "--seed",
                str(args.seed),
                "--clock",
                args.clock,
                "--memory-size",
                args.memory_size,
            ]
            if args.max_insts:
                command.extend(["--max-insts", str(args.max_insts)])
            if args.allow_partial_profile:
                command.append("--allow-partial-profile")
            if args.force:
                command.append("--force")
            result["profile"]["status"] = "running"
            announce(f"[START A] {workload_id}")
            completed = stage_command(
                command, driver_root / "01_profile.driver.log"
            )
            if completed.returncode:
                raise PipelineError(
                    f"Step A exited with {completed.returncode}; see "
                    f"{driver_root / '01_profile.driver.log'}"
                )
            result["profile"]["status"] = "complete"
            announce(f"[DONE  A] {workload_id}")

        profile = read_json(profile_path)
        if checkpoint_path.exists() and args.resume and not args.force:
            ensure_reusable(
                checkpoint_path,
                checkpoint_resume_errors(
                    read_json(checkpoint_path),
                    profile_path,
                    args,
                    gem5_sha256,
                ),
                "checkpoint set",
            )
            result["checkpoint"]["status"] = "reused"
            announce(f"[REUSE B] {workload_id}")
        else:
            command = [
                sys.executable,
                str(CHECKPOINT_SCRIPT),
                "--gem5",
                str(gem5),
                "--profile-manifest",
                str(profile_path),
                "--warmup",
                str(args.warmup),
            ]
            if args.allow_partial_profile:
                command.append("--allow-partial-profile")
            if args.force:
                command.append("--force")
            result["checkpoint"]["status"] = "running"
            announce(f"[START B] {workload_id}")
            completed = stage_command(
                command, driver_root / "02_checkpoint.driver.log"
            )
            if completed.returncode:
                raise PipelineError(
                    f"Step B exited with {completed.returncode}; see "
                    f"{driver_root / '02_checkpoint.driver.log'}"
                )
            result["checkpoint"]["status"] = "complete"
            announce(f"[DONE  B] {workload_id}")

        checkpoint = read_json(checkpoint_path)
        result["checkpoint_count"] = len(
            checkpoint.get("checkpoint_manifests", [])
        )
        result["selected_simpoints"] = len(profile.get("selected_points", []))
        result["status"] = "complete"
    except (OSError, PipelineError, ValueError, KeyError) as error:
        if result["profile"]["status"] == "running":
            result["profile"]["status"] = "failed"
        if result["checkpoint"]["status"] == "running":
            result["checkpoint"]["status"] = "failed"
        result["status"] = "failed"
        result["error"] = str(error)
        announce(f"[FAILED ] {workload_id}: {error}")
    result["wall_seconds"] = time.monotonic() - started
    return result


def atomic_write_text(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False
    ) as output:
        output.write(contents)
        temporary = Path(output.name)
    os.replace(temporary, path)


def markdown_cell(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\n", "<br>")


def render_report(report: Mapping[str, Any]) -> str:
    workloads = report.get("workloads", [])
    completed = sum(item.get("status") == "complete" for item in workloads)
    failed = sum(item.get("status") == "failed" for item in workloads)
    lines = [
        "# SPEC SimPoint batch report",
        "",
        f"- Status: `{report.get('status')}`",
        f"- Started: `{report.get('started_at')}`",
        f"- Finished: `{report.get('finished_at', '-')}`",
        f"- Parallel jobs: `{report.get('jobs')}`",
        f"- Completed workloads: `{completed}`",
        f"- Failed workloads: `{failed}`",
        f"- gem5: `{report.get('gem5', {}).get('path')}`",
        "",
        "| Workload | Profile | Checkpoint | Checkpoints | Seconds |",
        "|---|---:|---:|---:|---:|",
    ]
    for item in workloads:
        lines.append(
            "| {workload} | {profile} | {checkpoint} | {count} | {seconds} |".format(
                workload=markdown_cell(item.get("workload_id", "?")),
                profile=markdown_cell(
                    item.get("profile", {}).get("status", "-")
                ),
                checkpoint=markdown_cell(
                    item.get("checkpoint", {}).get("status", "-")
                ),
                count=markdown_cell(item.get("checkpoint_count", "-")),
                seconds=f"{item.get('wall_seconds', 0):.1f}",
            )
        )
    failures = [item for item in workloads if item.get("status") == "failed"]
    if failures:
        lines.extend(["", "## Failures", ""])
        for item in failures:
            lines.extend(
                [
                    f"### {item.get('workload_id', '?')}",
                    "",
                    f"```text\n{item.get('error', 'unknown error')}\n```",
                    "",
                ]
            )
    lines.extend(
        [
            "",
            "## Invocation",
            "",
            "```text",
            report.get("command", ""),
            "```",
            "",
        ]
    )
    return "\n".join(lines)


def write_reports(output_root: Path, report: Mapping[str, Any]) -> None:
    write_json(output_root / "batch_report.json", report)
    atomic_write_text(output_root / "batch_report.md", render_report(report))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    gem5_input = parser.add_mutually_exclusive_group(required=True)
    gem5_input.add_argument(
        "--bundle",
        type=Path,
        help="extracted bundle root, bundle-out directory, or gem5.fast",
    )
    gem5_input.add_argument(
        "--gem5", type=Path, help="non-bundled gem5.fast executable"
    )
    parser.add_argument("--spec-runs-root", required=True, type=Path)
    parser.add_argument(
        "--size", choices=("test", "train", "ref"), default="train"
    )
    parser.add_argument("--benchmarks", nargs="+", required=True)
    parser.add_argument(
        "--run-dir",
        action="append",
        default=[],
        metavar="BENCHMARK=PATH",
    )
    parser.add_argument(
        "--command-index",
        action="append",
        default=[],
        metavar="BENCHMARK=N",
    )
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--simpoint-bin", required=True, type=Path)
    parser.add_argument(
        "--output-root", type=Path, default=SCRIPT_DIR / "batch_results"
    )
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument(
        "--interval", type=parse_instruction_count, default=DEFAULT_INTERVAL
    )
    parser.add_argument(
        "--warmup", type=parse_instruction_count, default=DEFAULT_WARMUP
    )
    parser.add_argument("--max-k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--clock", default=DEFAULT_CLOCK)
    parser.add_argument("--memory-size", default=DEFAULT_MEMORY_SIZE)
    parser.add_argument("--max-insts", type=parse_instruction_count, default=0)
    parser.add_argument("--allow-partial-profile", action="store_true")
    parser.add_argument(
        "--env", action="append", default=[], metavar="NAME=VALUE"
    )
    parser.add_argument(
        "--redirect", action="append", default=[], metavar="GUEST=HOST"
    )
    parser.add_argument(
        "--resume",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="reuse compatible completed stages (default: enabled)",
    )
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser


def run(args: argparse.Namespace) -> int:
    if not 1 <= args.jobs <= MAX_JOBS:
        raise PipelineError(f"--jobs must be between 1 and {MAX_JOBS}")
    if len(set(args.benchmarks)) != len(args.benchmarks):
        raise PipelineError("--benchmarks contains duplicate entries")
    if args.max_insts and not args.allow_partial_profile:
        raise PipelineError(
            "--max-insts requires --allow-partial-profile and is only for "
            "smoke testing"
        )

    run_overrides = assignment_map(args.run_dir, "--run-dir")
    index_text = assignment_map(args.command_index, "--command-index")
    unknown = (set(run_overrides) | set(index_text)) - set(args.benchmarks)
    if unknown:
        raise PipelineError(
            "assignments were supplied for unlisted benchmark(s): "
            + ", ".join(sorted(unknown))
        )
    command_indices: dict[str, int] = {}
    for benchmark, value in index_text.items():
        try:
            command_indices[benchmark] = int(value)
        except ValueError as error:
            raise PipelineError(
                f"--command-index for {benchmark} is not an integer: {value}"
            ) from error

    gem5 = (
        resolve_bundle_gem5(args.bundle)
        if args.bundle
        else executable(args.gem5, "gem5 binary")
    )
    args.simpoint_bin = executable(args.simpoint_bin, "SimPoint binary")
    if args.sysroot:
        args.sysroot = args.sysroot.expanduser().resolve()
        if not args.sysroot.is_dir():
            raise PipelineError(f"sysroot does not exist: {args.sysroot}")
    args.output_root = args.output_root.expanduser().resolve()
    size_root, spec_runs_catalog = load_spec_runs_catalog(
        args.spec_runs_root, args.size
    )

    jobs = []
    preflight_failures = []
    for benchmark in args.benchmarks:
        try:
            run_dir = resolve_run_directory(
                benchmark,
                size_root,
                spec_runs_catalog,
                run_overrides,
            )
            output_dir = args.output_root / "workloads" / benchmark
            manifests = create_workloads(
                args,
                benchmark,
                run_dir,
                output_dir,
                command_indices.get(benchmark),
            )
            jobs.extend(manifests)
        except (OSError, PipelineError, ValueError) as error:
            preflight_failures.append(
                {
                    "benchmark": benchmark,
                    "workload_id": f"{benchmark}.manifest-setup",
                    "status": "failed",
                    "error": str(error),
                    "profile": {"status": "not-started"},
                    "checkpoint": {"status": "not-started"},
                    "wall_seconds": 0,
                }
            )
            announce(f"[FAILED ] {benchmark} manifest setup: {error}")

    if args.dry_run:
        print(f"gem5 command: {shlex.join(gem5_invocation(gem5))}")
        print(f"parallel jobs: {args.jobs}")
        for path in jobs:
            print(path)
        return 1 if preflight_failures else 0

    args.output_root.mkdir(parents=True, exist_ok=True)
    gem5_info = gem5_record(gem5)
    gem5_sha256 = sha256_file(gem5)
    report: dict[str, Any] = {
        "schema_version": 1,
        "manifest_type": "se_simpoint_batch_report",
        "status": "running",
        "started_at": datetime.now().astimezone().isoformat(),
        "jobs": args.jobs,
        "benchmarks": args.benchmarks,
        "spec_runs_root": str(args.spec_runs_root.expanduser().resolve()),
        "input_size": args.size,
        "command": shlex.join(sys.argv),
        "gem5": gem5_info,
        "simpoint_binary": str(args.simpoint_bin),
        "parameters": {
            "interval_instructions": args.interval,
            "warmup_instructions": args.warmup,
            "max_k": args.max_k,
            "seed": args.seed,
            "clock": args.clock,
            "memory_size": args.memory_size,
            "max_insts": args.max_insts,
        },
        "workloads": sorted(
            preflight_failures, key=lambda item: item["workload_id"]
        ),
    }
    write_reports(args.output_root, report)
    announce(
        f"Running {len(jobs)} workload pipeline(s) with --jobs {args.jobs}"
    )
    results = list(preflight_failures)
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_workload,
                manifest,
                args,
                gem5,
                gem5_sha256,
            ): manifest
            for manifest in jobs
        }
        for future in as_completed(futures):
            try:
                result = future.result()
            except Exception as error:  # Preserve reports for worker bugs.
                manifest = futures[future]
                result = {
                    "workload_id": manifest.stem,
                    "workload_manifest": str(manifest),
                    "status": "failed",
                    "error": f"unexpected worker error: {error}",
                    "profile": {"status": "unknown"},
                    "checkpoint": {"status": "unknown"},
                    "wall_seconds": 0,
                }
            results.append(result)
            report["workloads"] = sorted(
                results, key=lambda item: item["workload_id"]
            )
            write_reports(args.output_root, report)

    report["finished_at"] = datetime.now().astimezone().isoformat()
    report["status"] = (
        "complete"
        if all(item["status"] == "complete" for item in results)
        else "failed"
    )
    write_reports(args.output_root, report)
    print(f"Batch report: {args.output_root / 'batch_report.md'}")
    return 0 if report["status"] == "complete" else 1


def main() -> int:
    args = build_parser().parse_args()
    try:
        return run(args)
    except (OSError, PipelineError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
