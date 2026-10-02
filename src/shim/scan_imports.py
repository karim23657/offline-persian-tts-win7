#!/usr/bin/env python3
"""
Regenerate imports.txt from a set of Windows binaries.

imports.txt is the list of functions the shim must provide.  It must be derived
from the *actual* binaries rather than maintained by hand: an upstream release
that adds one new Windows 8+ import will otherwise produce a binary that still
fails to load on Windows 7, and the failure will look like a mystery.

    python3 src/shim/scan_imports.py build/sherpa/unpacked \
        build/sherpa/unpacked/bin/sherpa-onnx-offline-tts.exe ...

Reads the DLL name from each IMAGE_IMPORT_DESCRIPTOR via objdump.  Without
objdump it falls back to a pure-Python PE import-table parser, so this works on
a machine with no binutils.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "imports.txt")


# --------------------------------------------------------------------------
# pure-python PE reader (no dependencies)
# --------------------------------------------------------------------------
class PE(object):
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        self._parse()

    def _parse(self):
        if self.data[:2] != b"MZ":
            raise ValueError("not a PE file")
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError("bad PE signature")
        coff = e_lfanew + 4
        (_machine, nsec, _ts, _st, _ns, optsz,
         _ch) = struct.unpack_from("<HHIIIHH", self.data, coff)
        opt = coff + 20
        magic = struct.unpack_from("<H", self.data, opt)[0]
        if magic not in (0x10B, 0x20B):
            raise ValueError("unsupported optional header magic 0x%x" % magic)
        # NumberOfRvaAndSizes sits at offset 92 (PE32) or 108 (PE32+).
        num_off = opt + (92 if magic == 0x10B else 108)
        dd_off = num_off + 4
        (self.num_dirs,) = struct.unpack_from("<I", self.data, num_off)
        self.import_rva = self._dir(dd_off, 1)
        self.sections = []
        sec = opt + optsz
        for i in range(nsec):
            off = sec + i * 40
            name = self.data[off:off + 8].rstrip(b"\0").decode("latin-1")
            vs, va, rs, rp = struct.unpack_from("<IIII", self.data, off + 8)
            self.sections.append((va, vs, rp, rs, name))

    def _dir(self, dd_off, index):
        if index >= self.num_dirs:
            return 0
        return struct.unpack_from("<I", self.data, dd_off + 8 * index)[0]

    def rva_to_off(self, rva):
        for va, vs, rp, rs, _name in self.sections:
            if va <= rva < va + max(vs, rs):
                return rp + (rva - va)
        raise ValueError("RVA 0x%x outside any section" % rva)

    def cstr(self, rva):
        off = self.rva_to_off(rva)
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin-1")

    def imports(self):
        """{dll_lower: set(function_names)}"""
        result = {}
        if not self.import_rva:
            return result
        off = self.rva_to_off(self.import_rva)
        while True:
            oft, _ts, _fc, name_rva, ft = struct.unpack_from("<IIIII", self.data, off)
            if not any((oft, _ts, _fc, name_rva, ft)):
                break
            dll = self.cstr(name_rva).lower()
            names = result.setdefault(dll, set())
            table = oft or ft
            toff = self.rva_to_off(table)
            while True:
                (val,) = struct.unpack_from("<Q", self.data, toff)
                if val == 0:
                    break
                ordinal_flag = 1 << 63
                if not (val & ordinal_flag):
                    noff = self.rva_to_off(val & 0x7FFFFFFF)
                    end = self.data.index(b"\0", noff + 2)
                    names.add(self.data[noff + 2:end].decode("latin-1"))
                toff += 8
            off += 20
        return result


def read_version(path=os.path.join(HERE, "sherpa-version.env")):
    values = {}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                v = v.strip()
                if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                    v = v[1:-1]
                values[k.strip()] = v
    return values


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="PE binaries to scan")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--append", action="store_true",
                    help="add to the existing file instead of replacing it")
    args = ap.parse_args()

    cfg = read_version()
    redirect = {d.strip().lower()
                for d in cfg.get("SHERPA_REDIRECT_DLLS", "KERNEL32.dll").split()}

    merged = set()
    if args.append and os.path.exists(args.out):
        with open(args.out, "r", encoding="utf-8") as fh:
            for line in fh:
                # Skip comments: imports.txt carries a generated header, and
                # reading it as data injects words like "Functions" as
                # function names - which then reach the .def file and break
                # the link with a syntax error.
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if len(parts) > 1:
                    merged.update(parts[1:])

    have_objdump = shutil.which("objdump") is not None
    for path in args.files:
        if not os.path.exists(path):
            print("skip (missing): %s" % path, file=sys.stderr)
            continue
        if have_objdump:
            out = subprocess.run(["objdump", "-x", path], capture_output=True,
                                 text=True, errors="ignore").stdout
            cur, found = None, {}
            for line in out.split("Import Tables")[-1].split("\n"):
                m = re.search(r"DLL Name: (\S+)", line)
                if m:
                    cur = m.group(1).lower()
                    found.setdefault(cur, set())
                    continue
                m = re.match(r"\t[0-9a-f]+\s+\S+\s+(.+)$", line)
                if m and cur:
                    found[cur].add(m.group(1).strip())
        else:
            found = PE(path).imports()
        for dll, names in found.items():
            if dll in redirect:
                merged |= names
        print("scanned %-46s %d redirected imports"
              % (os.path.basename(path),
                 sum(len(v) for k, v in found.items() if k in redirect)))

    if not merged:
        print("error: no redirected imports found - wrong files?",
              file=sys.stderr)
        return 1

    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("# Generated by scan_imports.py - do not edit by hand.\n")
        fh.write("# Functions imported from %s that the shim must provide.\n"
                 % ", ".join(sorted(redirect)))
        fh.write("KERNEL32.dll " + " ".join(sorted(merged)) + "\n")
    print("wrote %s (%d functions)" % (args.out, len(merged)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
