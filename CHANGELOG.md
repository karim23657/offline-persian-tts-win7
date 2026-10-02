# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

The most important line item for any release is **sherpa-onnx**, because it
determines which Windows APIs the shim has to provide. See
[docs/upgrading-sherpa.md](docs/upgrading-sherpa.md).

## [1.0.0] - 2026-10-02

### Added

- `runtime/w7shim.dll` - redirects the Windows 8+ APIs the sherpa-onnx binaries
  statically import, with genuine Windows 7 implementations for
  `GetSystemTimePreciseAsFileTime`, `InitializeCriticalSectionEx`, `CreateFile2`,
  `GetProcessMitigationPolicy`, the `Fls*` family (rebuilt on `TlsAlloc`), the
  `CONDITION_VARIABLE` family (rebuilt on `SRWLOCK`), `PathCchRemove*` and
  `CreateDXGIFactory2` (forwarded to `CreateDXGIFactory1`).
- `src/shim/patch_pe.py` - PE import redirector, including the short-name case
  where a descriptor's name RVA is repointed rather than overwritten. Refuses to
  patch anything the shim does not export.
- `src/shim/scan_imports.py` - derives `imports.txt` from the real binaries, so
  an upstream release that adds a Windows 8+ import cannot slip through.
- `runtime/say.exe` - front end that passes text as UTF-8 through the
  sherpa-onnx C API instead of `argv`, so Persian works regardless of the
  system code page. Loads the model once, so batch mode is much faster.
- `runtime/tts_gui.exe` - native Win32 GUI with no external dependencies:
  model picker, right-to-left text box, speed slider, progress bar, playback.
- `scripts/gui.py` - the same GUI in Tkinter for people who prefer Python
  (needs Python 3.8, the last with Windows 7 support).
- `runtime/diagnose.exe` and `scripts/diagnose.cmd` - report CPU instruction
  set, runtime presence and a live exit code, turning "it crashes" into a
  specific exception code.
- `scripts/get_model.cmd` - download any of 13 Persian models.
- `scripts/say_batch.cmd` - one `.wav` per line, loading the model once.
- `Makefile` / `build.cmd` - reproducible build: fetch, scan, compile, patch,
  package. See [docs/compiling.md](docs/compiling.md).
- `.github/workflows/ci.yml` - builds, self-tests, synthesises audio and checks
  no un-redirected Windows 8+ imports remain.

### Fixed

- **FLS initialisation race in the shim.** One-time setup published "ready"
  before initialising the lock it protected, so onnxruntime allocating FLS slots
  from several threads during session creation could enter an uninitialised
  `CRITICAL_SECTION` - an access violation (`0xC0000005`) on Windows 7. Now uses
  `InitOnceExecuteOnce`. Invisible under Wine, which exports `FlsAlloc` itself;
  found by a user on real hardware.
- `say.exe` passed `argv` (ANSI) to the engine as UTF-8, producing
  "Non UTF8 encoded string is received" and garbage phonemes.
- `CreateDXGIFactory2` left unredirected because `dxgi.dll` is too short a name
  to overwrite in place, so `onnxruntime.dll` failed to load on Windows 7.

[1.0.0]: https://github.com/karim23657/win7-tts/releases/tag/v1.0.0
