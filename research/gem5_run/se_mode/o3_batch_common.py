# SPDX-License-Identifier: BSD-3-Clause
"""Discovery, provenance and measured-statistics support for O3 drivers."""

from __future__ import annotations

import hashlib
import math
import os
import platform
import re
import subprocess
import sys
import tempfile
import uuid
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path

from common import (
    PipelineError,
    executable,
    gem5_record,
    git_revision,
    parse_key_value,
    read_json,
    sha256_file,
    tool_record,
)

SCRIPT_DIR = Path(__file__).resolve().parent
REPO = SCRIPT_DIR.parents[2]
RESULTS = REPO / "research-result"


def add_options(parser):
    parser.add_argument(
        "--batch-root",
        type=Path,
        help="Step A/B batch directory; auto-discovered if unambiguous",
    )
    parser.add_argument(
        "--gem5",
        type=Path,
        default=REPO / "experimental/build/RISCV/gem5.fast",
    )
    parser.add_argument("--label", default="baseline")
    parser.add_argument("--purpose", default="O3 baseline measurement")
    parser.add_argument("--clock")
    parser.add_argument("--l1i-size", default="32KiB")
    parser.add_argument("--l1d-size", default="32KiB")
    parser.add_argument("--l2-size", default="256KiB")
    parser.add_argument("--branch-predictor")
    parser.add_argument("--cpu-param", action="append", default=[])
    parser.add_argument("--bp-param", action="append", default=[])
    parser.add_argument("--configuration-module", type=Path)
    parser.add_argument("--dry-run", action="store_true")


def test_names(items):
    names = [name.strip() for item in items for name in item.split(",")]
    names = [name for name in names if name]
    if not names or len(set(names)) != len(names):
        raise PipelineError("--tests must be nonempty and have no duplicates")
    if any(not re.fullmatch(r"[\w.-]+\.cmd\d{3}\.[\w.-]+", n) for n in names):
        raise PipelineError(
            "use recorded workload IDs such as 502.gcc_r.cmd000.cpugcc_r"
        )
    return names


def step_b_run_ids(items):
    identifiers = [
        value.strip()
        for item in items
        for value in item.split(",")
        if value.strip()
    ]
    if (
        not identifiers
        or len(set(identifiers)) != len(identifiers)
        or any(
            not re.fullmatch(r"warmup_(0|[1-9]\d*)", value)
            for value in identifiers
        )
    ):
        raise PipelineError(
            "use unique Step B run IDs such as warmup_50000000"
        )
    return identifiers


def discover(names, batch_root=None):
    """Search only stage locations, never recursively enter snapshots."""
    if batch_root:
        roots = [batch_root.expanduser().resolve()]
    else:
        roots = sorted((RESULTS / "batch_results").glob("*/"))
    candidates = {name: [] for name in names}
    for root in roots:
        try:
            root.relative_to(RESULTS.resolve())
        except ValueError as error:
            raise PipelineError(
                "--batch-root must be under research-result"
            ) from error
        for stage in ("profile", "o1_profile"):
            paths = set(root.glob(f"results/*/{stage}/profile_manifest.json"))
            paths.update(root.glob(f"*/{stage}/profile_manifest.json"))
            paths.update(root.glob(f"{stage}/profile_manifest.json"))
            for path in sorted(paths):
                profile = read_json(path)
                name = profile["workload"].get("workload_id")
                if name in candidates:
                    candidates[name].append((root, path, profile))
    selected = []
    for name, matches in candidates.items():
        if len(matches) != 1:
            paths = [str(item[1]) for item in matches]
            raise PipelineError(
                f"{name}: expected one profile, found {len(matches)}: "
                f"{paths}; select --batch-root explicitly"
            )
        root, path, profile = matches[0]
        if (
            profile.get("manifest_type") != "se_simpoint_profile"
            or profile.get("status") != "complete"
        ):
            raise PipelineError(f"invalid profile: {path}")
        selected.append(
            {
                "test": name,
                "batch_root": root,
                "profile_path": path,
                "profile": profile,
                "workload_root": path.parent.parent,
            }
        )
    return selected


