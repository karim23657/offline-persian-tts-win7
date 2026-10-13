#!/usr/bin/env python3
"""Capture full-page screenshots of the web UI (fa + en) via Playwright.

Starts runtime/tts_server.exe under Wine on a spare port, screenshots the page
in Persian (default, RTL) and after clicking the language toggle (English, LTR),
then stops the server.  Writes screenshots/web-1-fa.png and web-2-en.png.
"""
import os, subprocess, time, sys, urllib.request, signal
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.join(ROOT, "screenshots")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8799
os.makedirs(SHOTS, exist_ok=True)

env = dict(os.environ, WINEPREFIX=os.environ.get("WINEPREFIX", "/tmp/wp7"),
           WINEDEBUG="-all")
srv = subprocess.Popen(
    ["wine", os.path.join(ROOT, "runtime", "tts_server.exe"),
     "--port", str(PORT), "--preload"],
    cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def wait_healthy(timeout=45):
    url = f"http://127.0.0.1:{PORT}/api/health"
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with urllib.request.urlopen(url, timeout=2) as r:
                if r.status == 200:
                    return True
        except Exception:
            time.sleep(0.5)
    return False

try:
    if not wait_healthy():
        print("server did not become healthy", file=sys.stderr)
        sys.exit(1)
    with sync_playwright() as p:
        b = p.chromium.launch()
        # Persian (default)
        pg = b.new_page(viewport={"width": 1280, "height": 900},
                        device_scale_factor=1)
        pg.goto(f"http://127.0.0.1:{PORT}/", wait_until="networkidle")
        pg.wait_for_timeout(1200)
        pg.screenshot(path=os.path.join(SHOTS, "web-1-fa.png"), full_page=True)
        # English
        pg.click("#langBtn")
        pg.wait_for_timeout(600)
        pg.screenshot(path=os.path.join(SHOTS, "web-2-en.png"), full_page=True)
        b.close()
    print("web screenshots written")
finally:
    srv.send_signal(signal.SIGTERM)
    try:
        srv.wait(timeout=10)
    except Exception:
        srv.kill()
