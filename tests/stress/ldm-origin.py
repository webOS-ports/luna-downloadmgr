#!/usr/bin/env python3
# Copyright (c) 2026 LG Electronics, Inc.
#
# SPDX-License-Identifier: Apache-2.0
"""A deliberately awkward HTTP origin for stress-testing luna-downloadmgr.

Stressing the download manager against a real server only exercises the happy
path. The interesting states - interrupted transfers, resume, redirect loops,
header oddities - need a server that will produce them on demand. Every
behaviour is selected by URL path, so the driver script can ask for one:

  /ok/<bytes>              200, exactly <bytes> of payload, Content-Length set
  /slow/<bytes>/<kbps>     200, throttled, so pause/resume/cancel have a window
  /nolength/<bytes>        200 with no Content-Length (bytesTotal stays 0)
  /truncate/<bytes>        Content-Length says <bytes>, connection drops at half
  /drop/<bytes>            connection closed mid-body with no warning
  /redirect/<n>            n chained 302s, then /ok/1024 (n > 5 must be refused)
  /redirect-loop           302 to itself forever
  /status/<code>           that status with a short body
  /huge                    Content-Length above 2^32, body truncated
  /weird-headers           control characters, absurd lengths, duplicate fields
  /quote-name              Content-Disposition/redirect target full of quotes
  /range/<bytes>           honours Range:, so resume can actually resume
  /stall/<bytes>           headers, then nothing (trips LOW_SPEED_TIME)

Ranged GETs are honoured on /ok, /slow and /range, which is what makes a
resume test meaningful.

Usage: ldm-origin.py [--port 8099] [--bind 127.0.0.1]
"""

import argparse
import random
import re
import socketserver
import sys
import time
from http.server import BaseHTTPRequestHandler

