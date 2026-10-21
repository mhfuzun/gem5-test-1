#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Create one explicit workload manifest per SPEC CPU command."""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path
from typing import Any

from common import (
    DEFAULT_GUEST_WORKDIR,
    SCHEMA_VERSION,
    PipelineError,
    load_workload,
    write_json,
)

DEFAULT_ENVIRONMENT = {
    "LC_ALL": "C",
    "OMP_NUM_THREADS": "1",
    "OMP_THREAD_LIMIT": "1",
}
SPEC_PREFIX_OPTIONS = {"-i": "stdin", "-o": "stdout", "-e": "stderr"}
REDIRECTIONS = {"<", ">", ">>", "1>", "1>>", "2>", "2>>"}


def parse_environment(items: list[str]) -> dict[str, str]:
    environment = dict(DEFAULT_ENVIRONMENT)
    for item in items:
        if "=" not in item:
            raise PipelineError(f"--env expects NAME=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        if not key:
            raise PipelineError("environment variable name must not be empty")
        environment[key] = value
    return environment


def command_lines(path: Path) -> list[str]:
    lines = []
    contents = path.read_text(encoding="utf-8", errors="replace")
    for line in contents.splitlines():
        stripped = line.strip()
        if stripped.startswith(("-i ", "-o ", "-e ")):
            lines.append(stripped)
    if not lines:
        raise PipelineError(f"no SPEC commands found in {path}")
    return lines


def _remove_redirections(
    tokens: list[str], fields: dict[str, Any]
) -> list[str]:
    arguments = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in REDIRECTIONS:
            if index + 1 >= len(tokens):
                raise PipelineError(f"redirection {token!r} has no target")
            target = tokens[index + 1]
            if token == "<":
                fields["stdin"] = target
            elif token.startswith("2"):
                fields["stderr"] = target
            else:
                fields["stdout"] = target
            index += 2
            continue
        arguments.append(token)
        index += 1
    return arguments


def parse_spec_command(line: str, run_dir: Path) -> dict[str, Any]:
    tokens = shlex.split(line, posix=True)
    fields: dict[str, Any] = {"stdin": None, "stdout": None, "stderr": None}
    index = 0
    while index < len(tokens) and tokens[index] in SPEC_PREFIX_OPTIONS:
        option = tokens[index]
        if index + 1 >= len(tokens):
            raise PipelineError(f"SPEC option {option!r} has no value")
        fields[SPEC_PREFIX_OPTIONS[option]] = tokens[index + 1]
        index += 2
    if index >= len(tokens):
        raise PipelineError(f"SPEC command has no executable: {line}")

    executable_token = tokens[index]
    command_arguments = _remove_redirections(tokens[index + 1 :], fields)
    resolved_binary = (run_dir / executable_token).resolve()
    if not resolved_binary.is_file():
        raise PipelineError(
            f"SPEC command executable does not exist: {executable_token} "
            f"(resolved to {resolved_binary})"
        )
    try:
        relative_binary = resolved_binary.relative_to(run_dir).as_posix()
    except ValueError as error:
        raise PipelineError(
            "SPEC executable resolves outside the run directory; copy it into "
            f"the exported run directory first: {resolved_binary}"
        ) from error

    fields.update(
        {
            "binary": relative_binary,
            "guest_binary": f"{DEFAULT_GUEST_WORKDIR}/{relative_binary}",
            "arguments": command_arguments,
            "original_spec_command": line,
        }
    )
    return fields


def create_manifests(args: argparse.Namespace) -> int:
    run_dir = args.run_dir.expanduser().resolve()
    if not run_dir.is_dir():
        raise PipelineError(f"run directory does not exist: {run_dir}")
    commands_path = (
        args.commands.expanduser().resolve()
        if args.commands
        else run_dir / "speccmds.cmd"
    )
    if not commands_path.is_file():
        raise PipelineError(
            f"SPEC command file does not exist: {commands_path}"
        )

    parsed = [
        parse_spec_command(line, run_dir)
        for line in command_lines(commands_path)
    ]
    if args.command_index is not None:
        if not 0 <= args.command_index < len(parsed):
            raise PipelineError(
                f"command index {args.command_index} is outside "
                f"0..{len(parsed) - 1}"
            )
        selected = [(args.command_index, parsed[args.command_index])]
    else:
        selected = list(enumerate(parsed))

    output_dir = args.output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    environment = parse_environment(args.env)
    redirects = []
    interpreter_root = None
    if args.sysroot:
        sysroot = args.sysroot.expanduser().resolve()
        if not sysroot.is_dir():
            raise PipelineError(f"RISC-V sysroot does not exist: {sysroot}")
        interpreter_root = str(sysroot)
        redirects.append({"guest": "/", "host": str(sysroot)})
    for item in args.redirect:
        if "=" not in item:
            raise PipelineError(f"--redirect expects GUEST=HOST, got {item!r}")
        guest, host_text = item.split("=", 1)
        host = Path(host_text).expanduser().resolve()
        if not guest.startswith("/") or not host.exists():
            raise PipelineError(f"invalid path redirect: {item!r}")
        redirects.append({"guest": guest, "host": str(host)})
    created = []
    for index, command in selected:
        stem = Path(command["binary"]).name.split("_base", 1)[0]
        workload_id = f"{args.benchmark}.cmd{index:03d}.{stem}"
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "workload_id": workload_id,
            "benchmark": args.benchmark,
            "source_run_directory": str(run_dir),
            "binary": command["binary"],
            "guest_binary": command["guest_binary"],
            "arguments": command["arguments"],
            "stdin": command["stdin"],
            "stdout": command["stdout"],
            "stderr": command["stderr"],
            "environment": environment,
            "guest_working_directory": DEFAULT_GUEST_WORKDIR,
            "interpreter_root": interpreter_root,
            "path_redirects": redirects,
            "metadata": {
                "source": "SPEC CPU 2017 speccmds.cmd",
                "command_index": index,
                "command_count": len(parsed),
                "commands_file": str(commands_path),
                "original_spec_command": command["original_spec_command"],
            },
        }
        destination = output_dir / f"{workload_id}.json"
        if destination.exists() and not args.force:
            raise PipelineError(
                f"manifest already exists: {destination}; use --force to "
                "replace it"
            )
        write_json(destination, manifest)
        created.append(destination)
        print(f"[{index}] {destination}")
        print(
            "    "
            + shlex.join([command["guest_binary"]] + command["arguments"])
        )
    print(f"Created {len(created)} independent workload manifest(s).")
    return 0


