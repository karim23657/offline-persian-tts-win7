#!/usr/bin/env python3
"""
Build the small "runtime only" archive.

    python3 scripts/make_slim_zip.py dist

Ships runtime/, scripts/ and the docs but *no* model, so it downloads in a few
seconds.  Users fetch a model once with scripts\\get_model.cmd.

Keeping this in Python rather than a Makefile recipe keeps the START-HERE text
readable and lets the model directory be created empty on purpose, so users are
not confused by a models/ folder that exists but is empty.
"""

import os
import shutil
import sys
import zipfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

START_HERE = """win7-tts (runtime only - no model included)
============================================

This smaller archive has the program but NOT the 63 MB model, so it is quick to
download. Fetch the model once with an internet connection:

    scripts\\get_model.cmd

Pick 1 (gyro) from the menu - it is the best-sounding Persian model - or run
    scripts\\get_model.cmd gyro
directly. It downloads about 80 MB and unpacks itself into models\\.

Then start the interface:

    scripts\\gui.cmd

or from the command line:

    scripts\\say.cmd --file hello.txt

If anything crashes, run this first and send me the output:

    scripts\\diagnose.cmd

It needs a model installed to run its synthesis test, which is the step most
likely to crash.

Prerequisites
-------------
1. 64-bit Windows 7 SP1 on a CPU with AVX support (2011 or newer).
2. The Visual C++ 2015-2022 **x64** redistributable:
   https://aka.ms/vs/17/release/vc_redist.x64.exe
   The engine needs VCRUNTIME140.dll and MSVCP140.dll, which are not part of
   Windows 7. Install once; skip it and nothing will start.

Two messages that look like errors but are harmless
----------------------------------------------------
  "Failed to create DXGI factory."      onnxruntime probing for a GPU that is
                                       not there. Everything runs on the CPU.
  "Skip unknown phonemes ..."          some models have a small phoneme
                                       vocabulary and drop sounds they lack.

Full archive
------------
win7-tts.zip contains this plus the model already downloaded, so it works
straight away with no internet. Take that one if you are not in a hurry.

Documentation
-------------
README.md     how to use it
how_to.md    how it works, and the bugs to avoid if you modify it
win7.md      how to build the Wine-as-Windows-7 test environment
"""

INCLUDED_DIRS = ["runtime", "scripts"]
INCLUDED_FILES = ["README.md", "how_to.md", "win7.md", "LICENSE", "CHANGELOG.md"]


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "dist"
    os.makedirs(out_dir, exist_ok=True)
    staging = os.path.join(out_dir, "_slim")
    shutil.rmtree(staging, ignore_errors=True)
    root = os.path.join(staging, "win7-tts")
    os.makedirs(root)

    for name in INCLUDED_DIRS:
        src = os.path.join(REPO, name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(root, name),
                            ignore=shutil.ignore_patterns("*.wav", "__pycache__"))
    for name in INCLUDED_FILES:
        src = os.path.join(REPO, name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(root, name))
    os.makedirs(os.path.join(root, "models"), exist_ok=True)
    with open(os.path.join(root, "START-HERE.txt"), "w", encoding="utf-8") as fh:
        fh.write(START_HERE)

    archive = os.path.join(out_dir, "win7-tts-runtime-only.zip")
    if os.path.exists(archive):
        os.remove(archive)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for folder, _dirs, files in os.walk(staging):
            for name in sorted(files):
                full = os.path.join(folder, name)
                zf.write(full, os.path.relpath(full, staging))
    shutil.rmtree(staging, ignore_errors=True)

    size = os.path.getsize(archive) / (1024 * 1024)
    print("  %s (%.1f MB)" % (archive, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
