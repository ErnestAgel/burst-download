#!/usr/bin/env python3
"""Local HTTP test server for Burst Download download-regression tests.

Serves a deterministic payload so clients can verify content integrity.
Features:
  - HTTP Range support (single range) or --ignore-range (always 200)
  - ETag / Last-Modified response headers
  - optional bandwidth throttle (--throttle-bps)
  - optional mid-transfer truncation (--truncate-after, per request)

Usage:
  python test_http_server.py [--port 0] [--size 1048576] [--seed 1]
                             [--ignore-range] [--throttle-bps N]
                             [--truncate-after N]
"""

import argparse
import hashlib
import http.server
import socketserver
import time


class TestConfig:
    """Class-level configuration shared by all handler instances."""

    content = b""
    etag = ""
    ignore_range = False
    throttle_bps = 0
    truncate_after = 0


def MakeContent(dwSeed, u64Size):
    """Build a deterministic pseudo-random payload of exactly u64Size bytes."""
    content = bytearray()
    nIndex = 0
    while len(content) < u64Size:
        digest = hashlib.sha256(("%d:%d" % (dwSeed, nIndex)).encode())
        content += digest.digest()
        nIndex += 1
    return bytes(content[:u64Size])


class TestHandler(http.server.BaseHTTPRequestHandler):
    """Minimal GET-only handler for /file.bin and /health."""

    protocol_version = "HTTP/1.1"
    server_version = "BurstTestServer/1.0"

    def log_message(self, strFormat, *args):
        print("[req] %s %s" % (self.address_string(), strFormat % args),
              flush=True)

    def do_GET(self):
        if self.path == "/health":
            self._send_full(200, b"ok", "text/plain")
            return
        if self.path != "/file.bin":
            self._send_full(404, b"not found", "text/plain")
            return
        self._handle_file(True)

    def do_HEAD(self):
        if self.path != "/file.bin":
            self.send_response(404)
            self.end_headers()
            return
        self._handle_file(False)

    def _handle_file(self, bIncludeBody):
        if TestConfig.ignore_range:
            self._respond(200, TestConfig.content, bIncludeBody)
            return
        tRange = self._parse_range()
        if tRange is None:
            self._respond(200, TestConfig.content, bIncludeBody)
            return
        nStart, nEnd = tRange
        u64Total = len(TestConfig.content)
        if nStart > nEnd or nStart >= u64Total:
            self.send_response(416)
            self.end_headers()
            return
        nEnd = min(nEnd, u64Total - 1)
        byBody = TestConfig.content[nStart:nEnd + 1]
        self.send_response(206)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(byBody)))
        self.send_header("Content-Range",
                         "bytes %d-%d/%d" % (nStart, nEnd, u64Total))
        self._send_common_headers()
        self.end_headers()
        if bIncludeBody:
            self._write_body(byBody)

    def _parse_range(self):
        strValue = self.headers.get("Range", "")
        if not strValue.startswith("bytes="):
            return None
        strSpec = strValue[len("bytes="):]
        strStart, strSep, strEnd = strSpec.partition("-")
        if not strSep:
            return None
        if strStart == "":
            return None  # suffix ranges are not used by the downloader
        nStart = int(strStart)
        nEnd = int(strEnd) if strEnd != "" else len(TestConfig.content) - 1
        return (nStart, nEnd)

    def _send_full(self, nStatus, byBody, strType):
        self.send_response(nStatus)
        self.send_header("Content-Type", strType)
        self.send_header("Content-Length", str(len(byBody)))
        self._send_common_headers()
        self.end_headers()
        self._write_body(byBody)

    def _respond(self, nStatus, byBody, bIncludeBody):
        self.send_response(nStatus)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(byBody)))
        self._send_common_headers()
        self.end_headers()
        if bIncludeBody:
            self._write_body(byBody)

    def _send_common_headers(self):
        self.send_header("ETag", TestConfig.etag)
        self.send_header("Last-Modified", "Mon, 01 Jan 2024 00:00:00 GMT")
        if not TestConfig.ignore_range:
            self.send_header("Accept-Ranges", "bytes")

    def _write_body(self, byBody):
        nWritten = 0
        nChunk = 65536
        while nWritten < len(byBody):
            if (TestConfig.truncate_after > 0 and
                    nWritten >= TestConfig.truncate_after):
                self.connection.close()
                return
            nNext = min(nChunk, len(byBody) - nWritten)
            if TestConfig.throttle_bps > 0:
                time.sleep(nNext / float(TestConfig.throttle_bps))
            try:
                self.wfile.write(byBody[nWritten:nWritten + nNext])
                self.wfile.flush()
            except (ConnectionError, BrokenPipeError):
                return
            nWritten += nNext


def main():
    tParser = argparse.ArgumentParser(description=__doc__)
    tParser.add_argument("--port", type=int, default=0,
                         help="listen port (0 = auto-assign)")
    tParser.add_argument("--size", type=int, default=1024 * 1024,
                         help="payload size in bytes")
    tParser.add_argument("--seed", type=int, default=1,
                         help="payload derivation seed")
    tParser.add_argument("--ignore-range", action="store_true",
                         help="serve 200 full body even for Range requests")
    tParser.add_argument("--throttle-bps", type=int, default=0,
                         help="per-connection bandwidth limit in bytes/sec")
    tParser.add_argument("--truncate-after", type=int, default=0,
                         help="drop the connection after N bytes (per request)")
    tArgs = tParser.parse_args()

    TestConfig.content = MakeContent(tArgs.seed, tArgs.size)
    TestConfig.etag = '"%s"' % hashlib.sha256(TestConfig.content).hexdigest()
    TestConfig.ignore_range = tArgs.ignore_range
    TestConfig.throttle_bps = tArgs.throttle_bps
    TestConfig.truncate_after = tArgs.truncate_after

    with socketserver.TCPServer(("127.0.0.1", tArgs.port),
                                TestHandler) as tServer:
        nPort = tServer.server_address[1]
        print("LISTENING %d" % nPort, flush=True)
        tServer.serve_forever()


if __name__ == "__main__":
    main()