CHUNK = 64 * 1024


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "ldm-origin/1"

    # keep the terminal readable during a long run
    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("origin: %s - %s\n" % (self.address_string(), fmt % args))

    # ---- helpers -------------------------------------------------------

    def _requested_range(self, total):
        """Return (start, end_inclusive) honouring a single-range request."""
        hdr = self.headers.get("Range", "")
        m = re.match(r"bytes=(\d*)-(\d*)$", hdr.strip())
        if not m:
            return 0, total - 1
        start = int(m.group(1)) if m.group(1) else 0
        end = int(m.group(2)) if m.group(2) else total - 1
        start = max(0, min(start, max(total - 1, 0)))
        end = max(start, min(end, max(total - 1, 0)))
        return start, end

    def _body(self, n):
        """Deterministic filler, so a resumed file can be verified byte for byte."""
        return bytes((i % 251) for i in range(n))

    def _send_body(self, start, end, kbps=None, stop_after=None):
        remaining = end - start + 1
        sent = 0
        pos = start
        budget = remaining if stop_after is None else min(remaining, stop_after)
        while sent < budget:
            n = min(CHUNK, budget - sent)
            try:
                self.wfile.write(bytes((i % 251) for i in range(pos, pos + n)))
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return False
            sent += n
            pos += n
            if kbps:
                time.sleep(n / (kbps * 1024.0))
        return True

    # ---- routes --------------------------------------------------------

    def do_HEAD(self):
        self.do_GET(head_only=True)

    def do_GET(self, head_only=False):
        path = self.path.split("?", 1)[0]
        parts = [p for p in path.split("/") if p]

        def arg(i, default):
            try:
                return int(parts[i])
            except (IndexError, ValueError):
                return default

        route = parts[0] if parts else "ok"

        if route == "ok" or route == "range":
            total = arg(1, 1024 * 1024)
            start, end = self._requested_range(total)
            partial = (start, end) != (0, total - 1)
            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            if partial:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, total))
            self.end_headers()
            if not head_only:
                self._send_body(start, end)

        elif route == "slow":
            total = arg(1, 8 * 1024 * 1024)
            kbps = arg(2, 64)
            start, end = self._requested_range(total)
            partial = (start, end) != (0, total - 1)
            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            if partial:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, total))
            self.end_headers()
            if not head_only:
                self._send_body(start, end, kbps=kbps)

        elif route == "nolength":
            # bytesTotal stays 0, which drives the "Content-Length was the
            # remainder" fix-up in cbHeader() on the next resume
            total = arg(1, 512 * 1024)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            if not head_only:
                self._send_body(0, total - 1)
            self.close_connection = True

        elif route == "truncate":
            # promises <bytes>, delivers half: completed_dl() should call this
            # FILECORRUPT / interrupted
            total = arg(1, 1024 * 1024)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(total))
            self.end_headers()
            if not head_only:
                self._send_body(0, total - 1, stop_after=total // 2)
            self.close_connection = True

        elif route == "drop":
            total = arg(1, 1024 * 1024)
            self.send_response(200)
            self.send_header("Content-Length", str(total))
            self.end_headers()
            if not head_only:
                self._send_body(0, total - 1, stop_after=min(4096, total))
            try:
                self.connection.close()
            except OSError:
                pass
            self.close_connection = True

        elif route == "redirect":
            n = arg(1, 1)
            self.send_response(302)
            if n > 1:
                self.send_header("Location", "/redirect/%d" % (n - 1))
            else:
                self.send_header("Location", "/ok/1024")
            self.send_header("Content-Length", "0")
            self.end_headers()

        elif route == "redirect-loop":
            self.send_response(302)
            self.send_header("Location", "/redirect-loop")
            self.send_header("Content-Length", "0")
            self.end_headers()

        elif route == "status":
            code = arg(1, 404)
            body = b"status %d\n" % code
            self.send_response(code)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head_only:
                self.wfile.write(body)

        elif route == "huge":
            # Content-Length past 2^32, so a 32-bit truncation shows up as a
            # size mismatch rather than a successful short download
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(5 * 1024 ** 3))
            self.end_headers()
            if not head_only:
                self._send_body(0, 5 * 1024 ** 3 - 1, stop_after=64 * 1024)
            self.close_connection = True

        elif route == "weird-headers":
            body = self._body(1024)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream ")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("X-Empty", "")
            self.send_header("X-Long", "A" * 8000)
            self.send_header("X-Dup", "first")
            self.send_header("X-Dup", "second")
            self.send_header("X-Colons", "a:b:c:d")
            self.send_header("X-Quotes", 'he said "hi" \\ and left')
            self.send_header("X-Utf8", "café-über")
            self.end_headers()
            if not head_only:
                self.wfile.write(body)

        elif route == "quote-name":
            # redirect to a target whose filename is full of JSON metacharacters
            self.send_response(302)
            self.send_header("Location", '/ok/1024?name=a"b\\c')
            self.send_header("Content-Length", "0")
            self.end_headers()

        elif route == "stall":
            total = arg(1, 1024 * 1024)
            self.send_response(200)
            self.send_header("Content-Length", str(total))
            self.end_headers()
            # nothing else, ever: CURLOPT_LOW_SPEED_TIME should fire
            time.sleep(120)

        elif route == "flaky":
            # half the requests succeed, half drop, so a resume loop makes
            # forward progress but not monotonically
            total = arg(1, 4 * 1024 * 1024)
            start, end = self._requested_range(total)
            self.send_response(206 if start else 200)
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            if start:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, total))
            self.end_headers()
            if not head_only:
                cut = None if random.random() < 0.5 else (end - start + 1) // 3
                self._send_body(start, end, stop_after=cut)
                if cut is not None:
                    self.close_connection = True

        else:
            body = __doc__.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head_only:
                self.wfile.write(body)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    verbose = False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    srv = Server((args.bind, args.port), Handler)
    srv.verbose = args.verbose
    print("ldm-origin listening on http://%s:%d/ (see --help for routes)"
          % (args.bind, args.port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
