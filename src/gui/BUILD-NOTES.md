# Building `tts_gui.exe`

Native Win32 GUI. Needs MinGW-w64 and the sherpa-onnx headers (see
`../say/BUILD-NOTES.md` for where to get them).

```bash
# headers next to the source
cp -r /path/to/sherpa-onnx-win-x64-*/include .

x86_64-w64-mingw32-gcc -O2 -Wall -mwindows -municode -o tts_gui.exe gui.c \
    -Iinclude -L../../runtime -lsherpa-onnx-c-api \
    -lcomctl32 -lwinmm -lshell32 -luuid -lole32

cp tts_gui.exe ../../runtime/
```

Notes:

- Link against the **patched** DLLs in `../../runtime`; the resulting exe then
  needs those same DLLs beside it.
- `tts_gui.exe` does **not** need `w7shim.dll`. It is built here with MinGW and
  imports nothing newer than Windows 7 (`SetProcessDpiAware` is Vista), so it is
  never patched.
- Everything user-facing is wide (`W`) API; text becomes UTF-8 only when handed
  to the sherpa-onnx C API. That is what makes Persian work regardless of the
  system code page.
- Synthesis runs on a worker thread and posts its result back with
  `PostMessageW`, so the window never goes grey while a model loads.
