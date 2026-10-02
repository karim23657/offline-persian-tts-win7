# Building `say.exe`

`say.c` links sherpa-onnx's C API, so it needs the headers from a
sherpa-onnx Windows release:

```bash
# 1. fetch the release (this is the same archive setup.cmd uses)
curl -LO https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/sherpa-onnx-v1.13.8-win-x64-shared-MD-Release.tar.bz2
tar xjf sherpa-onnx-v1.13.8-win-x64-shared-MD-Release.tar.bz2

# 2. drop the headers next to the source
cp -r sherpa-onnx-*/include .

# 3. build against the already-patched runtime DLLs in ../../runtime
x86_64-w64-mingw32-gcc -O2 -Wall -o say.exe say.c \
    -Iinclude -L../../runtime -lsherpa-onnx-c-api

# 4. install
cp say.exe ../../runtime/
```

Notes:

- Build against the **patched** DLLs in `../../runtime`. Linking the pristine
  upstream DLLs produces a `say.exe` that then needs those same DLLs patched
  alongside it.
- `say.exe` imports from `sherpa-onnx-c-api.dll` only; it does not go through
  `KERNEL32.dll` imports of its own that the shim needs to patch, so it is
  *not* patched itself.
- It takes UTF-8 text via `--text-file`, converts to UTF-16 internally, and
  hands UTF-8 to the C API. That is what makes Persian independent of the
  system code page.
