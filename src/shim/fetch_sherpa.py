#!/usr/bin/env python3
"""
Download the pinned sherpa-onnx Windows release and unpack it *unpatched*.

Build products are never committed to this repository, so this is the first
step of a clean build: it produces a pristine copy of the upstream binaries in
build/sherpa/ that `patch_pe.py` then modifies.

    python3 src/shim/fetch_sherpa.py [--dir build/sherpa] [--force]

The version is read from src/shim/sherpa-version.env so that upgrading is a
one-line change.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
VERSION_FILE = os.path.join(HERE, "sherpa-version.env")
BASE_URL = "https://github.com/k2-fsa/sherpa-onnx/releases/download"


def read_version(path=VERSION_FILE):
    """Parse KEY=VALUE pairs, expanding $(OTHER_VAR) references.

    The env file writes the asset name as
    ``sherpa-onnx-v$(SHERPA_VERSION)-...`` so the version only ever appears
    once; without expansion the download 404s on a literal ``$(...)``.
    """
    raw = {}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            value = value.strip()
            # The file is shell-flavoured for readability; strip the quotes.
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            raw[key.strip()] = value

    values = {}
    for _ in range(4):  # a couple of passes is plenty for our file
        changed = False
        for key, value in raw.items():
            new = value
            for other, replacement in values.items():
                new = new.replace("$(" + other + ")", replacement)
            if new != value:
                changed = True
            values[key] = new
        if not changed:
            break

    if "SHERPA_VERSION" not in values or "$(" in values["SHERPA_VERSION"]:
        raise SystemExit("SHERPA_VERSION missing or unexpanded in %s" % path)
    return values


def asset_url(cfg):
    version = cfg["SHERPA_VERSION"]
    asset = cfg.get("SHERPA_WIN_ASSET") or \
        "sherpa-onnx-v%s-win-x64-shared-MD-Release.tar.bz2" % version
    return "%s/v%s/%s" % (BASE_URL, version, asset)


def download(url, dest):
    print("downloading %s" % url)
    print("         -> %s" % dest)
    tmp = dest + ".part"
    with urllib.request.urlopen(url) as response, open(tmp, "wb") as out:
        shutil.copyfileobj(response, out)
    os.replace(tmp, dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=os.path.join(REPO, "build", "sherpa"))
    ap.add_argument("--force", action="store_true", help="re-download even if present")
    args = ap.parse_args()

    cfg = read_version()
    url = asset_url(cfg)

    top = args.dir
    # The archive unpacks into a directory named after the asset.
    unpacked = os.path.join(top, "unpacked")
    marker = os.path.join(unpacked, ".complete")

    if os.path.exists(marker) and not args.force:
        print("sherpa %s already fetched at %s (use --force to refetch)"
              % (cfg["SHERPA_VERSION"], unpacked))
        return 0

    os.makedirs(top, exist_ok=True)
    archive = os.path.join(top, os.path.basename(url))

    if args.force or not os.path.exists(archive):
        download(url, archive)

    print("extracting")
    shutil.rmtree(unpacked, ignore_errors=True)
    os.makedirs(unpacked)
    with tarfile.open(archive, "r:bz2") as tar:
        tar.extractall(unpacked)

    # Collapse the single top-level directory so callers get stable paths.
    entries = [e for e in os.listdir(unpacked)
               if os.path.isdir(os.path.join(unpacked, e))]
    if len(entries) == 1:
        inner = os.path.join(unpacked, entries[0])
        for item in os.listdir(inner):
            shutil.move(os.path.join(inner, item), os.path.join(unpacked, item))
        os.rmdir(inner)

    open(marker, "w").close()
    print("ready: %s" % unpacked)
    return 0


if __name__ == "__main__":
    sys.exit(main())