# SPDX-License-Identifier: BSD-3-Clause
"""Exercise scheduling and reports without running gem5 or SPEC."""

import argparse
import contextlib
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

MODULE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MODULE_DIR))

import batch_o3_run  # noqa: E402
import o3_batch_common  # noqa: E402
import run_o3_full  # noqa: E402
from common import (  # noqa: E402
    PipelineError,
    canonical_hash,
    load_workload,
    read_json,
    write_json,
)

FAKE_GEM5 = r"""#!/usr/bin/env python3
import json
import os
import sys
import time
from pathlib import Path

if "--build-info" in sys.argv:
    print("gem5 version fake-test")
    sys.exit(0)
out = Path(sys.argv[sys.argv.index("-d") + 1])
config = json.loads(Path(sys.argv[-1]).read_text())
mode = sys.argv[sys.argv.index("--config") - 1]
ledger = Path(__file__).with_name("events.jsonl")
with ledger.open("a") as stream:
    stream.write(json.dumps(["start", os.getpid(), time.monotonic()]) + "\n")
time.sleep(0.08)
second = "sp_001" in str(out)
failure = config["o3"]["cpu_params"].get("fakeFailure") and second
if mode == "o3":
    restore = Path(config["checkpoint_path"])
    assert restore.name == "restore_checkpoint"
    assert not (restore / "m5.cpt").is_symlink()
    n = config["measurement_instructions"] + 2
    runtime = {"complete": not failure, "measurement_exact": False,
               "measurement_total_instruction_delta": n,
               "measurement_overshoot_instructions": 2}
else:
    limited = bool(config.get("max_insts"))
    n = 100
    runtime = {"complete": not limited, "program_completed": not limited,
               "exit_code": 0, "committed_instructions": n}
Path(config["runtime_status_path"]).write_text(json.dumps(runtime))
cycles = n * (4 if second else 2)
block = ("---------- Begin Simulation Statistics ----------\n"
         f"simInsts {n}\nboard.processor.cores.core.numCycles {cycles}\n"
         "---------- End Simulation Statistics ----------\n")
(out / "stats.txt").write_text(block + block)
with ledger.open("a") as stream:
    stream.write(json.dumps(["end", os.getpid(), time.monotonic()]) + "\n")
print("fake simulation log")
sys.exit(1 if failure else 0)
"""


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.batch = self.root / "batch_results" / "input"
        self.batch.mkdir(parents=True)
        self.gem5 = self.root / "fake_gem5"
        self.gem5.write_text(FAKE_GEM5)
        self.gem5.chmod(0o755)
        self.addCleanup(patch.stopall)
        patch.object(o3_batch_common, "RESULTS", self.root).start()
        patch.object(batch_o3_run, "provenance", wraps=self.provenance).start()
        patch.object(run_o3_full, "provenance", wraps=self.provenance).start()

    def provenance(self, args):
        result = o3_batch_common.provenance(args)
        result["gem5"]["repository_commit"] = None
        return result

    def args(self, full=False, **overrides):
        parser = argparse.ArgumentParser()
        o3_batch_common.add_options(parser)
        args = parser.parse_args([])
        args.gem5 = self.gem5
        args.batch_root = self.batch
        args.jobs = 2
        args.tests = ["502.gcc_r.cmd002.cpugcc_r"]
        args.test = "502.gcc_r.cmd002.cpugcc_r"
        args.max_insts = 0
        args.run_ids = ["warmup_50"]
        args.allow_gem5_mismatch = False
        for key, value in overrides.items():
            setattr(args, key, value)
        return args

    def workload(self, benchmark="502.gcc_r", index=2, warmup=50):
        stem = "cpugcc_r" if benchmark == "502.gcc_r" else "xz_r"
        workload_id = f"{benchmark}.cmd{index:03d}.{stem}"
        source = self.root / "source" / workload_id
        source.mkdir(parents=True, exist_ok=True)
        (source / "program").write_text("fake ELF")
        manifest = self.batch / "workloads" / f"{workload_id}.json"
        write_json(
            manifest,
            {
                "schema_version": 1,
                "workload_id": workload_id,
                "benchmark": benchmark,
                "metadata": {"command_index": index},
                "binary": "program",
                "source_run_directory": str(source),
            },
        )
        workload = load_workload(manifest)
        root = self.batch / "results" / workload_id
        profile_path = root / "profile" / "profile_manifest.json"
        system = {"clock": "3GHz", "memory_size": "2GiB"}
        points = [
            {"cluster_id": 2, "interval_index": 4, "weight": 0.25},
            {"cluster_id": 3, "interval_index": 20, "weight": 0.75},
        ]
        profile = {
            "manifest_type": "se_simpoint_profile",
            "status": "complete",
            "profile_id": workload_id,
            "workload": workload,
            "workload_fingerprint": workload["workload_fingerprint"],
            "system": system,
            "selected_points": points,
            "interval_instructions": 100,
            "bbv_covered_instructions": 3000,
            "runtime": {"committed_instructions": 3050},
            "representative_of_full_workload": True,
        }
        write_json(profile_path, profile)
        checkpoint_root = root / "checkpoints" / f"warmup_{warmup}"
        manifests = []
        for ordinal, point in enumerate(points):
            identifier = (
                f"sp_{ordinal:03d}_interval_{point['interval_index']:08d}"
            )
            snapshot = checkpoint_root / "filesystem_snapshots" / identifier
            shutil.copytree(source, snapshot)
            contract = dict(
                point,
                profile_id=workload_id,
                system=system,
                workload_fingerprint=workload["workload_fingerprint"],
                interval_instructions=100,
                checkpoint_id=identifier,
                ordinal=ordinal,
                simpoint_start_instruction=400,
                checkpoint_instruction=350,
                actual_checkpoint_instruction=350,
                instruction_count_offset=0,
                requested_warmup_instructions=warmup,
                actual_warmup_instructions=warmup,
                checkpoint_stage_workdir=str(checkpoint_root / "workdir"),
                filesystem_snapshot=str(snapshot),
            )
            data_path = checkpoint_root / "checkpoints" / identifier
            data_path.mkdir(parents=True)
            (data_path / "m5.cpt").touch()
            path = data_path / "checkpoint_manifest.json"
            write_json(
                path,
                dict(
                    contract,
                    manifest_type="se_simpoint_checkpoint",
                    status="complete",
                    checkpoint_path=str(data_path),
                    checkpoint_contract_hash=canonical_hash(contract),
                    profile_manifest=str(profile_path),
                    gem5={},
                ),
            )
            manifests.append(str(path))
        write_json(
            checkpoint_root / "checkpoint_set_manifest.json",
            {
                "manifest_type": "se_simpoint_checkpoint_set",
                "status": "complete",
                "profile_id": workload_id,
                "checkpoint_manifests": manifests,
            },
        )
        return root

    def run_batch(self, args):
        with contextlib.redirect_stdout(io.StringIO()):
            return batch_o3_run.run(args)


