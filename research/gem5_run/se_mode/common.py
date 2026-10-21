#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Shared host-side support for the RISC-V SE SimPoint pipeline."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import (
    Any,
    Iterable,
    Mapping,
    Sequence,
)

SCHEMA_VERSION = 1
DEFAULT_INTERVAL = 200_000_000
DEFAULT_WARMUP = 50_000_000
DEFAULT_MEMORY_SIZE = "2GiB"
DEFAULT_CLOCK = "3GHz"
DEFAULT_GUEST_WORKDIR = "/work"


class PipelineError(RuntimeError):
    """An actionable pipeline validation or execution error."""


def parse_instruction_count(value: str) -> int:
    """Parse an instruction count, accepting K/M/G decimal suffixes."""
    text = value.strip().lower().replace("_", "")
    multiplier = 1
    if text.endswith(("k", "m", "g")):
        multiplier = {"k": 1_000, "m": 1_000_000, "g": 1_000_000_000}[text[-1]]
        text = text[:-1]
    try:
        number = int(text)
    except ValueError as error:
        raise ValueError(f"invalid instruction count: {value!r}") from error
    number *= multiplier
    if number < 0:
        raise ValueError("instruction count must not be negative")
    return number


def read_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise PipelineError(f"JSON file does not exist: {path}") from error
    except json.JSONDecodeError as error:
        raise PipelineError(f"invalid JSON in {path}: {error}") from error
    if not isinstance(data, dict):
        raise PipelineError(f"top-level JSON value must be an object: {path}")
    return data


def write_json(path: Path, data: Mapping[str, Any]) -> None:
    """Atomically write a stable, human-readable JSON document."""
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(data, indent=2, sort_keys=True) + "\n"
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False
    ) as output:
        output.write(payload)
        temporary = Path(output.name)
    os.replace(temporary, path)


def mark_manifest_failed(path: Path, error: Exception) -> None:
    """Mark an already-created manifest failed without hiding the error."""
    if not path.is_file():
        return
    try:
        manifest = read_json(path)
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        write_json(path, manifest)
    except (OSError, PipelineError):
        return


def canonical_hash(value: Any) -> str:
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def directory_digest(directory: Path) -> str:
    """Hash all regular files and symlink targets in a workload directory."""
    digest = hashlib.sha256()
    for path in sorted(directory.rglob("*"), key=lambda item: item.as_posix()):
        relative = path.relative_to(directory).as_posix().encode("utf-8")
        if path.is_symlink():
            digest.update(b"L\0" + relative + b"\0")
            digest.update(os.readlink(path).encode("utf-8") + b"\0")
        elif path.is_file():
            digest.update(b"F\0" + relative + b"\0")
            mode = str(path.stat().st_mode & 0o777).encode("ascii")
            digest.update(mode + b"\0")
            with path.open("rb") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(block)
    return digest.hexdigest()


