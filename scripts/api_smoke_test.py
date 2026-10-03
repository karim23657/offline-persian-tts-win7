#!/usr/bin/env python3
"""
End-to-end test of the HTTP server: start it, exercise every endpoint, stop it.

This test exists because every unit test passed while the server was silently
broken: the request body was never read, so the engine received uninitialised
stack memory and produced near-silence while still reporting success.  Only a
test that speaks real HTTP notices that kind of failure.

    python3 scripts/api_smoke_test.py --port 8799

Exits non-zero on the first failure.
"""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import wave

FAILURES = []


def check(name, ok, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           ("  - " + detail) if detail and not ok else ""))
    if not ok:
        FAILURES.append(name)


def wait_for_port(port, timeout=120):
    """--preload loads the model first, so allow generous time."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=2):
                return True
        except OSError:
            time.sleep(0.5)
    return False


def get(port, path, timeout=120):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path),
                                timeout=timeout) as r:
        return r.status, r.read(), r.headers.get("Content-Type", "")


def post_json(port, path, payload, timeout=180):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path),
                                 data=data,
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def wav_stats(blob):
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as fh:
        fh.write(blob)
        path = fh.name
    try:
        with wave.open(path) as w:
            frames = w.readframes(w.getnframes())
            n, rate = w.getnframes(), w.getframerate()
        peak = max(abs(v) for v in struct.unpack("<%dh" % (len(frames) // 2), frames))
        return n / float(rate), peak, rate
    finally:
        os.unlink(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="runtime/tts_server.exe")
    ap.add_argument("--port", type=int, default=8799)
    ap.add_argument("--models", default=None)
    ap.add_argument("--skip-synthesis", action="store_true")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("  [SKIP] %s not built" % args.exe)
        return 0
    if shutil.which("wine") is None:
        print("  [SKIP] wine not available")
        return 0

    env = dict(os.environ)
    env.setdefault("WINEPREFIX", "/tmp/wp7")
    env["WINEDEBUG"] = "-all"

    # On Linux a PE binary has to go through wine; on Windows it runs directly.
    cmd = [exe]
    if os.name != "nt":
        cmd = ["wine"] + cmd
    cmd += ["--port", str(args.port)]
    cmd += ["--models", args.models] if args.models else ["--preload"]

    print("  starting: %s" % " ".join(cmd))
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        check("server accepts connections", wait_for_port(args.port))

        status, body, _ = get(args.port, "/api/health", timeout=90)
        health = json.loads(body)
        check("GET /api/health -> 200", status == 200)
        check("health reports ok", health.get("status") == "ok", str(health))

        status, body, _ = get(args.port, "/api/models", timeout=90)
        models = json.loads(body)
        check("GET /api/models -> 200", status == 200)
        check("models is a list", isinstance(models.get("models"), list))

        status, body, ctype = get(args.port, "/", timeout=90)
        check("GET / -> 200 html", status == 200 and "text/html" in ctype)
        check("html is the real UI", b"win7-tts" in body and b"api/synthesize" in body)
        status, body, ctype = get(args.port, "/app.js", timeout=90)
        check("GET /app.js -> 200 js", status == 200 and "javascript" in ctype)

        try:
            get(args.port, "/no/such/thing", timeout=30)
            check("unknown path -> 404", False, "no error raised")
        except urllib.error.HTTPError as e:
            check("unknown path -> 404", e.code == 404, "got %d" % e.code)

        try:
            post_json(args.port, "/api/synthesize", {"text": ""})
            check("empty text -> 400", False, "no error raised")
        except urllib.error.HTTPError as e:
            check("empty text -> 400", e.code == 400, "got %d" % e.code)

        if args.skip_synthesis:
            return 0
        name = models["models"][0]["name"] if models.get("models") else None
        if not name:
            check("synthesis", False, "no model installed")
            return 1

        status, blob = post_json(args.port, "/api/synthesize",
                                 {"text": "hello world, this is a test",
                                  "model": name})
        check("POST /api/synthesize -> 200", status == 200)
        secs, peak, rate = wav_stats(blob)
        print("         ascii : %.2f s, peak %d, %d Hz" % (secs, peak, rate))
        check("ascii wav is plausible (>1s)", secs > 1.0, "%.3f" % secs)
        check("ascii wav is not silence", peak > 1000, "peak %d" % peak)

        status, blob = post_json(args.port, "/api/synthesize",
                                 {"text": "سلام دنیا! این یک آزمایش است.",
                                  "model": name})
        secs, peak, rate = wav_stats(blob)
        print("         persian: %.2f s, peak %d, %d Hz" % (secs, peak, rate))
        check("persian produces real audio", secs > 1.0 and peak > 1000,
              "%.3f s peak %d" % (secs, peak))

        t0 = time.time()
        post_json(args.port, "/api/synthesize",
                  {"text": "سلام دنیا! این یک آزمایش است.", "model": name})
        warm = time.time() - t0
        print("         warm repeat: %.2f s (model was already loaded)" % warm)
        check("warm repeat is not slower than cold", warm <= secs + 1.0,
              "warm %.2f s vs %.2f s of audio" % (warm, secs))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("RESULT: %s" % ("ALL PASS" if not FAILURES else
                          "FAILURES %s" % ", ".join(FAILURES)))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
