#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Serve explicitly selected log tails on localhost for an HTTPS proxy."""

from __future__ import annotations

import argparse
import errno
import html
import json
import os
import re
import stat
import sys
import threading
from http.server import (
    BaseHTTPRequestHandler,
    ThreadingHTTPServer,
)
from pathlib import Path
from string import Template
from urllib.parse import (
    parse_qs,
    urlencode,
    urlsplit,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
ASSETS = Path(__file__).with_name("log_viewer_assets")
MAX_TAIL = 256 * 1024


def load_catalog(path: Path) -> dict:
    """Configuration paths are relative to the repo, never the URL or cwd."""
    entries = json.loads(path.read_text(encoding="utf-8"))["logs"]
    if not isinstance(entries, list) or not 1 <= len(entries) <= 100:
        raise ValueError("logs must be a list of 1..100 entries")
    catalog = {}
    for entry in entries:
        key, label = entry["id"], entry["label"]
        if not isinstance(key, str) or not re.fullmatch(
            r"[a-z0-9_-]{1,64}", key
        ):
            raise ValueError("log id must use lowercase letters/digits/_/-")
        if key in catalog:
            raise ValueError(f"duplicate log id: {key}")
        if not isinstance(label, str) or not 1 <= len(label) <= 160:
            raise ValueError("label must be 1..160 characters")
        source = Path(entry["path"]).expanduser()
        # Do not resolve the final component: open() must reject symlinks.
        if not source.is_absolute():
            source = REPO_ROOT / source
        catalog[key] = {"label": label, "path": source}
    return catalog


def read_tail(path: Path, limit: int) -> dict:
    """Bound memory/IO, reject symlinks and non-regular files (incl. FIFOs)."""
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("not a regular file")
        offset = max(0, info.st_size - limit)
        stream.seek(offset)
        data = stream.read(limit)
    return {
        "text": data.decode("utf-8", errors="replace"),
        "size_bytes": info.st_size,
        "modified_at": info.st_mtime,
        "truncated": offset > 0,
    }


def log_error(error: Exception) -> tuple[int, dict]:
    """Give public errors useful names without revealing local paths."""
    code = getattr(error, "errno", None)
    if code == errno.ENOENT:
        status, name, message = (
            404,
            "file_not_found",
            "Kayıtlı dosya yolu bulunamadı. Sunucuda --check ile kontrol edin.",
        )
    elif code in (errno.EACCES, errno.EPERM):
        status, name, message = (
            403,
            "permission_denied",
            "Viewer kullanıcısının dosyayı okuma veya dizine erişim izni yok.",
        )
    elif code == errno.ELOOP:
        status, name, message = (
            422,
            "symlink_rejected",
            "Symlink okunamaz. Yapılandırmaya gerçek dosya yolunu yazın.",
        )
    elif code == errno.ENOTDIR:
        status, name, message = (
            422,
            "invalid_path",
            "Kayıtlı yolun bir üst bileşeni dizin değil.",
        )
    elif isinstance(error, ValueError) or code == errno.EISDIR:
        status, name, message = (
            422,
            "not_regular_file",
            "Yalnızca normal dosyalar okunabilir; dizin/FIFO/cihaz okunamaz.",
        )
    else:
        status, name, message = (
            503,
            "read_failed",
            "Dosya okunamadı. Sunucuda --check ile kontrol edin.",
        )
    return status, {"error_code": name, "error": message}


def check_catalog(catalog: dict, tail_bytes: int) -> int:
    """Local diagnostics may display the resolved path; the HTTP API may not."""
    failed = False
    for key, item in catalog.items():
        result = {"id": key, "path": str(item["path"])}
        try:
            tail = read_tail(item["path"], tail_bytes)
            result.update(status="readable", size_bytes=tail["size_bytes"])
        except (OSError, ValueError) as error:
            failed = True
            _, detail = log_error(error)
            result.update(status="unreadable", **detail)
        print(json.dumps(result, ensure_ascii=False), flush=True)
    return 1 if failed else 0


class LogServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, port: int, catalog: dict, tail_bytes: int):
        self.catalog = catalog
        self.tail_bytes = tail_bytes
        self.slots = threading.BoundedSemaphore(16)
        super().__init__(("127.0.0.1", port), LogHandler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


class LogHandler(BaseHTTPRequestHandler):
    server_version = "LogViewer"
    sys_version = ""

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def reply(self, code: int, data: bytes, content_type: str):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Robots-Tag", "noindex, nofollow, noarchive")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'none'; script-src 'self'; "
            "style-src 'unsafe-inline'; connect-src 'self'; "
            "base-uri 'none'; frame-ancestors 'none'; form-action 'self'",
        )
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def json_reply(self, code: int, data: dict):
        self.reply(
            code,
            json.dumps(data, ensure_ascii=True).encode("utf-8"),
            "application/json; charset=utf-8",
        )

    def render_page(self, query: dict):
        """Render log text on the server; reading needs no browser fetch."""
        selected = query.get("id", [next(iter(self.server.catalog))])[0]
        auto = query.get("auto", ["0" if "id" in query else "1"])[0] == "1"
        follow = query.get("follow", ["0" if "id" in query else "1"])[0] == "1"
        options = []
        for key, item in self.server.catalog.items():
            marked = " selected" if key == selected else ""
            options.append(
                f'<option value="{html.escape(key, quote=True)}"{marked}>'
                f'{html.escape(item["label"])}</option>'
            )
        body = ""
        item = self.server.catalog.get(selected)
        if item is None:
            message = "Bu ID çalışan viewer'ın listesinde yok."
        else:
            try:
                result = read_tail(item["path"], self.server.tail_bytes)
                body = result["text"] or "(Dosya henüz boş.)"
                message = (
                    f'{result["size_bytes"]:,} bayt · '
                    + ("Son kısım" if result["truncated"] else "Tam içerik")
                    + " · Sunucudan okundu"
                )
            except (OSError, ValueError) as error:
                code, detail = log_error(error)
                message = (
                    f'HTTP {code} [{detail["error_code"]}]: {detail["error"]}'
                )
        template = Template(
            (ASSETS / "index.html").read_text(encoding="utf-8")
        )
        page = template.substitute(
            options="".join(options),
            status=html.escape(message),
            body=html.escape(body),
            refresh='<meta http-equiv="refresh" content="10">' if auto else "",
            auto_checked="checked" if auto else "",
            follow_checked="checked" if follow else "",
            raw_url=html.escape(
                "/api/log?" + urlencode({"id": selected}), quote=True
            ),
        )
        self.reply(200, page.encode("utf-8"), "text/html; charset=utf-8")

    def do_GET(self):
        if len(self.path) > 2048:
            self.json_reply(414, {"error": "Request too long"})
            return
        try:
            parsed = urlsplit(self.path)
            query = parse_qs(parsed.query, max_num_fields=8)
        except ValueError:
            self.json_reply(400, {"error": "Invalid request"})
            return
        assets = {
            "/app.js": ("app.js", "text/javascript; charset=utf-8"),
        }
        if parsed.path == "/":
            self.render_page(query)
        elif parsed.path in assets:
            name, content_type = assets[parsed.path]
            self.reply(200, (ASSETS / name).read_bytes(), content_type)
        elif parsed.path == "/robots.txt":
            self.reply(200, b"User-agent: *\nDisallow: /\n", "text/plain")
        elif parsed.path == "/api/logs":
            self.json_reply(
                200,
                {
                    "logs": [
                        {"id": key, "label": item["label"]}
                        for key, item in self.server.catalog.items()
                    ]
                },
            )
        elif parsed.path == "/api/log":
            keys = query.get("id", [])
            if set(query) != {"id"} or len(keys) != 1:
                self.json_reply(400, {"error": "One log id is required"})
                return
            item = self.server.catalog.get(keys[0])
            if item is None:
                self.json_reply(
                    404,
                    {
                        "error_code": "unknown_log",
                        "error": "Bu ID çalışan viewer'ın listesinde yok.",
                    },
                )
                return
            try:
                result = read_tail(item["path"], self.server.tail_bytes)
            except (OSError, ValueError) as error:
                code, detail = log_error(error)
                self.json_reply(code, detail)
                return
            self.json_reply(200, {"id": keys[0], **result})
        else:
            self.json_reply(404, {"error": "Not found"})

    def log_message(self, format, *args):
        # Avoid flooding disk and logging query strings; errors use HTTP codes.
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--tail-bytes", type=int, default=64 * 1024)
    parser.add_argument(
        "--check",
        action="store_true",
        help="print resolved paths and read errors locally, without serving",
    )
    args = parser.parse_args()
    try:
        if not 1 <= args.port <= 65535:
            raise ValueError("port must be 1..65535")
        if not 1024 <= args.tail_bytes <= MAX_TAIL:
            raise ValueError(f"tail-bytes must be 1024..{MAX_TAIL}")
        catalog = load_catalog(args.catalog)
        if args.check:
            return check_catalog(catalog, args.tail_bytes)
        for name in ("index.html", "app.js"):
            if not (ASSETS / name).is_file():
                raise ValueError(f"missing UI asset: {name}")
        with LogServer(args.port, catalog, args.tail_bytes) as server:
            print(
                f"Log viewer: http://127.0.0.1:{args.port}/ "
                f"({len(catalog)} public log entries)",
                flush=True,
            )
            server.serve_forever()
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
