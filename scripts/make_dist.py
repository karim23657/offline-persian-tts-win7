#!/usr/bin/env python3
"""
Build the release archives into dist/.

    python3 scripts/make_dist.py [outdir] [--flavors runtime,models,full]

Produces three version-stamped archives, so a release can offer the program
with or without the models instead of one 100 MB blob:

    win7-tts-<ver>-runtime.zip   program only  (small, fetch a model once)
    win7-tts-<ver>-models.zip    models only   (drop onto an existing install)
    win7-tts-<ver>-full.zip      program + models (works offline, no internet)

plus dist/SHA256SUMS.txt so a download can be verified, and a copy of the
runtime archive under the legacy name win7-tts-runtime-only.zip that the bug
report template and old links still point at.

Why a curated file list and not "zip the repo"
---------------------------------------------
The old release zipped the whole working tree, which shipped src/, .github/,
the Makefile and any stray build litter to end users. A release archive should
contain exactly what a Windows user needs to run the thing. Everything here is
explicit, and the user-data files are excluded on purpose (see EXCLUDE below).

Why models.json and sahel.ttf ride along with the runtime archive
----------------------------------------------------------------
They are not optional decorations: the GUI reads models.json to populate its
download tab and loads sahel.ttf for the Persian UI, and the web UI embeds the
same font. Shipping runtime/ without them would leave a download tab that says
"models.json not found" and a Persian UI that falls back to a font without
Arabic-script glyphs. Both are small (7 KB and 74 KB) and belong at the package
root next to runtime/.

Why espeak-ng-data travels inside models/
-----------------------------------------
sherpa-onnx needs the phonemiser data next to the model it loads. get_model.cmd
installs it once (either at models/espeak-ng-data or inside each model folder),
so copying models/ wholesale carries it along automatically.
"""

import argparse
import hashlib
import os
import re
import shutil
import sys
import zipfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# Files that must never ship: they are per-user state, not part of the package.
# user.dict is edited at runtime by the GUI/web/CLI; out.wav and *.part are
# transient outputs; .patched/_diag_* are build/test litter.
EXCLUDE_EXACT = {"user.dict", "out.wav", ".patched", ".DS_Store", "Thumbs.db"}
EXCLUDE_PREFIX = ("_diag_",)
EXCLUDE_SUFFIX = (".part", ".wav", ".pyc")

# Package-root files every flavor carries (docs + the two assets the UI needs).
ROOT_DOCS = ["README.md", "CHANGELOG.md", "LICENSE", "models.json", "sahel.ttf"]
# These live in docs/ rather than the root; look in both so a moved file cannot
# silently drop out while a START-HERE still points at it.
DOC_CANDIDATES = ["how_to.md", "win7.md"]


def version():
    """Single source of truth: the SAY_VERSION string in say.c."""
    path = os.path.join(REPO, "src", "say", "say.c")
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            m = re.search(r'SAY_VERSION\s+"([^"]+)"', fh.read())
            if m:
                return m.group(1)
    except OSError:
        pass
    return "0.0.0"


def excluded(name):
    if name in EXCLUDE_EXACT:
        return True
    if name.startswith(EXCLUDE_PREFIX):
        return True
    return name.endswith(EXCLUDE_SUFFIX)


def copy_tree_filtered(src, dst, extra_ignores=()):
    """Copy a directory, dropping user-data and transient files.

    Returns (files_copied, bytes).  Keeps the copy honest: nothing from
    EXCLUDE ever reaches an archive.
    """
    files = nbytes = 0
    ignore = set(extra_ignores)
    for folder, dirs, names in os.walk(src):
        dirs[:] = sorted(d for d in dirs
                         if d != "__pycache__" and d not in ignore)
        rel_dir = os.path.relpath(folder, src)
        out_dir = dst if rel_dir == "." else os.path.join(dst, rel_dir)
        os.makedirs(out_dir, exist_ok=True)
        for name in sorted(names):
            if excluded(name) or name in ignore:
                continue
            full = os.path.join(folder, name)
            shutil.copy2(full, os.path.join(out_dir, name))
            files += 1
            nbytes += os.path.getsize(full)
    return files, nbytes


