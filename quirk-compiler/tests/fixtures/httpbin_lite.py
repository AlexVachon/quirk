#!/usr/bin/env python3
"""
Minimal in-repo replacement for the slice of httpbin.org our http
client tests exercise. Spins up a local HTTP server with these
endpoints:

    GET  /headers           → JSON {"headers": {...request headers...}}
    GET  /get?k=v           → JSON {"args": {...}, "headers": {...}}
    POST /post              → JSON {"json": <parsed body>, "headers": {...}}
    GET  /redirect/N        → 302 to /redirect/(N-1) until N=0,
                              then 302 to /get
    GET  /relative-redirect/N → alias of /redirect/N

The shapes mirror httpbin closely enough that `resp.text.contains(...)`
assertions in `tests/http_client_test.quirk` work unchanged.

Quirk's http client is plain-HTTP only (no TLS yet), so we serve
plain HTTP on 127.0.0.1. The port is configurable via --port (default
8765) so the test wrapper can pick a free one.

Stays dependency-free — uses only Python stdlib so any GitHub Actions
runner with python3 can host it.
"""

from __future__ import annotations

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    # Silence the default access log (one line per request). The
    # surrounding test wrapper has its own progress output; the
    # request log here would interleave with the assertion output
    # and make CI logs harder to read.
    def log_message(self, format, *args):
        pass

    # ---- helpers ----

    def _send_json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _send_redirect(self, location: str) -> None:
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    def _request_headers_as_dict(self) -> dict:
        # Match httpbin's pretty-cased keys (the existing assertions
        # check for the exact strings "X-Quirk-Token" / "Accept" in
        # the body — preserve case).
        return {k: v for k, v in self.headers.items()}

    def _parse_query(self, path: str) -> tuple[str, dict]:
        if "?" not in path:
            return path, {}
        base, qs = path.split("?", 1)
        out: dict[str, str] = {}
        for pair in qs.split("&"):
            if not pair:
                continue
            if "=" in pair:
                k, v = pair.split("=", 1)
            else:
                k, v = pair, ""
            # URL-decode minimally — the tests assert against
            # the decoded values, so the same `%20` → ` ` and `+`
            # → ` ` rule as application/x-www-form-urlencoded.
            from urllib.parse import unquote_plus
            out[unquote_plus(k)] = unquote_plus(v)
        return base, out

    # ---- routes ----

    def do_GET(self):
        path, args = self._parse_query(self.path)

        if path == "/headers":
            self._send_json({"headers": self._request_headers_as_dict()})
            return

        if path == "/get":
            self._send_json({
                "args": args,
                "headers": self._request_headers_as_dict(),
            })
            return

        if path.startswith("/redirect/") or path.startswith("/relative-redirect/"):
            try:
                n = int(path.rsplit("/", 1)[1])
            except ValueError:
                self._send_json({"error": "bad redirect count"}, status=400)
                return
            # httpbin's contract: each step decrements N. When N hits 1,
            # the next 302 points at /get (the terminal echo endpoint).
            if n <= 1:
                self._send_redirect("/get")
            else:
                self._send_redirect(f"/redirect/{n - 1}")
            return

        self._send_json({"error": "not found", "path": path}, status=404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(length) if length else b""
        path, _ = self._parse_query(self.path)

        if path == "/post":
            parsed = None
            ctype = self.headers.get("Content-Type", "")
            if "application/json" in ctype:
                try:
                    parsed = json.loads(raw.decode("utf-8") or "null")
                except json.JSONDecodeError:
                    parsed = None
            self._send_json({
                "json": parsed,
                "data": raw.decode("utf-8", errors="replace"),
                "headers": self._request_headers_as_dict(),
            })
            return

        self._send_json({"error": "not found", "path": path}, status=404)


def main() -> int:
    parser = argparse.ArgumentParser(description="Local httpbin-lite for Quirk tests")
    parser.add_argument("--port", type=int, default=8765,
                        help="bind port (default 8765)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind host (default 127.0.0.1)")
    args = parser.parse_args()

    server = HTTPServer((args.host, args.port), Handler)
    # Print a single line so the wrapper script can `wait-for-ready`
    # by tailing stdout. Flushed explicitly because the script runs
    # under a pipe in the wrapper.
    print(f"httpbin_lite ready on http://{args.host}:{args.port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