def _require_string(data: Mapping[str, Any], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value:
        raise PipelineError(
            f"workload field {key!r} must be a non-empty string"
        )
    return value


def load_workload(path: Path, *, hash_source: bool = True) -> dict[str, Any]:
    """Load, normalize, and validate a workload manifest."""
    data = read_json(path.resolve())
    if data.get("schema_version") != SCHEMA_VERSION:
        raise PipelineError(
            f"unsupported workload schema_version in {path}; "
            f"expected {SCHEMA_VERSION}"
        )

    workload_id = _require_string(data, "workload_id")
    if not all(
        character.isalnum() or character in "._-" for character in workload_id
    ):
        raise PipelineError(
            "workload_id may contain only letters, digits, '.', '_' and '-'"
        )

    source_dir = Path(
        _require_string(data, "source_run_directory")
    ).expanduser()
    if not source_dir.is_absolute():
        source_dir = (path.resolve().parent / source_dir).resolve()
    else:
        source_dir = source_dir.resolve()
    if not source_dir.is_dir():
        raise PipelineError(
            f"source_run_directory is not a directory: {source_dir}"
        )

    binary = _require_string(data, "binary")
    binary_path = Path(binary)
    source_binary = (
        binary_path.resolve()
        if binary_path.is_absolute()
        else (source_dir / binary_path).resolve()
    )
    if not source_binary.is_file():
        raise PipelineError(f"workload binary does not exist: {source_binary}")

    arguments = data.get("arguments", [])
    if not isinstance(arguments, list) or not all(
        isinstance(argument, str) for argument in arguments
    ):
        raise PipelineError("workload arguments must be a list of strings")

    environment = data.get("environment", {})
    if not isinstance(environment, dict) or not all(
        isinstance(key, str) and isinstance(value, str)
        for key, value in environment.items()
    ):
        raise PipelineError(
            "workload environment must be a string-to-string object"
        )

    guest_workdir = data.get("guest_working_directory", DEFAULT_GUEST_WORKDIR)
    if not isinstance(guest_workdir, str) or not guest_workdir.startswith("/"):
        raise PipelineError("guest_working_directory must be an absolute path")

    interpreter_root = data.get("interpreter_root")
    if interpreter_root is not None:
        if not isinstance(interpreter_root, str):
            raise PipelineError(
                "interpreter_root must be an absolute directory or null"
            )
        interpreter_path = Path(interpreter_root).expanduser()
        if not interpreter_path.is_absolute() or not interpreter_path.is_dir():
            raise PipelineError(
                f"interpreter_root must be an existing absolute directory: "
                f"{interpreter_path}"
            )
        interpreter_root = str(interpreter_path.resolve())

    for key in ("stdin", "stdout", "stderr"):
        value = data.get(key)
        if value is not None and not isinstance(value, str):
            raise PipelineError(
                f"workload field {key!r} must be a string or null"
            )
        if key == "stdin" and value is not None:
            input_path = Path(value)
            if not input_path.is_absolute():
                input_path = source_dir / input_path
            if not input_path.is_file():
                raise PipelineError(f"stdin file does not exist: {input_path}")

    redirects = data.get("path_redirects", [])
    if not isinstance(redirects, list):
        raise PipelineError("path_redirects must be a list")
    normalized_redirects: list[dict[str, str]] = []
    for redirect in redirects:
        if not isinstance(redirect, dict):
            raise PipelineError("each path_redirects entry must be an object")
        guest = redirect.get("guest")
        host = redirect.get("host")
        if not isinstance(guest, str) or not guest.startswith("/"):
            raise PipelineError("redirect guest path must be absolute")
        if (
            not isinstance(host, str)
            or not Path(host).expanduser().is_absolute()
        ):
            raise PipelineError("redirect host path must be absolute")
        host_path = Path(host).expanduser().resolve()
        if not host_path.exists():
            raise PipelineError(
                f"redirect host path does not exist: {host_path}"
            )
        normalized_redirects.append({"guest": guest, "host": str(host_path)})

    normalized = dict(data)
    normalized.update(
        {
            "schema_version": SCHEMA_VERSION,
            "source_run_directory": str(source_dir),
            "binary": binary,
            "arguments": arguments,
            "environment": dict(sorted(environment.items())),
            "guest_working_directory": guest_workdir.rstrip("/") or "/",
            "path_redirects": normalized_redirects,
            "interpreter_root": interpreter_root,
            "stdin": data.get("stdin"),
            "stdout": data.get("stdout"),
            "stderr": data.get("stderr"),
        }
    )
    if "guest_binary" not in normalized:
        if binary_path.is_absolute():
            normalized["guest_binary"] = binary
        else:
            normalized["guest_binary"] = (
                normalized["guest_working_directory"]
                + "/"
                + Path(binary).as_posix().lstrip("./")
            )
    if not isinstance(normalized["guest_binary"], str) or not normalized[
        "guest_binary"
    ].startswith("/"):
        raise PipelineError("guest_binary must be an absolute path")

    identity = {
        key: normalized[key]
        for key in (
            "schema_version",
            "workload_id",
            "binary",
            "guest_binary",
            "arguments",
            "stdin",
            "stdout",
            "stderr",
            "environment",
            "guest_working_directory",
            "path_redirects",
            "interpreter_root",
        )
    }
    identity["source_tree_sha256"] = (
        directory_digest(source_dir) if hash_source else None
    )
    normalized["source_tree_sha256"] = identity["source_tree_sha256"]
    normalized["workload_fingerprint"] = canonical_hash(identity)
    normalized["manifest_path"] = str(path.resolve())
    return normalized


def resolve_stage_path(value: str | None, stage_workdir: Path) -> Path | None:
    if value is None:
        return None
    path = Path(value)
    return path if path.is_absolute() else stage_workdir / path


def stage_binary_path(
    workload: Mapping[str, Any], stage_workdir: Path
) -> Path:
    binary = Path(str(workload["binary"]))
    return binary if binary.is_absolute() else stage_workdir / binary


def prepare_output_directory(path: Path, *, force: bool) -> None:
    path = path.resolve()
    if path.exists():
        if not force:
            raise PipelineError(
                f"output already exists: {path}; choose another path or use "
                "--force"
            )
        if path == Path("/") or len(path.parts) < 4:
            raise PipelineError(
                f"refusing to remove unsafe output path: {path}"
            )
        shutil.rmtree(path)
    path.mkdir(parents=True)


def prepare_stage_workdir(
    workload: Mapping[str, Any],
    destination: Path,
    *,
    source_override: Path | None = None,
) -> None:
    source = (
        source_override.resolve()
        if source_override is not None
        else Path(str(workload["source_run_directory"])).resolve()
    )
    if not source.is_dir():
        raise PipelineError(f"stage source is not a directory: {source}")
    destination = destination.resolve()
    try:
        destination.relative_to(source)
    except ValueError:
        pass
    else:
        raise PipelineError(
            "stage work directory must not be inside the source run directory"
        )
    if destination.exists():
        raise PipelineError(
            f"stage work directory already exists: {destination}"
        )
    shutil.copytree(source, destination, symlinks=True)


def executable(path: Path, description: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise PipelineError(f"{description} is not executable: {resolved}")
    return resolved


def run_logged(
    command: Sequence[str],
    log_path: Path,
    *,
    cwd: Path | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    """Run a command without a shell while teeing merged output to a log."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        log.write("COMMAND: " + json.dumps(list(command)) + "\n")
        log.flush()
        process = subprocess.Popen(
            list(command),
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(line)
            log.write(line)
        return_code = process.wait()
    result = subprocess.CompletedProcess(
        list(command), return_code, stdout=None, stderr=None
    )
    if check and return_code:
        raise PipelineError(
            f"command failed with exit status {return_code}; see {log_path}"
        )
    return result


def gem5_command(
    gem5_binary: Path,
    output_dir: Path,
    runtime_config: Path,
    mode: str,
) -> list[str]:
    config_script = Path(__file__).with_name("gem5_config.py").resolve()
    return [
        str(gem5_binary),
        "-d",
        str(output_dir.resolve()),
        str(config_script),
        mode,
        "--config",
        str(runtime_config.resolve()),
    ]


def git_revision(repository: Path) -> str | None:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repository,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else None


def tool_record(path: Path) -> dict[str, Any]:
    return {
        "path": str(path.resolve()),
        "sha256": sha256_file(path.resolve()),
    }


def gem5_record(path: Path) -> dict[str, Any]:
    record = tool_record(path)
    repository = Path(__file__).resolve().parents[3]
    record["repository_commit"] = git_revision(repository)
    result = subprocess.run(
        [str(path.resolve()), "--build-info"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode == 0:
        for line in result.stdout.splitlines():
            if line.startswith("gem5 version "):
                record["version"] = line.removeprefix("gem5 version ")
            elif line.startswith("compiled "):
                record["compiled"] = line.removeprefix("compiled ")
    return record


def parse_simpoint_outputs(
    simpoints_path: Path,
    weights_path: Path,
    *,
    interval_count: int | None = None,
) -> list[dict[str, Any]]:
    points: dict[int, int] = {}
    for line_number, line in enumerate(
        simpoints_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) < 2:
            raise PipelineError(f"malformed {simpoints_path}:{line_number}")
        interval_index, cluster_id = int(fields[0]), int(fields[1])
        if cluster_id in points:
            raise PipelineError(
                f"duplicate cluster {cluster_id} in {simpoints_path}"
            )
        if interval_index < 0 or (
            interval_count is not None and interval_index >= interval_count
        ):
            raise PipelineError(
                f"SimPoint interval {interval_index} is outside the BBV "
                "profile"
            )
        points[cluster_id] = interval_index

    weights: dict[int, float] = {}
    for line_number, line in enumerate(
        weights_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) < 2:
            raise PipelineError(f"malformed {weights_path}:{line_number}")
        weight, cluster_id = float(fields[0]), int(fields[1])
        if cluster_id in weights:
            raise PipelineError(
                f"duplicate cluster {cluster_id} in {weights_path}"
            )
        if weight < 0:
            raise PipelineError(
                f"negative SimPoint weight for cluster {cluster_id}"
            )
        weights[cluster_id] = weight

    if not points or points.keys() != weights.keys():
        raise PipelineError(
            "SimPoint and weight cluster IDs are empty or mismatched"
        )
    weight_sum = sum(weights.values())
    if abs(weight_sum - 1.0) > 1e-4:
        raise PipelineError(
            f"SimPoint weights sum to {weight_sum}, not approximately 1"
        )

    return sorted(
        (
            {
                "cluster_id": cluster_id,
                "interval_index": interval_index,
                "weight": weights[cluster_id],
            }
            for cluster_id, interval_index in points.items()
        ),
        key=lambda item: item["interval_index"],
    )


def count_bbv_intervals(path: Path) -> int:
    import gzip

    try:
        with gzip.open(path, "rt", encoding="utf-8") as profile:
            return sum(1 for line in profile if line.startswith("T"))
    except OSError as error:
        raise PipelineError(
            f"invalid compressed BBV file {path}: {error}"
        ) from error


def parse_key_value(items: Iterable[str], *, option: str) -> dict[str, Any]:
    """Parse NAME=JSON-or-string options into a dictionary."""
    values: dict[str, Any] = {}
    for item in items:
        if "=" not in item:
            raise PipelineError(f"{option} expects NAME=VALUE, got {item!r}")
        key, raw_value = item.split("=", 1)
        if not key or key in values:
            raise PipelineError(f"invalid or duplicate {option} key: {key!r}")
        try:
            values[key] = json.loads(raw_value)
        except json.JSONDecodeError:
            values[key] = raw_value
    return values


def manifest_contract(data: Mapping[str, Any], fields: Sequence[str]) -> str:
    return canonical_hash({field: data[field] for field in fields})