def validate_manifest(args: argparse.Namespace) -> int:
    workload = load_workload(
        args.manifest, hash_source=not args.skip_source_hash
    )
    summary = {
        "workload_id": workload["workload_id"],
        "binary": workload["binary"],
        "arguments": workload["arguments"],
        "stdin": workload["stdin"],
        "stdout": workload["stdout"],
        "stderr": workload["stderr"],
        "environment": workload["environment"],
        "workload_fingerprint": workload["workload_fingerprint"],
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    create = subparsers.add_parser(
        "from-speccmds", help="create one manifest per speccmds.cmd command"
    )
    create.add_argument("--run-dir", type=Path, required=True)
    create.add_argument("--commands", type=Path)
    create.add_argument("--benchmark", required=True)
    create.add_argument("--output-dir", type=Path, required=True)
    create.add_argument("--command-index", type=int)
    create.add_argument(
        "--env", action="append", default=[], metavar="NAME=VALUE"
    )
    create.add_argument(
        "--sysroot",
        type=Path,
        help=(
            "host root containing the RISC-V /lib dynamic linker and "
            "libraries"
        ),
    )
    create.add_argument(
        "--redirect", action="append", default=[], metavar="GUEST=HOST"
    )
    create.add_argument("--force", action="store_true")
    create.set_defaults(function=create_manifests)

    validate = subparsers.add_parser("validate", help="validate one manifest")
    validate.add_argument("manifest", type=Path)
    validate.add_argument("--skip-source-hash", action="store_true")
    validate.set_defaults(function=validate_manifest)
    return parser


def main() -> int:
    try:
        args = build_parser().parse_args()
        return args.function(args)
    except (PipelineError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