def o3_configuration(args, system):
    module = args.configuration_module
    if module:
        module = module.expanduser().resolve()
        if not module.is_file():
            raise PipelineError(f"configuration module missing: {module}")
    return {
        "clock": args.clock or system["clock"],
        "cache": {
            "l1i_size": args.l1i_size,
            "l1d_size": args.l1d_size,
            "l2_size": args.l2_size,
        },
        "branch_predictor": args.branch_predictor,
        "branch_predictor_params": parse_key_value(
            args.bp_param, option="--bp-param"
        ),
        "cpu_params": parse_key_value(args.cpu_param, option="--cpu-param"),
        "configuration_module": str(module) if module else None,
        "configuration_module_sha256": sha256_file(module) if module else None,
    }


def provenance(args):
    binary = executable(args.gem5, "gem5 binary")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.label):
        raise PipelineError("--label accepts letters, digits, '.', '_' or '-'")
    if not args.purpose.strip():
        raise PipelineError("--purpose must describe the experiment")
    # Dry-run never executes even gem5 --build-info.
    info = tool_record(binary) if args.dry_run else gem5_record(binary)
    if args.dry_run:
        info["repository_commit"] = git_revision(binary.parent)
    diff = subprocess.run(
        ["git", "diff", "HEAD", "--binary"],
        cwd=REPO,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return {
        "gem5": info,
        "label": args.label,
        "purpose": args.purpose,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "host": platform.node(),
        "python": sys.version,
        "driver_argv": sys.argv,
        "tracked_diff_sha256": hashlib.sha256(diff.stdout).hexdigest(),
        "tracked_tree_dirty": bool(diff.stdout),
        "source_files": {
            p.name: sha256_file(p) for p in sorted(SCRIPT_DIR.glob("*.py"))
        },
    }


def session_id(args, record):
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return (
        f"{stamp}_{args.label}_{record['gem5']['sha256'][:12]}_"
        f"{uuid.uuid4().hex[:8]}"
    )


def stop_process(process):
    """Stop one session created by our drivers, never search host processes."""
    import signal

    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except ProcessLookupError:
        pass
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def write_markdown(path, contents):
    """Replace reports atomically inside a newly reserved session only."""
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, delete=False
    ) as stream:
        stream.write(contents)
        temporary = Path(stream.name)
    os.replace(temporary, path)


def measured_metrics(stats_path, expected_instructions):
    """Use the final complete dump, including duplicate final exit dumps."""
    contents = stats_path.read_text(encoding="utf-8", errors="replace")
    if "Begin Simulation Statistics" not in contents:
        raise PipelineError(f"no statistics dump in {stats_path}")
    block = contents.rsplit("Begin Simulation Statistics", 1)[1]
    if "End Simulation Statistics" not in block:
        raise PipelineError(f"truncated final statistics dump: {stats_path}")
    stats = {}
    for line in block.split("End Simulation Statistics", 1)[0].splitlines():
        fields = line.split()
        if len(fields) >= 2:
            try:
                stats[fields[0]] = float(fields[1])
            except ValueError:
                continue
    instructions = stats.get("simInsts", 0)
    cycles = [
        (key, value)
        for key, value in stats.items()
        if key.endswith(".numCycles")
    ]
    if len(cycles) != 1:
        raise PipelineError(f"expected one CPU numCycles counter: {cycles}")
    cycle_key, cycle_count = cycles[0]
    if any(
        not math.isfinite(v) or v <= 0 for v in (instructions, cycle_count)
    ):
        raise PipelineError("nonpositive or nonfinite instructions/cycles")
    if instructions != expected_instructions:
        raise PipelineError(
            f"stats simInsts={instructions} differs from runtime "
            f"measurement instructions={expected_instructions}"
        )
    return {
        "instructions": instructions,
        "cycles": cycle_count,
        "cycle_statistic": cycle_key,
        "cpi": cycle_count / instructions,
        "ipc": instructions / cycle_count,
    }


