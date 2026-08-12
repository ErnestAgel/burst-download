#!/usr/bin/env python3
"""Download regression runner for the Burst CLI against the local server.

Usage:
  python run_download_regression.py --burst ./build/burst

Cases:
  1. Range-supported server: multi-threaded download, verify size + SHA-256.
  2. Range-ignoring server: whole-file single-stream fallback, verify hash.
  3. --version sanity check.
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import time

from test_http_server import MakeContent

DW_SEED = 42
U64_SIZE = 2 * 1024 * 1024


def StartServer(bIgnoreRange):
    """Start the test server and return (process, port)."""
    strScript = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "test_http_server.py")
    lstCmd = [sys.executable, strScript, "--seed", str(DW_SEED),
              "--size", str(U64_SIZE)]
    if bIgnoreRange:
        lstCmd.append("--ignore-range")
    tProc = subprocess.Popen(lstCmd, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
    nPort = None
    fDeadline = time.time() + 15.0
    while time.time() < fDeadline:
        strLine = tProc.stdout.readline()
        if strLine.startswith("LISTENING "):
            nPort = int(strLine.split()[1])
            break
    if nPort is None:
        tProc.terminate()
        raise RuntimeError("test server failed to start")
    return (tProc, nPort)


def StopServer(tProc):
    """Stop the test server process."""
    if tProc is not None:
        tProc.terminate()
        try:
            tProc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            tProc.kill()


def RunBurst(strBurst, strCwd, lstArgs):
    """Run the burst CLI and return (returncode, output)."""
    tResult = subprocess.run([strBurst] + lstArgs, cwd=strCwd,
                             capture_output=True, text=True, timeout=120)
    return (tResult.returncode, tResult.stdout + tResult.stderr)


def CheckDownload(strBurst, strCwd, strName, strUrl, bIgnoreRange):
    """Download once and verify size + content hash."""
    strOut = os.path.join(strCwd, strName)
    nCode, strOutput = RunBurst(strBurst, strCwd,
                                [strUrl, "-o", strName, "-t", "2"])
    if nCode != 0:
        print("FAIL %s: exit=%d output=%s" % (strName, nCode, strOutput))
        return False
    byActual = open(strOut, "rb").read()
    byExpected = MakeContent(DW_SEED, U64_SIZE)
    if len(byActual) != len(byExpected):
        print("FAIL %s: size %d != %d" % (strName, len(byActual),
                                          len(byExpected)))
        return False
    if hashlib.sha256(byActual).hexdigest() != \
            hashlib.sha256(byExpected).hexdigest():
        print("FAIL %s: content hash mismatch" % strName)
        return False
    print("PASS %s" % strName)
    return True


def main():
    tParser = argparse.ArgumentParser(description=__doc__)
    tParser.add_argument("--burst", required=True,
                         help="path to the burst CLI executable")
    tArgs = tParser.parse_args()

    strBurst = os.path.abspath(tArgs.burst)
    if not os.path.isfile(strBurst):
        print("burst executable not found: %s" % strBurst)
        return 1

    nCode, strOutput = RunBurst(strBurst, tempfile.gettempdir(),
                                ["--version"])
    if nCode != 0:
        print("FAIL --version: exit=%d output=%s" % (nCode, strOutput))
        return 1
    print("PASS --version")

    bAllPass = True
    tProc = None
    with tempfile.TemporaryDirectory(prefix="burst-regression-") as strTmp:
        for bIgnoreRange, strName in ((False, "range.bin"),
                                      (True, "norange.bin")):
            tProc, nPort = StartServer(bIgnoreRange)
            try:
                strUrl = "http://127.0.0.1:%d/file.bin" % nPort
                bOk = CheckDownload(strBurst, strTmp, strName, strUrl,
                                    bIgnoreRange)
                bAllPass = bAllPass and bOk
            finally:
                StopServer(tProc)
                tProc = None

    if bAllPass:
        print("download regression: ALL PASS")
        return 0
    print("download regression: FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
