#!/usr/bin/env python3
"""
Redirect selected import DLLs of a PE32+ file to the Win7 shim.

Background
----------
Windows resolves imports by walking the IMAGE_IMPORT_DESCRIPTOR table in the
data directory named by the import directory entry (index 1).  Each descriptor
names the DLL it imports from plus the lookup/address tables for its functions.

The patch is deliberately tiny: we only change the 4-byte RVA that points at
the descriptor's DLL name string, so the descriptor now says "w7shim.dll"
instead of "KERNEL32.dll" / "api-ms-win-core-path-l1-1-0.dll" / "dxgi.dll".
Nothing else moves, so the file layout, checksum-of-imports and relocation
tables all stay valid.

Because the shim exports every function the host imported from those DLLs (see
gen_shim.py), the loader is satisfied either way.

Usage:
    python3 patch_pe.py <exe> [--dlls KERNEL32.dll dxgi.dll ...] [--dllname w7shim]
                        [--inplace] [--out patched.exe]

The default writes <exe>.win7.exe and leaves the original untouched.
"""

import argparse
import os
import struct
import sys

# Redirected by default.  dxgi.dll is here because onnxruntime.dll statically
# imports CreateDXGIFactory2 from it, which does not exist on Windows 7 (that is
# DXGI 1.2 / Windows 8).  Its name is too short to overwrite, so patch() repoints
# the descriptor's name RVA at the shim string instead.
DEFAULT_DLLS = (
    "KERNEL32.dll",
    "api-ms-win-core-path-l1-1-0.dll",
    "dxgi.dll",
)


class PEError(Exception):
    pass


