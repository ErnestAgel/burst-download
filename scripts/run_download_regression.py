#!/usr/bin/env python3
"""Download regression runner for the Burst CLI against the local server.

Usage:
  python run_download_regression.py --burst ./build/burst

Cases:
  1. Range-supported server: multi-threaded download, verify size + SHA-256.
  2. Range-ignoring server: whole-file single-stream fallback, verify hash.
  3. Resume integrity (R3): partial download, remote content changes (same
     size), resume detects the ETag change and redownloads the whole file.
  4. Retry resume (O1): a transient cut mid-chunk retries from the current
     offset (server Range log must show a chunk resumed at a later start).
  5. --version sanity check.
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import threading
import time

from test_http_server import MakeContent

DW_SEED = 42
U64_SIZE = 2 * 1024 * 1024


def StartServer(bIgnoreRange, seed=DW_SEED, size=U64_SIZE,
                truncate_total=0, drop_first_after=0):
    """Start the test server and return (process, port, log lines)."""
    strScript = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "test_http_server.py")
    lstCmd = [sys.executable, strScript, "--seed", str(seed),
              "--size", str(size)]
    if bIgnoreRange:
        lstCmd.append("--ignore-range")
    if truncate_total > 0:
        lstCmd += ["--truncate-total", str(truncate_total)]
    if drop_first_after > 0:
        lstCmd += ["--drop-first-request-after", str(drop_first_after)]
    tProc = subprocess.Popen(lstCmd, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
    lstLog = []

    def Drain():
        for strLine in tProc.stdout:
            lstLog.append(strLine.rstrip("\n"))

    tReader = threading.Thread(target=Drain, daemon=True)
    tReader.start()

    nPort = None
    fDeadline = time.time() + 15.0
    while time.time() < fDeadline:
        if lstLog:
            strLine = lstLog[0]
            if strLine.startswith("LISTENING "):
                nPort = int(strLine.split()[1])
                break
        time.sleep(0.05)
    if nPort is None:
        tProc.terminate()
        raise RuntimeError("test server failed to start")
    return (tProc, nPort, lstLog)


def StopServer(tProc):
    """Stop the test server process."""
    if tProc is not None:
        tProc.terminate()
        try:
            tProc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            tProc.kill()


def RunOnce(strBurst, strCwd, lstArgs):
    """Run the burst CLI once and return the CompletedProcess."""
    return subprocess.run([strBurst] + lstArgs, cwd=strCwd,
                          capture_output=True, text=True, timeout=120)


def CheckHash(strPath, seed, size):
    """Return TRUE when the file exists and matches the seeded content."""
    if not os.path.exists(strPath):
        return False
    byActual = open(strPath, "rb").read()
    byExpected = MakeContent(seed, size)
    return (len(byActual) == len(byExpected)) and \
        (hashlib.sha256(byActual).hexdigest() ==
         hashlib.sha256(byExpected).hexdigest())


def CaseRangeSupported(strBurst, strTmp):
    """Multi-threaded download against a Range-supporting server."""
    tProc, nPort, _ = StartServer(False)
    try:
        strUrl = "http://127.0.0.1:%d/file.bin" % nPort
        r = RunOnce(strBurst, strTmp,
                    [strUrl, "-o", "range.bin", "-t", "2"])
        bOk = (r.returncode == 0) and \
            CheckHash(os.path.join(strTmp, "range.bin"), DW_SEED, U64_SIZE)
        print("PASS range.bin" if bOk else "FAIL range.bin")
        return bOk
    finally:
        StopServer(tProc)


def CaseRangeIgnored(strBurst, strTmp):
    """Whole-file single-stream fallback against a Range-ignoring server."""
    tProc, nPort, _ = StartServer(True)
    try:
        strUrl = "http://127.0.0.1:%d/file.bin" % nPort
        r = RunOnce(strBurst, strTmp,
                    [strUrl, "-o", "norange.bin", "-t", "2"])
        bOk = (r.returncode == 0) and \
            CheckHash(os.path.join(strTmp, "norange.bin"),
                      DW_SEED, U64_SIZE)
        print("PASS norange.bin" if bOk else "FAIL norange.bin")
        return bOk
    finally:
        StopServer(tProc)


def CaseResumeEtag(strBurst, strTmp):
    """Issue R3: partial download, then the remote content changes (same
    size); resume must detect the ETag change and redownload the whole
    file."""
    strOut = os.path.join(strTmp, "r3.bin")
    strMeta = strOut + ".curlbolt.part"

    tProc, nPort, _ = StartServer(False, seed=DW_SEED, size=U64_SIZE,
                                  truncate_total=U64_SIZE // 2)
    try:
        strUrl = "http://127.0.0.1:%d/file.bin" % nPort
        r1 = RunOnce(strBurst, strTmp, [strUrl, "-o", "r3.bin", "-t", "2"])
        if r1.returncode == 0:
            print("FAIL r3: first download unexpectedly succeeded")
            return False
        if not os.path.exists(strMeta):
            print("FAIL r3: resume meta not created after partial download")
            return False
    finally:
        StopServer(tProc)

    # Second server: same size, different content (different ETag).
    tProc, nPort, _ = StartServer(False, seed=DW_SEED + 1, size=U64_SIZE)
    try:
        strUrl = "http://127.0.0.1:%d/file.bin" % nPort
        r2 = RunOnce(strBurst, strTmp,
                     [strUrl, "-o", "r3.bin", "-t", "2", "--continue"])
        bOk = (r2.returncode == 0) and \
            CheckHash(strOut, DW_SEED + 1, U64_SIZE)
        bOk = bOk and (not os.path.exists(strMeta))
        if bOk:
            print("PASS r3: etag change detected, full redownload")
        else:
            print("FAIL r3: resume download: rc=%d meta=%s" %
                  (r2.returncode, os.path.exists(strMeta)))
        return bOk
    finally:
        StopServer(tProc)


def CaseRetryOffset(strBurst, strTmp):
    """Issue O1: a transient cut mid-chunk must retry from the current
    offset, not from the chunk base."""
    tProc, nPort, lstLog = StartServer(False, seed=DW_SEED, size=U64_SIZE,
                                       drop_first_after=64 * 1024)
    try:
        strUrl = "http://127.0.0.1:%d/file.bin" % nPort
        r = RunOnce(strBurst, strTmp, [strUrl, "-o", "o1.bin", "-t", "2"])
        bOk = (r.returncode == 0) and \
            CheckHash(os.path.join(strTmp, "o1.bin"), DW_SEED, U64_SIZE)
        if not bOk:
            print("FAIL o1: download rc=%d" % r.returncode)
            return False

        # A chunk range requested twice with the second start later proves
        # the retry resumed from the current offset.
        dEndFirst = {}
        bResumed = False
        for strLine in lstLog:
            nPos = strLine.find("Range: bytes=")
            if nPos < 0:
                continue
            strSpec = strLine[nPos + len("Range: bytes="):].strip()
            strStart, strSep, strEnd = strSpec.partition("-")
            if not strSep:
                continue
            nStart = int(strStart)
            nEnd = int(strEnd)
            if nEnd in dEndFirst:
                if nStart > dEndFirst[nEnd]:
                    bResumed = True
                    break
            else:
                dEndFirst[nEnd] = nStart
        if bResumed:
            print("PASS o1: chunk retried from a later offset")
        else:
            print("FAIL o1: no chunk retried from a later offset (%s)" %
                  [s for s in lstLog if "Range:" in s][:8])
        return bResumed
    finally:
        StopServer(tProc)


def main():
    tParser = argparse.ArgumentParser(description=__doc__)
    tParser.add_argument("--burst", required=True,
                         help="path to the burst CLI executable")
    tArgs = tParser.parse_args()

    strBurst = os.path.abspath(tArgs.burst)
    if not os.path.isfile(strBurst):
        print("burst executable not found: %s" % strBurst)
        return 1

    r = RunOnce(strBurst, tempfile.gettempdir(), ["--version"])
    if r.returncode != 0:
        print("FAIL --version: %s" % r.stdout)
        return 1
    print("PASS --version")

    bAllPass = True
    with tempfile.TemporaryDirectory(prefix="burst-regression-") as strTmp:
        bAllPass = CaseRangeSupported(strBurst, strTmp) and bAllPass
        bAllPass = CaseRangeIgnored(strBurst, strTmp) and bAllPass
        bAllPass = CaseResumeEtag(strBurst, strTmp) and bAllPass
        bAllPass = CaseRetryOffset(strBurst, strTmp) and bAllPass

    if bAllPass:
        print("download regression: ALL PASS")
        return 0
    print("download regression: FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