class BatchTests(Fixture):
    def test_parallel_jobs_reports_and_no_overwrite(self):
        first = self.workload()
        second = self.workload("557.xz_r", 0)
        old = first / "profile" / "profile_manifest.json"
        original = old.read_bytes()
        args = self.args(
            tests=["502.gcc_r.cmd002.cpugcc_r,", "557.xz_r.cmd000.xz_r"]
        )
        self.assertEqual(self.run_batch(args), 0)
        self.assertEqual(self.run_batch(args), 0)
        self.assertEqual(old.read_bytes(), original)
        reports = list(first.glob("o3_runs/warmup_50/*/report.json"))
        self.assertEqual(len(reports), 2)
        for path in reports:
            report = read_json(path)
            self.assertEqual(report["test"], "502.gcc_r.cmd002.cpugcc_r")
            self.assertEqual(report["step_b_run_id"], "warmup_50")
            self.assertAlmostEqual(report["summary"]["weighted_cpi"], 3.5)
            self.assertAlmostEqual(report["summary"]["estimated_ipc"], 1 / 3.5)
            self.assertEqual(len(report["jobs"]), 2)
            for job in report["jobs"]:
                run = read_json(Path(job["run_manifest"]))
                self.assertIn("checkpoint_restore", run)
                self.assertEqual(
                    Path(run["paths"]["restore_checkpoint"]).parent,
                    Path(job["output"]),
                )
            self.assertIn(
                "Toplam ağırlıklı CPI", path.with_suffix(".md").read_text()
            )
        self.assertEqual(
            len(list(second.glob("o3_runs/warmup_50/*/report.md"))), 2
        )
        events = [
            json.loads(line)
            for line in (self.root / "events.jsonl").read_text().splitlines()
        ]
        active = peak = 0
        for event, _, _ in sorted(events, key=lambda e: e[2]):
            active += 1 if event == "start" else -1
            peak = max(peak, active)
        self.assertEqual(active, 0)
        self.assertEqual(peak, 2)

    def test_one_failure_does_not_stop_other_work(self):
        root = self.workload()
        self.assertEqual(
            self.run_batch(self.args(cpu_param=["fakeFailure=true"])), 1
        )
        report = read_json(next(root.glob("o3_runs/warmup_50/*/report.json")))
        self.assertIsNone(report["summary"])
        self.assertEqual(
            [j["status"] for j in report["jobs"]], ["complete", "failed"]
        )
        self.assertTrue(
            all(Path(j["driver_log"]).is_file() for j in report["jobs"])
        )

    def test_dry_run_does_not_execute_gem5_or_create_results(self):
        root = self.workload()
        self.assertEqual(self.run_batch(self.args(dry_run=True)), 0)
        self.assertFalse((root / "o3_runs").exists())
        self.assertFalse((self.root / "events.jsonl").exists())

    def test_restore_does_not_require_original_source_or_manifest(self):
        root = self.workload()
        profile = read_json(root / "profile/profile_manifest.json")
        shutil.rmtree(Path(profile["workload"]["source_run_directory"]))
        Path(profile["workload"]["manifest_path"]).unlink()
        self.assertEqual(self.run_batch(self.args()), 0)

    def test_incomplete_set_rejected_before_outputs(self):
        root = self.workload()
        path = root / "checkpoints/warmup_50/checkpoint_set_manifest.json"
        data = read_json(path)
        data["checkpoint_manifests"].pop()
        write_json(path, data)
        with self.assertRaises(PipelineError):
            self.run_batch(self.args())
        self.assertFalse((root / "o3_runs").exists())

    def test_saved_command_mutation_rejected_before_outputs(self):
        root = self.workload()
        path = root / "profile/profile_manifest.json"
        profile = read_json(path)
        profile["workload"]["arguments"] = ["different-input.c"]
        write_json(path, profile)
        with self.assertRaisesRegex(PipelineError, "fingerprints"):
            self.run_batch(self.args())
        self.assertFalse((root / "o3_runs").exists())

    def test_ambiguous_input_requires_explicit_batch(self):
        self.workload()
        other = self.root / "batch_results/other"
        shutil.copytree(self.batch, other)
        with self.assertRaisesRegex(PipelineError, "found 2"):
            o3_batch_common.discover(["502.gcc_r.cmd002.cpugcc_r"])

    def test_step_b_runs_are_selected_and_summarized_separately(self):
        root = self.workload()
        self.workload(warmup=100)
        self.assertEqual(
            self.run_batch(self.args(run_ids=["warmup_50", "warmup_100"])),
            0,
        )
        reports = [read_json(p) for p in root.glob("o3_runs/*/*/report.json")]
        self.assertEqual(
            {r["step_b_run_id"] for r in reports}, {"warmup_50", "warmup_100"}
        )
        for report in reports:
            self.assertAlmostEqual(report["summary"]["weighted_cpi"], 3.5)
            self.assertEqual(len(report["jobs"]), 2)
            self.assertTrue(
                all(
                    j["step_b_run_id"] == report["step_b_run_id"]
                    for j in report["jobs"]
                )
            )

    def test_missing_step_b_run_rejected_before_outputs(self):
        root = self.workload()
        with self.assertRaisesRegex(PipelineError, "warmup_100"):
            self.run_batch(self.args(run_ids=["warmup_50", "warmup_100"]))
        self.assertFalse((root / "o3_runs").exists())


