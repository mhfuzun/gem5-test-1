#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Count every prepared SPEC CPU2017 rate/ref command with QEMU.

Only private copies beneath inst-counts are writable in the QEMU namespace.
The source trees are fingerprinted before and after the batch. No shell is
used to interpret SPEC commands, and failed commands never supply totals.
"""

import argparse
import concurrent.futures
import fcntl
import hashlib
import json
import os
import queue
import re
import resource
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOTAL = re.compile(r"^total insns:\s*([0-9]+)\s*$", re.MULTILINE)
TERMINAL = {"ok", "failed"}
STOP = threading.Event()


def now():
    return datetime.now(timezone.utc).isoformat()


def save_json(path, data):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n")
    temporary.replace(path)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def local_name(name):
    path = Path(name)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise ValueError(f"Unsafe relative path: {name!r}")
    return path


@dataclass(frozen=True)
class Job:
    benchmark: str
    index: int
    source: Path
    argv: tuple
    stdin: str | None
    stdout: str
    stderr: str
    original: str

    @property
    def key(self):
        return f"{self.benchmark}/{self.index:03d}"


def parse_command(line, source, index):
    tokens = shlex.split(line)
    streams = {"-i": None, "-o": "stdout.txt", "-e": "stderr.txt"}
    while tokens and tokens[0] in streams:
        flag = tokens.pop(0)
        streams[flag] = str(local_name(tokens.pop(0)))
    redirect = next(
        (
            i
            for i, token in enumerate(tokens)
            if token in {"<", ">", ">>", "2>", "2>>"}
        ),
        len(tokens),
    )
    argv, tail = tokens[:redirect], tokens[redirect:]
    if not argv:
        raise ValueError(f"Empty command: {line}")
    for i in range(0, len(tail), 2):
        operator, filename = tail[i : i + 2]
        flag = {"<": "-i", ">": "-o", ">>": "-o", "2>": "-e", "2>>": "-e"}[
            operator
        ]
        if filename != streams[flag]:
            raise ValueError(f"SPEC redirections disagree: {line}")
    if any(token in {"|", "||", "&&", ";", "&"} for token in argv):
        raise ValueError(f"Unsupported shell operator: {line}")
    executable = source / Path(argv[0]).name
    if not executable.is_file():
        raise ValueError(f"Missing executable: {executable}")
    argv[0] = "./" + executable.name
    return Job(
        source.parent.name,
        index,
        source,
        tuple(argv),
        streams["-i"],
        streams["-o"],
        streams["-e"],
        line,
    )


def discover(root):
    jobs = []
    for benchmark in sorted(root.iterdir()):
        if not benchmark.is_dir() or not re.fullmatch(
            r"5[0-9]{2}\..+_r", benchmark.name
        ):
            continue
        index = 0
        for commands in sorted(benchmark.glob("run*/speccmds.cmd")):
            for line in commands.read_text().splitlines():
                if line.startswith(("-i ", "-o ", "-e ")):
                    jobs.append(parse_command(line, commands.parent, index))
                    index += 1
        if not index:
            raise ValueError(f"No commands found: {benchmark}")
    if not jobs:
        raise ValueError(f"No 5xx.*_r commands under {root}")
    return jobs


def fingerprint(jobs, root):
    files = {}
    sources = sorted({job.source for job in jobs})
    for source in sources:
        for path in sorted(source.rglob("*")):
            if path.is_symlink():
                raise ValueError(f"Source symlink is not supported: {path}")
            stat = path.stat()
            entry = {"mode": stat.st_mode, "mtime_ns": stat.st_mtime_ns}
            if path.is_file():
                entry.update(size=stat.st_size, sha256=digest(path))
            files[str(path.relative_to(root))] = entry
    return files


def cpu_list(count):
    available = sorted(os.sched_getaffinity(0))
    physical = {}
    for cpu in available:
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        key = tuple(
            (topology / name).read_text().strip()
            for name in ("physical_package_id", "core_id")
        )
        physical.setdefault(key, cpu)
    ordered = list(physical.values())
    ordered.extend(cpu for cpu in available if cpu not in ordered)
    if count > len(ordered):
        raise ValueError(f"Requested {count} CPUs; available: {len(ordered)}")
    return ordered[:count]


def qemu_argv(args, executable, plugin_log):
    return [
        str(args.qemu),
        "-L",
        str(args.sysroot),
        "-plugin",
        f"{args.plugin},inline=true",
        "-d",
        "plugin",
        "-D",
        str(plugin_log),
        *executable,
    ]


def isolate(command, directory, cpu):
    return [
        "taskset",
        "-c",
        str(cpu),
        "bwrap",
        "--die-with-parent",
        "--unshare-net",
        "--ro-bind",
        "/",
        "/",
        "--proc",
        "/proc",
        "--dev",
        "/dev",
        "--bind",
        str(directory),
        str(directory),
        "--bind",
        str(directory / "tmp"),
        "/tmp",
        "--chdir",
        str(directory / "work"),
        "--",
        *command,
    ]


def execute(job, args, cpu, smoke=False):
    directory = args.output / job.key
    directory.mkdir(parents=True, exist_ok=False)
    record = {
        "benchmark": job.benchmark,
        "index": job.index,
        "status": "staging",
        "source": str(job.source),
        "original_spec_line": job.original,
        "command": shlex.join(job.argv),
        "stdin": job.stdin,
        "stdout": job.stdout,
        "stderr": job.stderr,
        "cpu": cpu,
        "started": now(),
        "instructions": None,
    }
    started = time.monotonic()
    result_path = directory / "result.json"
    save_json(result_path, record)
    try:
        # Full copies, never hardlinks or writable symlinks to source inputs.
        shutil.copytree(job.source, directory / "work")
        (directory / "tmp").mkdir()
        if smoke:
            (directory / "work" / "_smoke.c").write_text(
                "int main(void) { return 42; }\n"
            )
        executable = [str(directory / "work" / job.argv[0]), *job.argv[1:]]
        command = qemu_argv(args, executable, directory / "qemu.plugin.log")
        sandbox_command = isolate(command, directory, cpu)
        record["qemu_argv"] = command
        record["sandbox_argv"] = sandbox_command
        invocation = shlex.join(sandbox_command)
        if job.stdin:
            invocation += " < " + shlex.quote(
                str(directory / "work" / job.stdin)
            )
        invocation += " > " + shlex.quote(str(directory / job.stdout))
        invocation += " 2> " + shlex.quote(str(directory / job.stderr))
        (directory / "command.txt").write_text(invocation + "\n")
        env = dict(
            os.environ,
            LC_ALL="C",
            OMP_NUM_THREADS="1",
            OMP_THREAD_LIMIT="1",
            TMPDIR="/tmp",
        )
        for key in ("QEMU_PLUGIN", "QEMU_LOG", "QEMU_LOG_FILENAME"):
            env.pop(key, None)
        if STOP.is_set():
            raise InterruptedError("Batch was interrupted")
        stdin_path = directory / "work" / job.stdin if job.stdin else None
        for name in (job.stdout, job.stderr):
            (directory / name).parent.mkdir(parents=True, exist_ok=True)
        with (
            (
                stdin_path.open("rb") if stdin_path else open(os.devnull, "rb")
            ) as input_file,
            (directory / job.stdout).open("wb") as output_file,
            (directory / job.stderr).open("wb") as error_file,
        ):
            process = subprocess.Popen(
                sandbox_command,
                stdin=input_file,
                stdout=output_file,
                stderr=error_file,
                env=env,
                start_new_session=True,
            )
            record.update(status="running", pid=process.pid)
            save_json(result_path, record)
            deadline = started + args.timeout if args.timeout else None
            while process.poll() is None:
                if STOP.is_set() or (deadline and time.monotonic() > deadline):
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                    raise InterruptedError("Interrupted or timed out")
                STOP.wait(0.5)
            record["returncode"] = process.returncode
        plugin_log = directory / "qemu.plugin.log"
        counts = (
            TOTAL.findall(plugin_log.read_text())
            if (plugin_log.exists())
            else []
        )
        if len(counts) == 1:
            record["observed_instructions"] = int(counts[0])
        if process.returncode:
            raise RuntimeError(f"QEMU exited with {process.returncode}")
        if len(counts) != 1 or int(counts[0]) <= 0:
            raise RuntimeError("Expected exactly one positive total insns")
        if smoke and not (directory / "work" / "_smoke.s").is_file():
            raise RuntimeError("Smoke compiler did not produce assembly")
        record.update(status="ok", instructions=int(counts[0]))
    except Exception as error:
        status = "interrupted" if STOP.is_set() else "failed"
        record.update(status=status, error=f"{type(error).__name__}: {error}")
    record.update(finished=now(), seconds=round(time.monotonic() - started, 3))
    save_json(result_path, record)
    return record


def write_report(args, jobs, results, phase, integrity=None):
    lines = [
        f"SPEC CPU2017 rate/ref instruction counts — {phase}",
        f"Updated (UTC): {now()}",
        "Counts include guest userspace libraries and the loader.",
        "Failed/unfinished commands are excluded from totals.",
        "No SPEC output comparison/score validation is performed.",
        "",
        "BENCHMARK\tCOMMAND_INDEX\tSTATUS\tTOTAL_INSNS\tCOMMAND",
    ]
    for job in jobs:
        record = results.get(job.key, {})
        count = record.get("instructions")
        command = shlex.join(job.argv)
        if job.stdin:
            command += " < " + shlex.quote(job.stdin)
        command += " > " + shlex.quote(job.stdout)
        command += " 2> " + shlex.quote(job.stderr)
        lines.append(
            "\t".join(
                (
                    job.benchmark,
                    f"{job.index:03d}",
                    record.get("status", "pending"),
                    str(count) if count is not None else "-",
                    command,
                )
            )
        )
    lines.extend(
        [
            "",
            "BENCHMARK TOTALS (all commands must pass)",
            "BENCHMARK\tOK/ALL\tSTATUS\tSUCCESSFUL_INSNS",
        ]
    )
    for benchmark in sorted({job.benchmark for job in jobs}):
        group = [
            results.get(job.key, {})
            for job in jobs
            if job.benchmark == benchmark
        ]
        passed = [record for record in group if record.get("status") == "ok"]
        status = "complete" if len(passed) == len(group) else "incomplete"
        lines.append(
            f"{benchmark}\t{len(passed)}/{len(group)}\t{status}\t"
            f"{sum(record['instructions'] for record in passed)}"
        )
    passed = [
        record for record in results.values() if record.get("status") == "ok"
    ]
    failed = [
        record
        for record in results.values()
        if record.get("status") == "failed"
    ]
    lines.extend(
        [
            "",
            f"Successful commands: {len(passed)}/{len(jobs)}",
            f"Failed commands: {len(failed)}",
            "Successful instruction sum: "
            + str(sum(record["instructions"] for record in passed)),
        ]
    )
    if integrity is not None:
        lines.append(f"Source integrity: {integrity}")
    for record in failed:
        lines.append(
            f"ERROR {record['benchmark']}/{record['index']:03d}: "
            f"{record.get('error', 'unknown failure')}"
        )
    temporary = args.report.with_suffix(".txt.tmp")
    temporary.write_text("\n".join(lines) + "\n")
    temporary.replace(args.report)


def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    lock = (args.output / "runner.lock").open("w")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    (args.output / "runner.pid").write_text(f"{os.getpid()}\n")
    # Avoid gigabyte-sized QEMU core dumps from failing benchmarks.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: STOP.set())
    for binary in ("taskset", "bwrap"):
        if not shutil.which(binary):
            raise ValueError(f"Required executable missing: {binary}")
    for path in (args.qemu, args.plugin, args.sysroot):
        if not path.exists():
            raise ValueError(f"Required path missing: {path}")
    jobs = discover(args.root)
    cpus = cpu_list(args.jobs)
    configuration = {
        "qemu": str(args.qemu),
        "qemu_sha256": digest(args.qemu),
        "plugin": str(args.plugin),
        "plugin_sha256": digest(args.plugin),
        "sysroot": str(args.sysroot),
        "source_root": str(args.root),
        "jobs": args.jobs,
        "cpus": cpus,
    }
    print(
        f"{now()} Found {len(jobs)} commands in "
        f"{len({job.benchmark for job in jobs})} benchmarks; CPUs {cpus}",
        flush=True,
    )
    before = fingerprint(jobs, args.root)
    before_path = args.output / "source-before.json"
    config_path = args.output / "configuration.json"
    if before_path.exists():
        if not args.resume:
            raise ValueError("Existing batch: use --resume or another output")
        if json.loads(before_path.read_text()) != before:
            raise ValueError("Source tree differs from the original snapshot")
        if json.loads(config_path.read_text()) != configuration:
            raise ValueError("Configuration differs from the original batch")
    else:
        save_json(before_path, before)
        save_json(config_path, configuration)
    smoke_source = next(
        job.source for job in jobs if job.benchmark == "502.gcc_r"
    )
    smoke_job = Job(
        "_smoke",
        0,
        smoke_source,
        ("./cpugcc_r_base.mytest", "_smoke.c", "-O0", "-o", "_smoke.s"),
        None,
        "stdout.txt",
        "stderr.txt",
        "Compile a tiny C file with the prepared RISC-V cpugcc",
    )
    smoke_result = args.output / smoke_job.key / "result.json"
    if args.resume and smoke_result.exists():
        smoke = json.loads(smoke_result.read_text())
    else:
        smoke = execute(smoke_job, args, cpus[0], smoke=True)
    if smoke["status"] != "ok":
        raise RuntimeError(f"Smoke failed: {smoke.get('error')}")
    print(
        f"{now()} Smoke OK: {smoke['instructions']} instructions", flush=True
    )
    if args.smoke_only:
        after = fingerprint(jobs, args.root)
        save_json(args.output / "source-after-smoke.json", after)
        if after != before:
            raise RuntimeError("Source integrity check failed after smoke")
        return 0
    results = {}
    pending = []
    for job in jobs:
        directory = args.output / job.key
        result_path = directory / "result.json"
        if args.resume and result_path.exists():
            record = json.loads(result_path.read_text())
            if record["status"] in TERMINAL:
                results[job.key] = record
                continue
        if directory.exists():
            if not args.resume:
                raise ValueError(f"Refusing to overwrite: {directory}")
            archive = args.output / "_interrupted" / job.benchmark
            archive.mkdir(parents=True, exist_ok=True)
            directory.rename(archive / f"{job.index:03d}-{time.time_ns()}")
        pending.append(job)
    cpu_queue = queue.Queue()
    for cpu in cpus:
        cpu_queue.put(cpu)

    def worker(job):
        if STOP.is_set():
            return None
        cpu = cpu_queue.get()
        try:
            print(f"{now()} START {job.key} CPU {cpu}", flush=True)
            return execute(job, args, cpu)
        finally:
            cpu_queue.put(cpu)

    write_report(args, jobs, results, "running")
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as executor:
        futures = {executor.submit(worker, job): job for job in pending}
        remaining = set(futures)
        while remaining:
            done, remaining = concurrent.futures.wait(
                remaining,
                timeout=15,
                return_when=concurrent.futures.FIRST_COMPLETED,
            )
            for future in done:
                job = futures[future]
                record = future.result()
                if record is not None:
                    results[job.key] = record
                    print(
                        f"{now()} {record['status'].upper()} {job.key} "
                        f"insns={record['instructions']} "
                        f"seconds={record['seconds']}",
                        flush=True,
                    )
            current = dict(results)
            for future in remaining:
                job = futures[future]
                path = args.output / job.key / "result.json"
                if path.exists():
                    current[job.key] = json.loads(path.read_text())
            write_report(args, jobs, current, "running")
    after = fingerprint(jobs, args.root)
    save_json(args.output / "source-after.json", after)
    integrity = "UNCHANGED" if before == after else "CHANGED"
    phase = "interrupted" if STOP.is_set() else "finished"
    write_report(args, jobs, results, phase, integrity)
    summary = {
        "finished": now(),
        "phase": phase,
        "source_integrity": integrity,
        "commands": len(jobs),
        "ok": sum(record["status"] == "ok" for record in results.values()),
        "failed": sum(
            record["status"] == "failed" for record in results.values()
        ),
        "report": str(args.report),
    }
    save_json(args.output / "summary.json", summary)
    print(json.dumps(summary), flush=True)
    return 0 if integrity == "UNCHANGED" and not STOP.is_set() else 1


def show_status(args):
    jobs = discover(args.root)
    counts = {"pending": 0}
    for job in jobs:
        path = args.output / job.key / "result.json"
        record = json.loads(path.read_text()) if path.exists() else {}
        status = record.get("status", "pending")
        counts[status] = counts.get(status, 0) + 1
        if status != "pending":
            print(
                f"{job.key}\t{status}\t"
                f"{record.get('instructions') or '-'}\t"
                f"{record.get('error', '')}"
            )
    pid_path = args.output / "runner.pid"
    if pid_path.exists():
        pid = int(pid_path.read_text())
        # PIDs may be hidden by the caller's PID namespace; use the lock.
        with (args.output / "runner.lock").open("r") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                active = False
                fcntl.flock(lock, fcntl.LOCK_UN)
            except BlockingIOError:
                active = True
        print(f"Controller host PID: {pid}; lock active: {active}")
    print(json.dumps(counts, sort_keys=True))
    print(f"Report: {args.report}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=REPO / "research-out/spec-runs/ref"
    )
    parser.add_argument(
        "--qemu",
        type=Path,
        default=Path.home() / "github_repos/qemu-count/build/qemu-riscv64",
    )
    parser.add_argument(
        "--plugin",
        type=Path,
        default=Path.home()
        / "github_repos/qemu-count/build/tests/tcg/plugins/"
        "libinsn.so",
    )
    parser.add_argument(
        "--sysroot", type=Path, default=Path("/usr/riscv64-linux-gnu")
    )
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument(
        "--timeout",
        type=float,
        default=0,
        help="Optional seconds per command; 0 = unlimited",
    )
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--smoke-only", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--background", action="store_true")
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.output = (args.output or args.root / "inst-counts").resolve()
    args.report = (args.report or args.root / "inst-counts.txt").resolve()
    for name in ("qemu", "plugin", "sysroot"):
        setattr(args, name, getattr(args, name).resolve())
    if args.jobs < 1 or args.timeout < 0:
        parser.error("jobs must be positive; timeout must be nonnegative")
    # Neither metadata nor copies may be written inside any source tree.
    for benchmark in args.root.glob("5*_r"):
        for path in (args.output, args.report):
            if path.is_relative_to(benchmark.resolve()):
                parser.error(f"Output is inside a benchmark source: {path}")
    if args.list:
        for job in discover(args.root):
            print(job.key, shlex.join(job.argv), sep="\t")
        return 0
    if args.status:
        return show_status(args)
    if args.background:
        args.output.mkdir(parents=True, exist_ok=True)
        child_args = [arg for arg in sys.argv[1:] if arg != "--background"]
        with (args.output / "runner.log").open("ab") as log:
            child = subprocess.Popen(
                [
                    sys.executable,
                    "-u",
                    str(Path(__file__).resolve()),
                    *child_args,
                ],
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        print(f"Started PID {child.pid}; log: {args.output / 'runner.log'}")
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
