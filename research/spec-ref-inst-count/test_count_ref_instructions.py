# SPDX-License-Identifier: BSD-3-Clause
"""Check parsing and accounting without running the reference workloads."""

import tempfile
import unittest
from pathlib import Path

import count_ref_instructions as runner


class CommandTests(unittest.TestCase):
    def test_stdin_and_compiler_output_option(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "502.gcc_r" / "run_base_ref.0000"
            source.mkdir(parents=True)
            (source / "compiler").touch()
            job = runner.parse_command(
                "-i in.c -o out.txt -e err.txt "
                "../run_base_ref.0000/compiler 'input name.c' -o result.s "
                "< in.c > out.txt 2>> err.txt",
                source,
                4,
            )
            self.assertEqual(
                job.argv, ("./compiler", "input name.c", "-o", "result.s")
            )
            self.assertEqual(job.stdin, "in.c")
            self.assertEqual(job.key, "502.gcc_r/004")

    def test_streams_cannot_escape_output_directory(self):
        for name in ("../original", "/tmp/original", "a/../../original"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                runner.local_name(name)

    def test_inconsistent_redirection_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / "compiler").touch()
            with self.assertRaises(ValueError):
                runner.parse_command(
                    "-o a.out -e a.err ./compiler > other.out 2>> a.err",
                    source,
                    0,
                )

    def test_count_is_exact_above_float_precision(self):
        count = 12345678901234567
        matches = runner.TOTAL.findall(
            f"cpu 0 insns: {count}\ntotal insns: {count}\n"
        )
        self.assertEqual([int(value) for value in matches], [count])

    def test_failed_partial_counts_are_excluded(self):
        with tempfile.TemporaryDirectory() as temporary:
            from argparse import Namespace

            report = Path(temporary) / "counts.txt"
            job = runner.Job(
                "500.example_r",
                0,
                Path(temporary),
                ("./example",),
                None,
                "out",
                "err",
                "",
            )
            result = {
                job.key: {
                    "benchmark": job.benchmark,
                    "index": 0,
                    "status": "failed",
                    "observed_instructions": 999,
                    "instructions": None,
                    "error": "nonzero exit",
                }
            }
            runner.write_report(
                Namespace(report=report),
                [job],
                result,
                "finished",
                "UNCHANGED",
            )
            text = report.read_text()
            self.assertIn("failed\t-\t./example", text)
            self.assertIn("Successful instruction sum: 0", text)
            self.assertIn("0/1\tincomplete\t0", text)


if __name__ == "__main__":
    unittest.main()
