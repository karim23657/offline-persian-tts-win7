#!/usr/bin/env python3
"""
Protocol-level test of tts_server's HTTP behaviour.

api_smoke_test.py proves the *routes* work; this file proves the *transport*:

    python3 scripts/protocol_test.py --port 8801

  - keep-alive: many requests on ONE connection (what a browser session does)
  - pipelining: two requests that arrive in a single TCP segment
  - HTTP/1.0 and "Connection: close" ending the connection when asked
  - honest 411 / 413 answers instead of a dropped socket
  - /api/help, /api/catalog, /api/dict (add + delete round trip)
  - /api/unload: keep-one semantics, then unload-all - the beacon the web UI
    sends when its window closes must free the model

Starts the server itself, against the model in models/.  Exits non-zero on the
first failed check.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

FAILURES = []
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL = "vits-piper-fa_IR-gyro-medium"


def check(name, ok, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           ("  - " + detail) if detail and not ok else ""))
    if not ok:
        FAILURES.append(name)


def wait_for_port(port, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), 1)
            s.close()
            return True
        except OSError:
            time.sleep(0.25)
    return False


class Conn:
    """A raw HTTP/1.1 connection with a read buffer (pipelining friendly)."""

    def __init__(self, port, timeout=30):
        self.port = port
        self.s = socket.create_connection(("127.0.0.1", port), timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        self.reqs = 0

    def send_raw(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.s.sendall(data)

    def request(self, method, path, body=None, ctype="application/json",
                extra=""):
        self.reqs += 1
        head = "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n" % (
            method, path, self.port)
        if body is not None:
            if isinstance(body, str):
                body = body.encode("utf-8")
            head += "Content-Type: %s\r\n" % ctype
            head += "Content-Length: %d\r\n" % len(body)
        head += extra
        head += "\r\n"
        self.send_raw(head)
        if body:
            self.s.sendall(body)
        return self.read_response()

    def read_response(self):
        while b"\r\n\r\n" not in self.buf:
            chunk = self.s.recv(4096)
            if not chunk:
                raise EOFError("connection closed before headers")
            self.buf += chunk
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        status = int(lines[0].split()[1])
        headers = {}
        for line in lines[1:]:
            if b":" in line:
                k, v = line.split(b":", 1)
                headers[k.strip().lower().decode()] = v.strip().decode()
        clen = int(headers.get("content-length", "0"))
        while len(rest) < clen:
            chunk = self.s.recv(4096)
            if not chunk:
                raise EOFError("connection closed before body")
            rest += chunk
        self.buf = rest[clen:]
        return status, headers, rest[:clen]

    def eof(self, timeout=8):
        """True when the server closes its side."""
        old = self.s.gettimeout()
        self.s.settimeout(timeout)
        try:
            while True:
                chunk = self.s.recv(4096)
                if not chunk:
                    return True
        except socket.timeout:
            return False
        except OSError:
            return True
        finally:
            self.s.settimeout(old)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8801)
    args = ap.parse_args()

    wine = shutil.which("wine")
    if not wine:
        print("FAIL: wine not found")
        return 1
    env = os.environ.copy()
    env.setdefault("WINEPREFIX", "/tmp/wp7")
    env["WINEDEBUG"] = "-all"

    log_path = os.path.join(tempfile.gettempdir(), "win7tts-protocol.log")
    log = open(log_path, "wb")
    print("  starting: wine runtime/tts_server.exe --port %d --preload" % args.port)
    proc = subprocess.Popen(
        [wine, os.path.join(REPO, "runtime", "tts_server.exe"),
         "--port", str(args.port), "--preload"],
        cwd=REPO, env=env, stdout=log, stderr=log)

    try:
        if not wait_for_port(args.port):
            print("FAIL: server did not start (see %s)" % log_path)
            return 1

        # ---- 1-3: keep-alive across several requests on ONE socket --------
        c1 = Conn(args.port)
        st, hd, _ = c1.request("GET", "/api/health")
        check("health 200 on keep-alive socket", st == 200)
        check("response advertises keep-alive",
              hd.get("connection") == "keep-alive" and "keep-alive" in hd,
              "connection=%r" % hd.get("connection"))
        st, _, _ = c1.request("GET", "/api/models")
        check("second request on the same socket", st == 200)
        st, _, _ = c1.request("GET", "/api/health")
        check("third request on the same socket", st == 200)

        # ---- 4: pipelining: two requests in one TCP segment ----------------
        c1.reqs += 2
        q1 = ("GET /api/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n"
              % args.port)
        q2 = ("GET /api/models HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n"
              % args.port)
        c1.send_raw(q1 + q2)
        st_a, _, _ = c1.read_response()
        st_b, _, _ = c1.read_response()
        check("pipelined requests both answered",
              st_a == 200 and st_b == 200, "%s %s" % (st_a, st_b))

        # ---- 5: HTTP/1.0 without keep-alive closes ------------------------
        c2 = Conn(args.port)
        c2.send_raw("GET /api/health HTTP/1.0\r\nHost: x\r\n\r\n")
        st, hd, _ = c2.read_response()
        check("HTTP/1.0 answered and told to close",
              st == 200 and hd.get("connection") == "close",
              "connection=%r" % hd.get("connection"))
        check("HTTP/1.0 connection then closed by server", c2.eof())
        c2.close()

        # ---- 6: Connection: close honored ---------------------------------
        c3 = Conn(args.port)
        st, hd, _ = c3.request("GET", "/api/health", extra="Connection: close\r\n")
        check("Connection: close honored",
              st == 200 and hd.get("connection") == "close" and c3.eof())
        c3.close()

        # ---- 7: POST without Content-Length -> 411 ------------------------
        c4 = Conn(args.port)
        c4.send_raw("POST /api/synthesize HTTP/1.1\r\n"
                    "Host: 127.0.0.1:%d\r\nConnection: close\r\n\r\n"
                    % args.port)
        st, _, _ = c4.read_response()
        check("POST without Content-Length answers 411", st == 411, str(st))
        c4.close()

        # ---- 8: oversized body -> 413 -------------------------------------
        c5 = Conn(args.port)
        c5.send_raw("POST /api/synthesize HTTP/1.1\r\n"
                    "Host: 127.0.0.1:%d\r\nContent-Length: 9000000\r\n"
                    "Connection: close\r\n\r\n" % args.port)
        st, _, _ = c5.read_response()
        check("oversized body answers 413", st == 413, str(st))
        c5.close()

        # ---- 9: /api/help -------------------------------------------------
        st, _, body = c1.request("GET", "/api/help")
        text = body.decode("utf-8", "replace")
        check("/api/help 200 and documents synthesize",
              st == 200 and "/api/synthesize" in text and "/api/unload" in text)

        # ---- 10: /api/catalog --------------------------------------------
        st, _, body = c1.request("GET", "/api/catalog")
        try:
            cat = json.loads(body.decode("utf-8"))
            n_items = len(cat.get("items", []))
        except ValueError:
            n_items = -1
        check("/api/catalog serves models.json", st == 200 and n_items > 0,
              "status=%s items=%s" % (st, n_items))

        # ---- 11: /api/dict round trip -------------------------------------
        st, _, body = c1.request("GET", "/api/dict")
        try:
            before = len(json.loads(body.decode("utf-8"))["entries"])
        except (ValueError, KeyError):
            before = -1
        check("/api/dict 200", st == 200 and before >= 0, "status=%s" % st)
        st, _, body = c1.request(
            "POST", "/api/dict",
            json.dumps({"word": "protocol-test-word", "repl": "replacement"}))
        after = before + 1 if st == 200 else -1
        check("/api/dict add entry", st == 200, body[:120].decode("utf8", "replace"))
        st, _, _ = c1.request(
            "POST", "/api/dict",
            json.dumps({"word": "protocol-test-word", "delete": True}))
        st2, _, body = c1.request("GET", "/api/dict")
        try:
            final = len(json.loads(body.decode("utf-8"))["entries"])
        except (ValueError, KeyError):
            final = -1
        check("/api/dict delete entry restores count",
              st == 200 and st2 == 200 and final == before,
              "before=%s after=%s final=%s" % (before, after, final))
        check("user.dict written on disk",
              os.path.exists(os.path.join(REPO, "user.dict")))

        # ---- 12: warmup / unload keep / unload all (the beacon) -----------
        st, _, body = c1.request("POST", "/api/warmup",
                                 json.dumps({"model": MODEL}))
        warm = json.loads(body.decode("utf-8")) if st == 200 else {}
        check("warmup loads the model", st == 200 and "loaded_ms" in warm,
              body[:120].decode("utf8", "replace"))

        st, _, body = c1.request("GET", "/api/health")
        h = json.loads(body.decode("utf-8"))
        resident1 = h.get("models_loaded", -1)
        check("model resident after warmup", resident1 >= 1, str(resident1))

        st, _, body = c1.request("POST", "/api/unload",
                                 json.dumps({"keep": MODEL}))
        un = json.loads(body.decode("utf-8")) if st == 200 else {}
        check("unload {keep} keeps the selected model resident",
              st == 200 and un.get("resident") == 1,
              "status=%s body=%s" % (st, body[:120].decode("utf8", "replace")))

        # Exactly what navigator.sendBeacon('/api/unload', '{}') puts on the
        # wire: text/plain body, no JSON content type, connection reusable.
        st, hd, body = c1.request("POST", "/api/unload", "{}",
                                  ctype="text/plain;charset=UTF-8")
        un = json.loads(body.decode("utf-8")) if st == 200 else {}
        check("beacon-style unload frees the model",
              st == 200 and un.get("resident") == 0 and un.get("unloaded") == 1,
              "status=%s body=%s" % (st, body[:120].decode("utf8", "replace")))
        check("unload answered on the same keep-alive socket",
              hd.get("connection") == "keep-alive")

        # ---- 13: synthesis over the reused socket (cold load again) -------
        st, _, body = c1.request(
            "GET", "/api/synthesize?text=" + "hello%20world")
        check("GET synthesize after unload reloads and answers",
              st == 200 and body[:4] == b"RIFF",
              "status=%s magic=%r" % (st, body[:4]))

        c1.reqs += 0   # (the pipelined pair was counted when it was sent)
        check("first socket served the whole session (>= 16 requests)",
              c1.reqs >= 16, "reqs=%d" % c1.reqs)

        c1.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()

    if FAILURES:
        print("RESULT: %d FAILED: %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("RESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
