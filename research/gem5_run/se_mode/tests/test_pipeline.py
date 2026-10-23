#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause

import json
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MODULE_DIR))

from common import (  # noqa: E402
    PipelineError,
    load_workload,
    parse_instruction_count,
    parse_key_value,
    parse_simpoint_outputs,
)
from run_spec_batch import (  # noqa: E402
    RESEARCH_RESULT_ROOT,
    load_spec_runs_catalog,
    render_report,
    resolve_gem5_binary,
    resolve_output_root,
    resolve_run_directory,
    selected_manifest_paths,
)
from spec_workload import parse_spec_command  # noqa: E402


class CountParsingTests(unittest.TestCase):
    def test_decimal_suffixes(self):
        self.assertEqual(parse_instruction_count("50M"), 50_000_000)
        self.assertEqual(parse_instruction_count("2_000k"), 2_000_000)

    def test_negative_rejected(self):
        with self.assertRaises(ValueError):
            parse_instruction_count("-1")

    def test_parameter_values(self):
        self.assertEqual(
            parse_key_value(
                ["fetchWidth=8", "enabled=true", "policy=TAGE"],
                option="--cpu-param",
            ),
            {"fetchWidth": 8, "enabled": True, "policy": "TAGE"},
        )


class SimPointParsingTests(unittest.TestCase):
    def test_cluster_ids_join_points_and_weights(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            points = root / "simpoints.txt"
            weights = root / "weights.txt"
            points.write_text("9 3\n2 7\n", encoding="utf-8")
            weights.write_text("0.25 7\n0.75 3\n", encoding="utf-8")
            parsed = parse_simpoint_outputs(points, weights, interval_count=10)
        self.assertEqual([point["interval_index"] for point in parsed], [2, 9])
        self.assertEqual([point["weight"] for point in parsed], [0.25, 0.75])

    def test_weight_sum_is_validated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            points = root / "simpoints.txt"
            weights = root / "weights.txt"
            points.write_text("1 0\n", encoding="utf-8")
            weights.write_text("0.5 0\n", encoding="utf-8")
            with self.assertRaises(PipelineError):
                parse_simpoint_outputs(points, weights)


class WorkloadTests(unittest.TestCase):
    def test_spec_command_keeps_program_o_argument(self):
        with tempfile.TemporaryDirectory() as temporary:
            run_dir = Path(temporary)
            binary = run_dir / "gcc"
            binary.write_bytes(b"ELF")
            command = (
                "-o output.log -e output.err ./gcc input.c -O3 -o output.s "
                "> output.log 2>> output.err"
            )
            parsed = parse_spec_command(command, run_dir)
        self.assertEqual(parsed["binary"], "gcc")
        self.assertEqual(
            parsed["arguments"],
            ["input.c", "-O3", "-o", "output.s"],
        )
        self.assertEqual(parsed["stdout"], "output.log")

    def test_workload_fingerprint_tracks_source_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = root / "run"
            run_dir.mkdir()
            binary = run_dir / "program"
            binary.write_bytes(b"version one")
            manifest_path = root / "workload.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "workload_id": "unit-test",
                        "source_run_directory": str(run_dir),
                        "binary": "program",
                        "arguments": [],
                        "environment": {},
                    }
                ),
                encoding="utf-8",
            )
            first = load_workload(manifest_path)["workload_fingerprint"]
            binary.write_bytes(b"version two")
            second = load_workload(manifest_path)["workload_fingerprint"]
        self.assertNotEqual(first, second)


class BatchPipelineTests(unittest.TestCase):
    def test_explicit_gem5_binary_overrides_repository_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "custom-gem5"
            binary.write_bytes(b"binary")
            binary.chmod(0o755)
            resolved = resolve_gem5_binary(binary, "opt")
        self.assertEqual(resolved, binary.resolve())

    def test_spec_runs_catalog_discovers_train_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = (
                root / "train" / "531.deepsjeng_r" / "run_base_train_test.0000"
            )
            run_dir.mkdir(parents=True)
            (root / "train" / "manifest.tsv").write_text(
                "benchmark\trun_dir\n"
                "531.deepsjeng_r\trun_base_train_test.0000\n",
                encoding="utf-8",
            )
            size_root, catalog = load_spec_runs_catalog(root, "train")
            resolved = resolve_run_directory(
                "531.deepsjeng_r",
                size_root,
                catalog,
                {},
            )
        self.assertEqual(resolved, run_dir.resolve())

    def test_direct_train_directory_is_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            train_dir = Path(temporary) / "train_reduced"
            train_dir.mkdir()
            (train_dir / "manifest.tsv").write_text(
                "benchmark\trun_dir\n"
                "541.leela_r\trun_base_train_test.0000\n",
                encoding="utf-8",
            )
            input_root, catalog = load_spec_runs_catalog(train_dir)
        self.assertEqual(input_root, train_dir.resolve())
        self.assertEqual(
            catalog,
            {"541.leela_r": "run_base_train_test.0000"},
        )

    def test_default_batch_output_is_below_research_result(self):
        output = resolve_output_root(None, Path("/input/train_reduced"))
        self.assertEqual(
            output,
            (
                RESEARCH_RESULT_ROOT / "batch_results" / "train_reduced"
            ).resolve(),
        )

    def test_batch_output_outside_research_result_is_rejected(self):
        with self.assertRaises(PipelineError):
            resolve_output_root(
                Path("/tmp/outside-research-result"), Path("train")
            )

    def test_manifest_plan_preserves_each_spec_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = root / "run_base_train_test.0000"
            run_dir.mkdir()
            binary = run_dir / "cpugcc_r_base.test"
            binary.write_bytes(b"ELF")
            (run_dir / "speccmds.cmd").write_text(
                "-o one.out -e one.err ./cpugcc_r_base.test one.c\n"
                "-o two.out -e two.err ./cpugcc_r_base.test two.c\n",
                encoding="utf-8",
            )
            paths = selected_manifest_paths(
                "502.gcc_r", run_dir, root / "manifests", None
            )
        self.assertEqual(len(paths), 2)
        self.assertEqual(paths[0].name, "502.gcc_r.cmd000.cpugcc_r.json")
        self.assertEqual(paths[1].name, "502.gcc_r.cmd001.cpugcc_r.json")

    def test_markdown_report_contains_failures(self):
        report = {
            "status": "failed",
            "jobs": 4,
            "gem5": {"path": "/bundle/bin/gem5.fast"},
            "workloads": [
                {
                    "workload_id": "test.cmd000",
                    "status": "failed",
                    "error": "example failure",
                    "profile": {"status": "failed"},
                    "checkpoint": {"status": "pending"},
                    "wall_seconds": 1.0,
                }
            ],
        }
        rendered = render_report(report)
        self.assertIn("example failure", rendered)
        self.assertIn("test.cmd000", rendered)


if __name__ == "__main__":
    unittest.main()