def validate_points(jobs, profile):
    """Require exactly the profiled points and original finite weights."""
    expected = {
        (p["cluster_id"], p["interval_index"]): p["weight"]
        for p in profile["selected_points"]
    }
    actual = {
        (j["cluster_id"], j["interval_index"]): j["weight"] for j in jobs
    }
    if (
        not expected
        or actual != expected
        or len(actual) != len(jobs)
        or len(expected) != len(profile["selected_points"])
    ):
        raise PipelineError("incomplete or duplicated SimPoint set")
    weight_sum = sum(actual.values())
    if any(not math.isfinite(w) or w < 0 for w in actual.values()):
        raise PipelineError("weights must be finite and nonnegative")
    if abs(weight_sum - 1) > 1e-4:
        raise PipelineError(f"weight sum {weight_sum} is not approximately 1")
    return weight_sum


def weighted_result(jobs, profile):
    """Never fill gaps, renormalize missing weights, or average IPC."""
    if any(job["status"] != "complete" for job in jobs):
        return None
    weight_sum = validate_points(jobs, profile)
    cpi = sum(j["weight"] * j["metrics"]["cpi"] for j in jobs)
    if not math.isfinite(cpi) or cpi <= 0:
        raise PipelineError("invalid weighted CPI")
    return {
        "weight_sum": weight_sum,
        "weighted_cpi": cpi,
        "estimated_ipc": 1 / cpi,
        "bbv_covered_instructions": profile.get("bbv_covered_instructions"),
        "profile_total_instructions": profile.get("runtime", {}).get(
            "committed_instructions"
        ),
        "representative_of_full_workload": profile.get(
            "representative_of_full_workload", False
        ),
    }


def report_header(record):
    return (
        f"- Deney: `{record['label']}`\n"
        f"- Amaç: {record['purpose']}\n"
        f"- Başlangıç (UTC): {record['started_utc']}\n"
        f"- gem5: `{record['gem5']['path']}`\n"
        f"- Binary SHA-256: `{record['gem5']['sha256']}`\n"
        f"- Kaynak commit: `{record['gem5'].get('repository_commit')}`\n"
        "- Parametreler, kaynak betik hash'leri ve komut: JSON raporunda.\n"
    )


TERMS = """
## Terimler ve hesap

- Cluster: benzer BBV interval'lerinin SimPoint grubu. Cluster ID ile
  checkpoint ordinal'i aynı olmak zorunda değildir.
- Interval index: profil içindeki sıfır tabanlı temsilci pencere indeksi.
- Weight (w): cluster'ın profillenen interval'ler içindeki instruction payı.
- Warmup: restore sonrasında O3/cache/predictor durumunu ısıtan bölüm;
  ölçüm sayaçları bu bölümün sonunda sıfırlanır.
- N: ölçümde gerçekten commit edilen instruction; C: aynı pencerenin CPU
  cycle sayısı. CPI = C/N, IPC = N/C, katkı = w*CPI.
- Toplam tahmin: CPI = sum(w*CPI); IPC = 1/CPI. IPC'lerin aritmetik
  ortalaması kullanılmaz. Ağırlıklar yeniden normalize edilmez.
- Overshoot: geniş O3 commit nedeniyle istenenden fazla emekli olan
  instruction sayısı; CPI gerçek N ile hesaplanır.
- Wall seconds: host çalışma süresi; simüle edilen program cycle'ı değildir.

Her testin toplamı ayrı hesaplanır. Eksik/başarısız checkpoint varsa o test
için toplam yoktur. BBV kapsamı dışındaki son kısa interval tahmine dahil
değildir. Tamamlanmamış profil tam programı temsil etmez.
Bu sonuçlar resmi SPEC skoru değildir.
"""
