#!/usr/bin/env python3
"""Minimal HTTP server with Cache-Control: no-store for WebRTC HTML tests."""
from __future__ import annotations

import argparse
import hashlib
import os
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class NoStoreHandler(SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, fmt: str, *args) -> None:
        print("[%s] %s" % (self.log_date_time_string(), fmt % args))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--dir", default=".")
    args = ap.parse_args()
    os.chdir(args.dir)
    # Print HTML sha for ?v= cache-busting
    for name in ("webrtc-test-9.html", "webrtc-test-6.html", "webrtc-test-3.html"):
        if os.path.isfile(name):
            h = hashlib.sha256(open(name, "rb").read()).hexdigest()[:12]
            print("HTML_SHA %s=%s  url=?v=%s" % (name, h, h))
    handler = partial(NoStoreHandler, directory=os.getcwd())
    httpd = ThreadingHTTPServer((args.bind, args.port), handler)
    print("HTTP_NOSTORE bind=%s port=%s dir=%s" % (args.bind, args.port, os.getcwd()))
    httpd.serve_forever()


if __name__ == "__main__":
    main()