def installed_models():
    """Model folders under models/ that actually contain a .onnx (plus espeak)."""
    found = []
    models_dir = os.path.join(REPO, "models")
    if not os.path.isdir(models_dir):
        return found
    for name in sorted(os.listdir(models_dir)):
        path = os.path.join(models_dir, name)
        if not os.path.isdir(path):
            continue
        has_onnx = any(f.endswith(".onnx") for f in os.listdir(path)
                       if os.path.isfile(os.path.join(path, f)))
        if has_onnx or name.startswith("espeak"):
            found.append(name)
    return found


def zip_dir(staging_root, archive):
    """Zip a staging folder so the archive has one tidy top-level directory."""
    if os.path.exists(archive):
        os.remove(archive)
    top = os.listdir(staging_root)[0]
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        base = os.path.join(staging_root, top)
        for folder, _dirs, names in os.walk(base):
            for name in sorted(names):
                full = os.path.join(folder, name)
                arc = os.path.relpath(full, staging_root)
                zf.write(full, arc)
    return os.path.getsize(archive)


def write_root_docs(root, start_here):
    for name in ROOT_DOCS + DOC_CANDIDATES:
        for candidate in (os.path.join(REPO, name), os.path.join(REPO, "docs", name)):
            if os.path.isfile(candidate):
                shutil.copy2(candidate, os.path.join(root, name))
                break
    if start_here:
        with open(os.path.join(root, "START-HERE.txt"), "w", encoding="utf-8") as fh:
            fh.write(start_here)


def stage_flavor(kind, ver, staging):
    """Stage one flavor's tree.  Returns the top-level folder name."""
    models = installed_models()
    if kind == "models":
        top = "win7-tts-%s-models" % ver
    else:
        top = "win7-tts-%s" % ver
    root = os.path.join(staging, top)
    os.makedirs(root)

    if kind in ("runtime", "full"):
        copy_tree_filtered(os.path.join(REPO, "runtime"), os.path.join(root, "runtime"))
        copy_tree_filtered(os.path.join(REPO, "scripts"), os.path.join(root, "scripts"))
        write_root_docs(root, START_HERE_FULL if kind == "full" else START_HERE_RUNTIME)

    if kind in ("models", "full"):
        # Carry espeak-ng-data and every model folder as-is.
        copy_tree_filtered(os.path.join(REPO, "models"), os.path.join(root, "models"))
        if kind == "models":
            with open(os.path.join(root, "README-MODELS.txt"), "w",
                      encoding="utf-8") as fh:
                fh.write(README_MODELS % {"models": "\n".join("  " + m for m in models)})
    elif kind == "runtime":
        # An empty models/ on purpose: the folder existing (but empty) is less
        # confusing to a new user than its absence, and get_model.cmd fills it.
        os.makedirs(os.path.join(root, "models"))

    return top, models


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description="build win7-tts release archives")
    ap.add_argument("outdir", nargs="?", default="dist")
    ap.add_argument("--flavors", default="runtime,models,full",
                    help="comma list of runtime,models,full")
    args = ap.parse_args()

    ver = version()
    out_dir = os.path.abspath(args.outdir)
    os.makedirs(out_dir, exist_ok=True)
    flavors = [f.strip() for f in args.flavors.split(",") if f.strip()]
    models = installed_models()

    built = []
    for kind in flavors:
        staging = os.path.join(out_dir, "_stage_%s" % kind)
        shutil.rmtree(staging, ignore_errors=True)
        os.makedirs(staging)
        top, _ = stage_flavor(kind, ver, staging)
        archive = os.path.join(out_dir, "win7-tts-%s-%s.zip" % (ver, kind))
        size = zip_dir(staging, archive)
        shutil.rmtree(staging, ignore_errors=True)
        built.append(archive)
        print("  %-44s %7.1f MB" % (os.path.relpath(archive), size / 1048576.0))
        if kind in ("models", "full") and not models:
            print("      note: no models/ found; archive contains an empty models folder.")

    # Legacy name the bug-report template and older links still reference.
    runtime_zip = os.path.join(out_dir, "win7-tts-%s-runtime.zip" % ver)
    legacy = os.path.join(out_dir, "win7-tts-runtime-only.zip")
    if os.path.exists(runtime_zip):
        if os.path.exists(legacy):
            os.remove(legacy)
        try:
            os.link(runtime_zip, legacy)   # hardlink: no extra bytes on disk
        except OSError:
            shutil.copy2(runtime_zip, legacy)
        built.append(legacy)
        print("  %-44s (legacy name)" % os.path.relpath(legacy))

    sums = os.path.join(out_dir, "SHA256SUMS.txt")
    with open(sums, "w", encoding="utf-8") as fh:
        for path in sorted(set(built)):
            fh.write("%s  %s\n" % (sha256(path), os.path.basename(path)))
    print("  %s" % os.path.relpath(sums))

    print("  models packaged: %s" % (", ".join(models) if models else "(none)"))
    return 0


