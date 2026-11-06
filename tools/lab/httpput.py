#!/usr/bin/env python3
"""Minimal HTTP sink for `curl -T -` from devices that cannot buffer a file.

    python3 tools/lab/httpput.py --dir /srv/tftp --port 8000

Accepts PUT (Content-Length or chunked, streamed straight to disk) and writes
into --dir under the request path's basename.  A bare `GET /` lists the
directory and `GET /<name>` serves the file back, so an upload can be verified
without leaving the tool.  No authentication, no TLS: lab LAN only.  Use
`--bind 127.0.0.1` to keep it on the loopback interface.

Why it exists: an E2631 whose rootfs is 100 % full has nowhere to put a 17 MiB
image, and `tftp -p` needs a local file first.  `dd if=/dev/mtdblock8 | curl -T -`
streams the partition out without touching the device's flash or tmpfs.  See
docs/research/MESH_INTEROP.md §4.2.3.

Tested with curl 8.x (`-T -`, chunked) and with a Content-Length PUT.
"""

from __future__ import annotations

import argparse
import html
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

BLOCK = 1 << 16
PROGRESS_EVERY = 4 << 20


class Handler(BaseHTTPRequestHandler):
    server_version = "httpput/1"
    root: Path = Path(".")
    chatty: bool = True

    # -- helpers ---------------------------------------------------------
    def _target(self) -> Path | None:
        """Map the request path to a file under root, or None if unsafe."""
        name = Path(unquote(urlsplit(self.path).path)).name
        if not name or name in {".", ".."}:
            return None
        return self.root / name

    def _body_to(self, out, length: int | None) -> int:
        """Copy the request body to `out`, honouring chunked framing."""
        total = 0
        chunked = "chunked" in self.headers.get("Transfer-Encoding", "").lower()
        if chunked:
            while True:
                line = self.rfile.readline().strip()
                if b";" in line:                       # chunk extensions
                    line = line.split(b";", 1)[0]
                if not line:                           # tolerate stray CRLF
                    continue
                size = int(line, 16)
                if size == 0:                          # trailer section
                    while True:
                        trailer = self.rfile.readline()
                        if trailer in (b"\r\n", b"\n", b""):
                            break
                    break
                remaining = size
                while remaining:
                    block = self.rfile.read(min(BLOCK, remaining))
                    if not block:
                        raise ConnectionError("truncated chunk")
                    out.write(block)
                    remaining -= len(block)
                    total += len(block)
                    self._tick(total)
                self.rfile.read(2)                     # trailing CRLF
        else:
            remaining = length or 0
            while remaining:
                block = self.rfile.read(min(BLOCK, remaining))
                if not block:
                    raise ConnectionError("truncated body")
                out.write(block)
                remaining -= len(block)
                total += len(block)
                self._tick(total)
        return total

    def _tick(self, total: int) -> None:
        if self.chatty and total % PROGRESS_EVERY < BLOCK:
            print(f"  ... {total / 1e6:.1f} MB", file=sys.stderr, flush=True)

    def _reply(self, code: int, text: str) -> None:
        payload = (text + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    # -- verbs -----------------------------------------------------------
    def do_PUT(self) -> None:                          # noqa: N802
        target = self._target()
        if target is None:
            self._reply(400, "bad path")
            return
        length = self.headers.get("Content-Length")
        if length is None and "chunked" not in self.headers.get("Transfer-Encoding", "").lower():
            self._reply(411, "need Content-Length or chunked")
            return
        partial = target.with_name(target.name + ".part")
        try:
            self.root.mkdir(parents=True, exist_ok=True)
            with open(partial, "wb") as out:
                total = self._body_to(out, int(length) if length else None)
            os.replace(partial, target)                # only publish complete files
        except Exception as exc:                       # noqa: BLE001
            partial.unlink(missing_ok=True)
            self._reply(500, f"write failed: {exc}")
            return
        print(f"PUT {target.name} <- {total} bytes", file=sys.stderr, flush=True)
        self._reply(201, f"stored {target.name} {total}")

    def do_GET(self) -> None:                          # noqa: N802
        if urlsplit(self.path).path in ("", "/"):
            rows = sorted(p.name for p in self.root.glob("*") if p.is_file())
            body = "\n".join(rows) or "(empty)"
            payload = html.escape(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        target = self._target()
        if target is None or not target.is_file():
            self._reply(404, "not found")
            return
        size = target.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(target, "rb") as handle:
            while block := handle.read(BLOCK):
                self.wfile.write(block)

    def log_message(self, fmt: str, *args) -> None:    # quieter than default
        if self.chatty:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main() -> int:
    parser = argparse.ArgumentParser(description="HTTP sink for `curl -T -`.")
    parser.add_argument("--dir", default=".", help="where uploads land (default: .)")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--bind", default="0.0.0.0", help="127.0.0.1 to stay local")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    root = Path(args.dir).expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    Handler.root = root
    Handler.chatty = not args.quiet

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"httpput on http://{args.bind}:{args.port}/ -> {root}", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
