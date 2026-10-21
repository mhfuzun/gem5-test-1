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


if __name__ == "__main__":
    unittest.main()