START_HERE_RUNTIME = """win7-tts (runtime only - no model included)
============================================

This smaller archive has the program but NOT the models, so it is quick to
download. Fetch a model once with an internet connection:

    scripts\\get_model.cmd

Pick 1 (gyro) from the menu - it is the best-sounding Persian model - or run

    scripts\\get_model.cmd gyro

directly. It downloads about 64 MB and unpacks itself into models\\.

Then start the interface:

    scripts\\gui.cmd

or from the command line:

    scripts\\say.cmd --file hello.txt

If anything crashes, run this first and send me the output:

    scripts\\diagnose.cmd

It needs a model installed to run its synthesis test, which is the step most
likely to crash.

Want everything in one download?
-------------------------------
The -full archive contains this program plus the models already downloaded, so
it works straight away with no internet. Take that one if you are not in a
hurry. The -models archive is just the models, to drop onto an existing copy.

Prerequisites
-------------
1. 64-bit Windows 7 SP1 on a CPU with AVX support (2011 or newer).
2. The Visual C++ 2015-2022 x64 redistributable:
   https://aka.ms/vs/17/release/vc_redist.x64.exe
   The engine needs VCRUNTIME140.dll and MSVCP140.dll, which are not part of
   Windows 7. Install once; skip it and nothing will start.

Two messages that look like errors but are harmless
---------------------------------------------------
  "Failed to create DXGI factory."      onnxruntime probing for a GPU that is
                                       not there. Everything runs on the CPU.
  "Skip unknown phonemes ..."          some models have a small phoneme
                                       vocabulary and drop sounds they lack.

Documentation
-------------
README.md     how to use it
how_to.md    how it works, and the bugs to avoid if you modify it
"""

START_HERE_FULL = """win7-tts (complete - models included, works offline)
=====================================================

Everything is already here: the program AND the Persian models. No internet is
needed at any point.

Start the graphical interface:

    scripts\\gui.cmd

or the browser interface (http://127.0.0.1:8756/):

    scripts\\server.cmd

or from the command line:

    scripts\\say.cmd --file hello.txt

The default voice is gyro, the best-sounding Persian model. To add another
voice later (still needs internet once):

    scripts\\get_model.cmd

If anything crashes, run this first and send me the output:

    scripts\\diagnose.cmd

Prerequisites
-------------
1. 64-bit Windows 7 SP1 on a CPU with AVX support (2011 or newer).
2. The Visual C++ 2015-2022 x64 redistributable:
   https://aka.ms/vs/17/release/vc_redist.x64.exe
   The engine needs VCRUNTIME140.dll and MSVCP140.dll, which are not part of
   Windows 7. Install once; skip it and nothing will start.

Two messages that look like errors but are harmless
---------------------------------------------------
  "Failed to create DXGI factory."      onnxruntime probing for a GPU that is
                                       not there. Everything runs on the CPU.
  "Skip unknown phonemes ..."          some models have a small phoneme
                                       vocabulary and drop sounds they lack.

Documentation
-------------
README.md     how to use it
how_to.md    how it works, and the bugs to avoid if you modify it
"""

README_MODELS = """win7-tts models
===============

These are the pre-downloaded voices. To use them, copy the models\\ folder from
here next to the runtime\\ folder of a win7-tts install (merge the two), so you
end up with:

    win7-tts\\runtime\\   the program (from the -runtime or -full archive)
    win7-tts\\models\\    the voices (this archive)

then run scripts\\gui.cmd or scripts\\server.cmd as usual.

Models in this archive:
%(models)s

Prefer one download instead? The win7-tts-<version>-full.zip archive contains
the program and these models together and works offline out of the box.
"""


if __name__ == "__main__":
    sys.exit(main())