class PE(object):
    """Just enough PE32+ parsing to reach the import directory."""

    def __init__(self, data):
        self.data = bytearray(data)
        if self.data[:2] != b"MZ":
            raise PEError("not a PE file (no MZ signature)")
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise PEError("bad PE signature")
        self.pe_off = e_lfanew

        # COFF header
        coff = e_lfanew + 4
        (self.machine, self.nsections, self.timestamp, self.symtab_ptr,
         self.nsyms, self.opthdr_size, self.characteristics) = struct.unpack_from(
            "<HHIIIHH", self.data, coff)

        opt = coff + 20
        magic = struct.unpack_from("<H", self.data, opt)[0]
        if magic != 0x20B:
            raise PEError("expected PE32+ (magic 0x20b), got 0x%x" % magic)
        self.opt_off = opt
        self.is_64 = True

        # Data directories start after the fixed part of the optional header.
        # For PE32+ that is 24 bytes (standard fields) + 88 bytes
        # (ImageBase/NumberOfRvaAndSizes and the Windows-specific fields) = 112
        # bytes, then NumberOfRvaAndSizes itself, then the directory array.
        self.num_dirs_off = opt + 108
        (self.num_dirs,) = struct.unpack_from("<I", self.data, self.num_dirs_off)
        self.dd_off = self.num_dirs_off + 4

        # Section table, needed to map RVAs to file offsets.
        sec_off = opt + self.opthdr_size
        self.sections = []
        for i in range(self.nsections):
            off = sec_off + i * 40
            name = bytes(self.data[off:off + 8]).rstrip(b"\0").decode("latin-1")
            (vsize, vaddr, rawsize, rawptr) = struct.unpack_from("<IIII", self.data, off + 8)
            self.sections.append(
                {"name": name, "vsize": vsize, "vaddr": vaddr,
                 "rawsize": rawsize, "rawptr": rawptr})

        # Import directory = data directory index 1, exports = index 0.
        (self.import_rva, self.import_size) = self._dir(1)
        (self.export_rva, self.export_size) = self._dir(0)

    def _dir(self, index):
        """RVA/size of data directory `index` (0 = export, 1 = import, ...)."""
        if index >= self.num_dirs:
            return 0, 0
        return struct.unpack_from("<II", self.data, self.dd_off + 8 * index)

    def rva_to_offset(self, rva):
        for s in self.sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rawsize"]):
                return s["rawptr"] + (rva - s["vaddr"])
        raise PEError("RVA 0x%x is not inside any section" % rva)

    def cstring_at_rva(self, rva):
        off = self.rva_to_offset(rva)
        end = self.data.index(b"\0", off)
        return bytes(self.data[off:end]).decode("latin-1"), off

    def descriptors(self):
        """Yield (descriptor_file_offset, name_rva, name) for each import descriptor."""
        if not self.import_rva:
            return
        off = self.rva_to_offset(self.import_rva)
        while True:
            fields = struct.unpack_from("<IIIII", self.data, off)
            oft, ts, fc, name_rva, ft = fields
            if not any(fields):  # null terminator
                return
            name = ""
            if name_rva:
                try:
                    name, _ = self.cstring_at_rva(name_rva)
                except PEError:
                    name = "<?>"
            yield off, name_rva, name
            off += 20

    def imported_functions(self, dll_name):
        """Names imported from one DLL - used to check shim coverage."""
        out = []
        for _, _, name in self.descriptors():
            if name.lower() != dll_name.lower():
                continue
            out.append(name)
        return out
    # ---- import-name walking (used by --verify) ----------------------------

    def _hint_name(self, thunk_rva):
        """Follow an ILT/IAT thunk to (hint, name); name is None for ordinals."""
        try:
            off = self.rva_to_offset(thunk_rva)
        except PEError:
            return None, None
        value = struct.unpack_from("<Q", self.data, off)[0]
        ordinal_flag = 1 << 63
        if value & ordinal_flag:
            return value & 0xFFFF, None
        if value == 0:
            return None, None
        try:
            noff = self.rva_to_offset(value & 0x7FFFFFFF)
            (hint,) = struct.unpack_from("<H", self.data, noff)
            end = self.data.index(b"\0", noff + 2)
            name = bytes(self.data[noff + 2:end]).decode("latin-1")
        except (PEError, ValueError):
            return None, None
        return hint, name

    def imports_of(self, dll_name):
        """All function names imported from one DLL, via its descriptor."""
        out = []
        for _, _, name in self.descriptors():
            if name.lower() != dll_name.lower():
                continue
            for _, name_rva, _ in self.descriptors():
                pass
        return out

    def descriptor_detail(self):
        """[(name, [thunk RVAs])] for every import descriptor."""
        result = []
        if not self.import_rva:
            return result
        off = self.rva_to_offset(self.import_rva)
        while True:
            oft, ts, fc, name_rva, ft = struct.unpack_from("<IIIII", self.data, off)
            if not any((oft, ts, fc, name_rva, ft)):
                break
            name = self.cstring_at_rva(name_rva)[0] if name_rva else ""
            # Prefer the original lookup table (OFT); fall back to the IAT,
            # which holds identical entries until the loader overwrites it.
            thunks = []
            table_rva = oft if oft else ft
            if table_rva:
                try:
                    toff = self.rva_to_offset(table_rva)
                except PEError:
                    toff = None
                if toff is not None:
                    rva = table_rva
                    while True:
                        (val,) = struct.unpack_from("<Q", self.data, toff)
                        if val == 0:
                            break
                        thunks.append(rva)
                        toff += 8
                        rva += 8
            result.append((name, thunks))
            off += 20
        return result

    def function_names(self, dll_name):
        """Set of function names imported from dll_name."""
        names = set()
        for name, thunks in self.descriptor_detail():
            if name.lower() != dll_name.lower():
                continue
            for trva in thunks:
                _, fn = self._hint_name(trva)
                if fn:
                    names.add(fn)
        return names


def find_free_name_slot(pe, dllname):
    """
    Find (offset, capacity) for a NUL-terminated string we can write into.

    The shim's name may be shorter than the DLL it replaces (typically
    "w7shim.dll" vs "api-ms-win-core-path-l1-1-0.dll"), so we can usually write
    it over the old name in place, padding the remainder with NULs.  All the
    redirected names are longer than the shim, which is what makes this safe.
    """
    for _, name_rva, name in pe.descriptors():
        off = pe.rva_to_offset(name_rva)
        return off, len(name) + 1
    raise PEError("no import descriptors found")


