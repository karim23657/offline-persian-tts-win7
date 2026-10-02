# Compiling for Windows 7

Everything in `runtime/` is built by `make` (Linux/macOS) or `build.cmd`
(Windows). One command, from a clean checkout, produces a working package:

```bash
make          # fetch + scan + shim + patch + front ends -> runtime/
make test     # build and run the shim self-test under Wine
make release  # package the archives into dist/
```

`runtime/` is **not** committed: it is 24 MB of upstream DLLs plus our own
builds. `make` recreates it exactly.

---

## Toolchain

| Tool | Version used | Why |
| --- | --- | --- |
| `x86_64-w64-mingw32-gcc` | 13.x (Ubuntu `g++-mingw-w64-x86-64`) | compiles the shim and all front ends |
| Python 3 | 3.8+ | the patch/scan tooling (runs on Win7 too) |
| `wine64` | 9.0 | **testing only** - see [win7.md](win7.md) |
| `zip` | any | packaging |

```bash
sudo apt-get install -y --no-install-recommends \
    g++-mingw-w64-x86-64 python3 zip
```

MinGW is a modern (Windows 10 SDK) toolchain, so **anything compiled with it
may import Windows 8+ APIs and need patching** - that is precisely why our own
executables are checked after every build.

---

## The pipeline, step by step

### 1. Fetch the pinned upstream release

`src/shim/sherpa-version.env` pins the version:

```
SHERPA_VERSION=1.13.8
SHERPA_WIN_ASSET=sherpa-onnx-v$(SHERPA_VERSION)-win-x64-shared-MD-Release.tar.bz2
```

```bash
python3 src/shim/fetch_sherpa.py     # -> build/sherpa/unpacked/
```

Kept as `-MD-Release` deliberately: the `Debug` and `MinSizeRel` variants import
extra Windows-only DLLs. This is the same release the users ultimately run, so
what CI tests is what ships.

### 2. Derive `imports.txt` from the actual binaries

```bash
python3 src/shim/scan_imports.py \
    build/sherpa/unpacked/bin/sherpa-onnx-offline-tts.exe \
    build/sherpa/unpacked/lib/sherpa-onnx-c-api.dll \
    build/sherpa/unpacked/lib/onnxruntime.dll \
    --out src/shim/imports.txt
```

This must **never** be hand-maintained. A new upstream release can add a single
Windows 8+ import; if `imports.txt` misses it, the binary loads on Windows 10
and dies on Windows 7 with a message that looks like a mystery. Two readers are
implemented and cross-checked: `objdump -x` when binutils is present, and a
pure-Python PE import-table parser otherwise. They must agree - a test in CI
would catch a divergence.

### 3. Generate the shim tables and compile it

`gen_shim.py` turns `imports.txt` into three files: a resolver table
(`win7shim_table.c`), one 6-byte trampoline per function
(`win7shim_thunks.S`), and a module definition (`win7shim.def`).

```bash
python3 src/shim/gen_shim.py src/shim/imports.txt --outdir build/shim

x86_64-w64-mingw32-gcc -shared -O2 -Wall -Isrc/shim \
    -o runtime/w7shim.dll \
    src/shim/win7shim.c src/shim/win7shim_fallback.c \
    build/shim/win7shim_table.c build/shim/win7shim_thunks.S build/shim/win7shim.def \
    -lkernel32 -static-libgcc
```

`-Isrc/shim` is required because the generated C includes `win7shim.h`.
`-static-libgcc` keeps the DLL free of a libgcc dependency; the shim must have
no dependencies beyond `KERNEL32.dll` and `msvcrt.dll`.

### 4. Redirect the imports of the upstream binaries

```bash
for f in sherpa-onnx-offline-tts.exe sherpa-onnx-c-api.dll onnxruntime.dll; do
    python3 src/shim/patch_pe.py "runtime/$f" --inplace --shim runtime/w7shim.dll
done
```

`patch_pe.py` refuses to patch anything the shim does not export, so a stale
`imports.txt` fails loudly here instead of producing a broken binary.

**Patch the copies in `runtime/`, never `build/sherpa/unpacked/`.** Patching
the downloaded originals means the next `scan_imports` no longer sees
`KERNEL32.dll` (it already says `w7shim.dll`) and silently writes an empty
import list. That mistake cost an hour here.

### 5. Build the front ends against the *patched* libraries

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -o runtime/say.exe src/say/say.c \
    -I build/sherpa/unpacked/include -Lruntime -lsherpa-onnx-c-api

x86_64-w64-mingw32-gcc -O2 -Wall -mwindows -municode -o runtime/tts_gui.exe src/gui/gui.c \
    -I build/sherpa/unpacked/include -Lruntime -lsherpa-onnx-c-api \
    -lcomctl32 -lwinmm -lshell32 -luuid -lole32

x86_64-w64-mingw32-gcc -O2 -Wall -o runtime/diagnose.exe src/diag/diag.c
```

Link against `runtime/`, not `build/sherpa/unpacked/lib/` - the runtime copies
are the patched ones. Linking the pristine DLLs produces a `say.exe` that needs
those same DLLs patched beside it afterwards.

Note the flags: `-mwindows` makes the GUI a windowed app (no console), while
`say.exe` and `diagnose.exe` are console apps. A GUI-subsystem binary launched
under Wine can hang on `pipe_read`, so console builds are what CI uses.

### 6. Test

```bash
make test
```

Builds a **superset** shim for the self-test, because the test imports a few
`KERNEL32` functions the shipped binaries do not, then patches and runs it.
`RESULT: ALL PASS` is the gate.

The subtlety, documented at length in [win7.md](win7.md): Wine exports the very
Windows 8+ APIs the shim replaces, so a normal run would forward everything and
execute **none** of the fallbacks. The test therefore resolves each fallback by
name through the `win7shim_get_fallback` hook, which is why that hook exists.

### 7. Package

```bash
make release        # -> dist/win7-tts.zip and dist/win7-tts-runtime-only.zip
```

Always extract to a clean directory and run a synthesis from the extract before
calling it done. A stale zip shipped twice during this build.

---

## Building on Windows instead

`build.cmd` runs the same steps natively and needs Python 3, MinGW-w64 and
7-Zip (or `bsdtar`). Note that `make release` is Linux-only; on Windows, zip
`runtime\`, `scripts\`, `models\` and the docs with Explorer or 7-Zip.
