#!/usr/bin/env python3
"""
Backward-compatible wrapper: build the slim runtime-only archive.

    python3 scripts/make_slim_zip.py dist

CI (.github/workflows/ci.yml) and older docs call this script and then upload
dist/win7-tts-runtime-only.zip.  All the packaging logic now lives in
make_dist.py, which stamps a version into the archive name and *also* writes
this legacy filename, so the wrapper simply forwards to it and keeps the old
contract intact.  See make_dist.py for why the layout is curated and why
models.json / sahel.ttf ride along with the runtime.
"""

import os
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "dist"
    script = os.path.join(REPO, "scripts", "make_dist.py")
    return subprocess.call([sys.executable, script, outdir, "--flavors", "runtime"])


if __name__ == "__main__":
    sys.exit(main())