def patch(pe, dlls, dllname):
    """Point every matching import descriptor's DLL name at the shim.

    Two cases, because the shim name may or may not fit over the old one:

    * ``"KERNEL32.dll"`` and ``"api-ms-win-core-path-l1-1-0.dll"`` are long
      enough, so the name is overwritten in place with the shim name plus NUL
      padding.

    * ``"dxgi.dll"`` is *shorter* than ``"w7shim.dll"``, so it cannot be
      overwritten.  Instead the descriptor's Name RVA - which is just a pointer
      to a string - is repointed at the ``"w7shim.dll"`` string already written
      by an earlier descriptor.  Several descriptors may legitimately share one
      name string; the loader is happy with that.
    """
    targets = {d.lower() for d in dlls}
    changed = []
    shim_name_rva = None
    raw = dllname.encode("latin-1")

    for desc_off, name_rva, name in list(pe.descriptors()):
        if name.lower() not in targets:
            continue

        if len(raw) <= len(name):
            # Overwrite the name in place, NUL-padding whatever is left over.
            str_off = pe.rva_to_offset(name_rva)
            pe.data[str_off:str_off + len(name)] = raw + b"\0" * (len(name) - len(raw))
            if shim_name_rva is None:
                shim_name_rva = name_rva
            changed.append((name, dllname))
            continue

        # Too short to overwrite: reuse a string another descriptor already
        # points at, or fail with a clear message.
        if shim_name_rva is None:
            raise PEError(
                "cannot redirect %r: the shim name %r does not fit in the %d "
                "available bytes, and no other descriptor has been patched yet"
                % (name, dllname, len(name) + 1))
        struct.pack_into("<I", pe.data, desc_off + 12, shim_name_rva)
        changed.append((name, dllname + " (name RVA repointed)"))

    if not changed:
        raise PEError("nothing to patch: none of %s are imported" % ", ".join(sorted(targets)))
    return changed


def read_exports(dll_path):
    """Set of exported names from a PE DLL, using its export directory."""
    with open(dll_path, "rb") as fh:
        pe = PE(fh.read())
    if not pe.export_rva:
        return set()
    base = pe.rva_to_offset(pe.export_rva)
    (_, _, _, _, name_rva, ordinal_base, nfuncs, nnames,
     funcs_rva, names_rva, ords_rva) = struct.unpack_from("<IIHHIIIIIII", pe.data, base)
    names = set()
    for i in range(nnames):
        off = pe.rva_to_offset(names_rva + 4 * i)
        (nr,) = struct.unpack_from("<I", pe.data, off)
        name, _ = pe.cstring_at_rva(nr)
        names.add(name)
    return names


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", help="PE32+ executable to patch")
    ap.add_argument("--dlls", nargs="+", default=list(DEFAULT_DLLS),
                    help="import DLL names to redirect (default: %(default)s)")
    ap.add_argument("--dllname", default="w7shim.dll", help="shim DLL name (default: %(default)s)")
    ap.add_argument("--out", default=None, help="output path (default: <exe>.win7.exe)")
    ap.add_argument("--inplace", action="store_true", help="overwrite the input file")
    ap.add_argument("--inspect", action="store_true",
                    help="list import descriptors and exit without writing")
    ap.add_argument("--shim", default=None,
                    help="shim DLL to verify against (default: <dllname> next to the exe)")
    args = ap.parse_args()

    with open(args.exe, "rb") as fh:
        pe = PE(fh.read())

    if args.inspect:
        for name, thunks in pe.descriptor_detail():
            print("%s: %d imports" % (name, len(thunks)))
        return 0

    # Coverage check first: redirecting an import the shim does not export
    # would turn a clear error into an obscure one, so refuse up front.
    shim_path = args.shim or os.path.join(os.path.dirname(args.exe) or ".",
                                          args.dllname)
    targets = {d.lower() for d in args.dlls}
    needed = set()
    for name, thunks in pe.descriptor_detail():
        if name.lower() in targets:
            for trva in thunks:
                _, fn = pe._hint_name(trva)
                if fn:
                    needed.add(fn)

    if os.path.exists(shim_path):
        exported = read_exports(shim_path)
        missing = sorted(needed - exported)
        print("shim %s exports %d names; %d needed by %s"
              % (shim_path, len(exported), len(needed), args.exe))
        if missing:
            print("error: shim is missing %d export(s): %s"
                  % (len(missing), ", ".join(missing[:10])), file=sys.stderr)
            return 3
    else:
        print("warning: %s not found, skipping coverage check" % shim_path,
              file=sys.stderr)

    changed = patch(pe, args.dlls, args.dllname)

    if args.inplace:
        out = args.exe
    elif args.out:
        out = args.out
    else:
        base, ext = os.path.splitext(args.exe)
        out = base + ".win7" + ext

    with open(out, "wb") as fh:
        fh.write(pe.data)

    print("patched %d import descriptor(s):" % len(changed))
    for old, new in changed:
        print("  %s -> %s" % (old, new))
    print("wrote %s (%d bytes)" % (out, len(pe.data)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PEError as exc:
        print("error: %s" % exc, file=sys.stderr)
        sys.exit(2)
