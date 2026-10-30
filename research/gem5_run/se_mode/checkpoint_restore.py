# SPDX-License-Identifier: BSD-3-Clause
"""Create private checkpoint views with resolved file-backed VMA paths.

Some gem5 versions reopen serialized VMA names directly on the host,
bypassing Process path redirects. Keep the original checkpoint immutable
and resolve only those names in a per-run copy of m5.cpt.
"""

from __future__ import annotations

import re
from pathlib import Path

from common import (
    PipelineError,
    sha256_file,
)

RESTORE_POLICY = "private_vma_paths_v1"


def _under(path: str, directory: str) -> str | None:
    prefix = directory.rstrip("/") + "/"
    return path[len(prefix) :] if path.startswith(prefix) else None


def resolve_backing_file(name, checkpoint, workload, workdir):
    """Resolve guest and saved stage paths to this run's host filesystem."""
    if not name:
        raise PipelineError("file-backed VMA has an empty name")
    guest_cwd = workload["guest_working_directory"]
    absolute = (
        name if name.startswith("/") else guest_cwd.rstrip("/") + "/" + name
    )
    # Saved host paths must follow the private filesystem copy as well.
    stage_paths = [
        checkpoint.get("checkpoint_stage_workdir"),
        checkpoint.get("filesystem_snapshot"),
        workload.get("source_run_directory"),
        guest_cwd,
    ]
    for directory in stage_paths:
        if directory:
            tail = _under(absolute, directory)
            if tail is not None:
                return Path(workdir) / tail

    redirects = workload.get("path_redirects", [])
    # An older checkpoint may already contain a resolved sysroot host path.
    # Do not apply a guest '/' redirect to it a second time.
    for redirect in redirects:
        if _under(absolute, redirect["host"]) is not None:
            return Path(absolute)
    for redirect in redirects:
        tail = _under(absolute, redirect["guest"])
        if tail is not None:
            return Path(redirect["host"]) / tail
    return Path(absolute)


def rewrite_vma_paths(checkpoint, workload, workdir):
    """Return m5.cpt text and an audit trail without changing any files."""
    source = Path(checkpoint["checkpoint_path"]) / "m5.cpt"
    with source.open(encoding="utf-8", newline="") as stream:
        lines = stream.readlines()
    sections = []
    for index, line in enumerate(lines):
        header = re.fullmatch(r"\[([^\]]+)\]", line.strip())
        if header:
            sections.append((header[1], index))
    mappings = []
    for ordinal, (section, start) in enumerate(sections):
        if not re.search(r"(?:^|\.)vmalist\.Vma\d+$", section):
            continue
        end = (
            sections[ordinal + 1][1]
            if ordinal + 1 < len(sections)
            else len(lines)
        )
        fields = {}
        for index in range(start + 1, end):
            key, separator, value = lines[index].partition("=")
            if separator and key.strip() in ("name", "fileOffset"):
                fields[key.strip()] = (index, value.strip())
        if "fileOffset" not in fields:
            continue
        if "name" not in fields:
            raise PipelineError(f"file-backed VMA is missing name: {section}")
        name_index, name = fields["name"]
        host_path = resolve_backing_file(name, checkpoint, workload, workdir)
        try:
            # Open for reading, just as gem5 will do. Do not fall back to
            # host libraries when the configured guest library is missing.
            with host_path.open("rb"):
                pass
            if not host_path.is_file():
                raise OSError("not a regular file")
        except OSError as error:
            raise PipelineError(
                f"VMA backing file cannot be read: {name!r} -> "
                f"{str(host_path)!r} ({section}): {error}"
            ) from error
        old_line = lines[name_index]
        newline = (
            "\r\n"
            if old_line.endswith("\r\n")
            else "\n" if old_line.endswith("\n") else ""
        )
        lines[name_index] = (
            old_line.split("=", 1)[0] + "=" + str(host_path) + newline
        )
        mappings.append(
            {
                "section": section,
                "checkpoint_name": name,
                "host_path": str(host_path),
                "file_offset": int(fields["fileOffset"][1]),
            }
        )
    return "".join(lines), mappings


def prepare_restore_checkpoint(checkpoint, workload, workdir, destination):
    """Copy checkpoint metadata and reference original memory read-only."""
    source = Path(checkpoint["checkpoint_path"]).resolve()
    destination = Path(destination).resolve()
    if destination == source or source in destination.parents:
        raise PipelineError(
            "restore view must be outside the source checkpoint"
        )
    contents, mappings = rewrite_vma_paths(checkpoint, workload, workdir)
    destination.mkdir(parents=True, exist_ok=False)
    with (destination / "m5.cpt").open(
        "x", encoding="utf-8", newline=""
    ) as stream:
        stream.write(contents)
    # gem5 opens physical-memory checkpoint blobs for reading. Symlinks
    # avoid a multi-GiB copy for every independent restore job.
    for attachment in source.iterdir():
        if attachment.name != "m5.cpt":
            (destination / attachment.name).symlink_to(attachment)
    return {
        "schema_version": 1,
        "policy": RESTORE_POLICY,
        "source_checkpoint": str(source),
        "restore_checkpoint": str(destination),
        "source_m5_cpt_sha256": sha256_file(source / "m5.cpt"),
        "restore_m5_cpt_sha256": sha256_file(destination / "m5.cpt"),
        "vma_mappings": mappings,
    }
