# SPDX-License-Identifier: BSD-3-Clause
"""Regression tests for immutable checkpoint VMA path relocation."""

import sys
import tempfile
import unittest
from pathlib import Path

MODULE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MODULE_DIR))

from checkpoint_restore import (  # noqa: E402
    prepare_restore_checkpoint,
    resolve_backing_file,
    rewrite_vma_paths,
)
from common import (  # noqa: E402
    PipelineError,
    sha256_file,
)


class CheckpointRestoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "checkpoint"
        self.source.mkdir()
        self.workdir = self.root / "private workdir"
        self.workdir.mkdir()
        self.sysroot = self.root / "sysroot"
        (self.sysroot / "lib").mkdir(parents=True)
        (self.sysroot / "lib/libm.so.6").write_bytes(b"guest library")
        self.checkpoint = {
            "checkpoint_path": str(self.source),
            "checkpoint_stage_workdir": "/old/checkpoint/workdir",
            "filesystem_snapshot": "/old/checkpoint/snapshot",
        }
        self.workload = {
            "guest_working_directory": "/work",
            "source_run_directory": "/old/spec/source",
            "path_redirects": [{"guest": "/", "host": str(self.sysroot)}],
        }

    def write_checkpoint(self, name="/lib/libm.so.6", newline="\n"):
        text = (
            "# checkpoint comment\n"
            "[board.process.vmalist]\nsize=2\n"
            "[board.process.vmalist.Vma0]\nname=stack\n"
            "addrRangeStart=100\naddrRangeEnd=200\n"
            "[board.process.vmalist.Vma1]\n"
            f"name={name}\nfileOffset=0\n"
            "addrRangeStart=200\naddrRangeEnd=300\n"
            "[board.process.fdarray.Entry3]\n"
            "_fileName=/lib/libm.so.6\n_fileOffset=9\n"
            "[board.physmem.store0]\nfilename=memory.bin\n"
        ).replace("\n", newline)
        (self.source / "m5.cpt").write_bytes(text.encode())
        (self.source / "memory.bin").write_bytes(b"original memory blob")
        return text

    def test_guest_library_redirected_and_other_state_preserved(self):
        original = self.write_checkpoint(newline="\r\n")
        contents, mappings = rewrite_vma_paths(
            self.checkpoint, self.workload, self.workdir
        )
        library = str(self.sysroot / "lib/libm.so.6")
        self.assertEqual(
            contents,
            original.replace("name=/lib/libm.so.6\r\n", f"name={library}\r\n"),
        )
        self.assertEqual(len(mappings), 1)
        self.assertEqual(mappings[0]["file_offset"], 0)
        self.assertEqual(mappings[0]["checkpoint_name"], "/lib/libm.so.6")
        self.assertEqual(mappings[0]["host_path"], library)
        self.assertEqual(
            (self.source / "m5.cpt").read_bytes(), original.encode()
        )

    def test_private_copy_and_memory_references_preserve_source(self):
        self.write_checkpoint()
        before = {p.name: sha256_file(p) for p in self.source.iterdir()}
        destination = self.root / "run/restore_checkpoint"
        record = prepare_restore_checkpoint(
            self.checkpoint, self.workload, self.workdir, destination
        )
        self.assertFalse((destination / "m5.cpt").is_symlink())
        self.assertTrue((destination / "memory.bin").is_symlink())
        self.assertEqual(
            (destination / "memory.bin").resolve(), self.source / "memory.bin"
        )
        self.assertEqual(record["source_m5_cpt_sha256"], before["m5.cpt"])
        self.assertNotEqual(
            record["source_m5_cpt_sha256"], record["restore_m5_cpt_sha256"]
        )
        self.assertEqual(
            before, {p.name: sha256_file(p) for p in self.source.iterdir()}
        )

    def test_relative_guest_and_old_host_paths_use_private_workdir(self):
        for name in (
            "input file.bin",
            "/work/input file.bin",
            "/old/checkpoint/workdir/input file.bin",
            "/old/checkpoint/snapshot/input file.bin",
            "/old/spec/source/input file.bin",
        ):
            self.assertEqual(
                resolve_backing_file(
                    name, self.checkpoint, self.workload, self.workdir
                ),
                self.workdir / "input file.bin",
            )
        (self.workdir / "input file.bin").write_bytes(b"snapshot input")
        self.write_checkpoint("input file.bin")
        _, mappings = rewrite_vma_paths(
            self.checkpoint, self.workload, self.workdir
        )
        self.assertEqual(
            mappings[0]["host_path"], str(self.workdir / "input file.bin")
        )

    def test_existing_host_sysroot_path_is_not_redirected_twice(self):
        path = self.sysroot / "lib/libm.so.6"
        self.write_checkpoint(str(path))
        _, mappings = rewrite_vma_paths(
            self.checkpoint, self.workload, self.workdir
        )
        self.assertEqual(mappings[0]["host_path"], str(path))

    def test_guest_prefix_requires_directory_boundary(self):
        self.assertEqual(
            resolve_backing_file(
                "/workspace/data", self.checkpoint, self.workload, self.workdir
            ),
            self.sysroot / "workspace/data",
        )

    def test_missing_guest_library_has_no_host_fallback_or_writes(self):
        original = self.write_checkpoint("/lib/missing.so")
        destination = self.root / "run/restore_checkpoint"
        with self.assertRaisesRegex(PipelineError, "VMA backing file"):
            prepare_restore_checkpoint(
                self.checkpoint, self.workload, self.workdir, destination
            )
        self.assertFalse(destination.exists())
        self.assertEqual((self.source / "m5.cpt").read_text(), original)

    def test_existing_view_and_source_directory_are_never_overwritten(self):
        self.write_checkpoint()
        destination = self.root / "existing"
        destination.mkdir()
        sentinel = destination / "m5.cpt"
        sentinel.write_text("keep this")
        with self.assertRaises(FileExistsError):
            prepare_restore_checkpoint(
                self.checkpoint, self.workload, self.workdir, destination
            )
        self.assertEqual(sentinel.read_text(), "keep this")
        for destination in (self.source, self.source / "nested-view"):
            with self.assertRaises(PipelineError):
                prepare_restore_checkpoint(
                    self.checkpoint, self.workload, self.workdir, destination
                )

    def test_no_file_backed_vma_does_not_require_files(self):
        path = self.source / "m5.cpt"
        text = "[process.vmalist.Vma0]\nname=anon\naddrRangeStart=0\n"
        path.write_text(text)
        self.assertEqual(
            rewrite_vma_paths(self.checkpoint, self.workload, self.workdir),
            (text, []),
        )

    def test_missing_file_backed_name_is_rejected(self):
        (self.source / "m5.cpt").write_text(
            "[process.vmalist.Vma0]\nfileOffset=0\n"
        )
        with self.assertRaisesRegex(PipelineError, "missing name"):
            rewrite_vma_paths(self.checkpoint, self.workload, self.workdir)


if __name__ == "__main__":
    unittest.main()