class FullTests(Fixture):
    def test_full_program_and_limited_run(self):
        root = self.workload()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_o3_full.run(self.args()), 0)
            self.assertEqual(run_o3_full.run(self.args(max_insts=10)), 1)
        reports = [
            read_json(p) for p in root.glob("o3_full_runs/*/run_manifest.json")
        ]
        self.assertEqual(
            sorted(r["status"] for r in reports), ["complete", "failed"]
        )
        complete = next(r for r in reports if r["status"] == "complete")
        self.assertEqual(complete["metrics"]["cpi"], 2)
        self.assertIn("o3_full", complete["command"])

    def test_changed_original_input_rejected(self):
        root = self.workload()
        profile = read_json(root / "profile/profile_manifest.json")
        source = Path(profile["workload"]["source_run_directory"])
        (source / "program").write_text("changed")
        with self.assertRaisesRegex(PipelineError, "changed since profiling"):
            run_o3_full.run(self.args())


class MetricsTests(unittest.TestCase):
    def test_recorded_identifiers_are_required(self):
        self.assertEqual(
            o3_batch_common.test_names(["502.gcc_r.cmd000.cpugcc_r"]),
            ["502.gcc_r.cmd000.cpugcc_r"],
        )
        with self.assertRaises(PipelineError):
            o3_batch_common.test_names(["gcc.002"])
        self.assertEqual(
            o3_batch_common.step_b_run_ids(
                ["warmup_50000000,warmup_100000000"]
            ),
            ["warmup_50000000", "warmup_100000000"],
        )
        for values in (["50M"], ["../warmup_50"], ["warmup_50", "warmup_50"]):
            with self.assertRaises(PipelineError):
                o3_batch_common.step_b_run_ids(values)

    def test_final_dump_and_instruction_agreement(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "stats.txt"
            block = (
                "Begin Simulation Statistics\nsimInsts {}\n"
                "cpu.numCycles {}\nEnd Simulation Statistics\n"
            )
            path.write_text(block.format(999, 999) + block.format(100, 250))
            self.assertEqual(
                o3_batch_common.measured_metrics(path, 100)["cpi"], 2.5
            )
            with self.assertRaises(PipelineError):
                o3_batch_common.measured_metrics(path, 99)
            path.write_text(
                block.format(100, 250) + "Begin Simulation Statistics"
            )
            with self.assertRaisesRegex(PipelineError, "truncated"):
                o3_batch_common.measured_metrics(path, 100)

    def test_missing_and_invalid_weights(self):
        profile = {
            "selected_points": [
                {"cluster_id": 0, "interval_index": 0, "weight": 1.0}
            ]
        }
        job = {"status": "failed"}
        self.assertIsNone(o3_batch_common.weighted_result([job], profile))
        job = dict(
            status="complete",
            cluster_id=0,
            interval_index=0,
            weight=float("nan"),
            metrics={"cpi": 1},
        )
        with self.assertRaises(PipelineError):
            o3_batch_common.weighted_result([job], profile)


if __name__ == "__main__":
    unittest.main()
