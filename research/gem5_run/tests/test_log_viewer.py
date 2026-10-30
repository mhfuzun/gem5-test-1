#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause

import contextlib
import errno
import http.client
import io
import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from log_viewer import (  # noqa: E402
    LogServer,
    check_catalog,
    load_catalog,
    read_tail,
)


class LogViewerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.log = self.root / "public.log"
        self.log.write_text("first line\n", encoding="utf-8")
        self.server = LogServer(
            0, {"batch": {"label": "Batch log", "path": self.log}}, 1024
        )
        self.worker = threading.Thread(target=self.server.serve_forever)
        self.worker.start()
        self.addCleanup(self.close_server)

    def close_server(self):
        self.server.shutdown()
        self.worker.join(timeout=5)
        self.server.server_close()

    def request(self, path, method="GET"):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=5
        )
        try:
            connection.request(method, path)
            response = connection.getresponse()
            return (
                response.status,
                dict(response.getheaders()),
                response.read(),
            )
        finally:
            connection.close()

    def test_catalog_hides_paths_and_tail_updates(self):
        status, headers, body = self.request("/api/logs")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertEqual(
            json.loads(body),
            {"logs": [{"id": "batch", "label": "Batch log"}]},
        )
        with self.log.open("a", encoding="utf-8") as output:
            output.write("<script>alert(1)</script>\n")
        status, headers, body = self.request("/api/log?id=batch")
        self.assertEqual(status, 200)
        self.assertEqual(
            headers["Content-Type"], "application/json; charset=utf-8"
        )
        self.assertIn("<script>", json.loads(body)["text"])
        self.assertIn("script-src 'self'", headers["Content-Security-Policy"])

    def test_only_allowlisted_ids_can_be_read(self):
        for path in (
            "/api/log?id=../../etc/passwd",
            "/../../etc/passwd",
            "/examples/public_logs.json",
            "/api/log?id=batch&path=/etc/passwd",
            "/api/log?id=batch&id=batch",
        ):
            with self.subTest(path=path):
                status, _, body = self.request(path)
                self.assertIn(status, (400, 404))
                self.assertNotIn(str(self.root).encode(), body)

    def test_write_is_not_supported(self):
        status, _, _ = self.request("/api/log?id=batch", "POST")
        self.assertEqual(status, 501)
        self.assertEqual(self.log.read_text(), "first line\n")

    def test_missing_file_can_appear_later(self):
        self.log.unlink()
        status, _, body = self.request("/api/log?id=batch")
        self.assertEqual(status, 404)
        self.assertEqual(json.loads(body)["error_code"], "file_not_found")
        self.assertNotIn(str(self.root).encode(), body)
        self.log.write_text("new run", encoding="utf-8")
        self.assertEqual(self.request("/api/log?id=batch")[0], 200)

    def test_tail_is_bounded_and_handles_truncation(self):
        self.log.write_bytes(b"x" * 4096)
        result = read_tail(self.log, 1024)
        self.assertEqual(len(result["text"]), 1024)
        self.assertTrue(result["truncated"])
        self.log.write_bytes(b"rotated")
        self.assertEqual(read_tail(self.log, 1024)["text"], "rotated")

    def test_symlink_and_fifo_are_rejected(self):
        for name in ("link", "fifo"):
            source = self.root / name
            if name == "link":
                source.symlink_to(self.log)
            else:
                os.mkfifo(source)
            self.server.catalog["batch"]["path"] = source
            status, _, body = self.request("/api/log?id=batch")
            self.assertEqual(status, 422)
            self.assertEqual(
                json.loads(body)["error_code"],
                "symlink_rejected" if name == "link" else "not_regular_file",
            )

    def test_permissions_have_a_distinct_error_without_private_paths(self):
        denied = PermissionError(errno.EACCES, "denied", str(self.log))
        with patch("log_viewer.read_tail", side_effect=denied):
            status, _, body = self.request("/api/log?id=batch")
        self.assertEqual(status, 403)
        self.assertEqual(json.loads(body)["error_code"], "permission_denied")
        self.assertNotIn(str(self.log).encode(), body)

    def test_local_check_shows_effective_paths_and_failures(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = check_catalog(self.server.catalog, 1024)
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(output.getvalue())["path"], str(self.log))
        self.log.unlink()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = check_catalog(self.server.catalog, 1024)
        self.assertEqual(result, 1)
        self.assertEqual(
            json.loads(output.getvalue())["error_code"], "file_not_found"
        )

    def test_duplicate_config_ids_are_rejected(self):
        config = self.root / "logs.json"
        entry = {"id": "batch", "label": "Batch", "path": "some.log"}
        config.write_text(json.dumps({"logs": [entry, entry]}))
        with self.assertRaises(ValueError):
            load_catalog(config)

    def test_page_and_javascript_are_served(self):
        status, _, page = self.request("/")
        self.assertEqual(status, 200)
        self.assertIn(b"first line", page)
        self.assertIn(b'http-equiv="refresh"', page)
        status, _, script = self.request("/app.js")
        self.assertEqual(status, 200)
        self.assertNotIn(b"fetch(", script)
        self.assertNotIn(b"innerHTML", script)

    def test_html_escapes_log_content_and_labels_without_javascript(self):
        self.log.write_text(
            "<script>alert(1)</script> $status", encoding="utf-8"
        )
        self.server.catalog["batch"][
            "label"
        ] = '<img src=x onerror="alert(1)">'
        status, _, page = self.request("/?id=batch")
        self.assertEqual(status, 200)
        self.assertIn(b"&lt;script&gt;alert(1)&lt;/script&gt; $status", page)
        self.assertNotIn(b"<script>alert(1)</script>", page)
        self.assertIn(b"&lt;img", page)
        self.assertNotIn(b'http-equiv="refresh"', page)

    def test_server_rendered_selection_and_refresh_are_preserved(self):
        other = self.root / "report.md"
        other.write_text("Report contents", encoding="utf-8")
        self.server.catalog["report"] = {"label": "Report", "path": other}
        status, _, page = self.request("/?id=report&auto=1&follow=1")
        self.assertEqual(status, 200)
        self.assertIn(b"Report contents", page)
        self.assertNotIn(b"first line", page)
        self.assertIn(b'<option value="report" selected>', page)
        self.assertIn(b'http-equiv="refresh"', page)

    def test_server_rendered_error_does_not_disclose_path(self):
        self.log.unlink()
        status, _, page = self.request("/?id=batch")
        self.assertEqual(status, 200)
        self.assertIn(b"file_not_found", page)
        self.assertNotIn(str(self.root).encode(), page)


if __name__ == "__main__":
    unittest.main()
