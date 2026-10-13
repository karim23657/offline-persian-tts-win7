# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

The most important line item for any release is **sherpa-onnx**, because it
determines which Windows APIs the shim has to provide. See
[docs/upgrading-sherpa.md](docs/upgrading-sherpa.md).

## [1.1.0] - 2026-10-13

### Added

- **Tabbed GUI** (`tts_gui.exe`): four tabs - text & speech, pronunciation
  dictionary, settings, model download. The window is still one small native
  window; each tab is a page of child controls shown or hidden on switch.
- **fa/en language switch** in the GUI. Every visible string goes through one
  table; Persian flips the window and text box to RTL reading order and uses
  the Sahel font (loaded from `sahel.ttf` at the package root via
  `AddFontResourceW`, no install needed), English uses Segoe UI and LTR.
- **Pronunciation dictionary** (`user.dict`): whole-word substitution applied
  before synthesis. Editable from the GUI tab, from `POST /api/dict`, and from
  `say.exe`; all three read and write the same package-root file, so an entry
  added in one place applies everywhere. This is the honest replacement for a
  "phoneme editor": the sherpa-onnx C API has no phoneme input (its `lexicon`
  field is ignored whenever `data_dir` is set - always the case for the piper
  models here).
- **Model download tab** in the GUI, reading the package-root `models.json`
  (the same file the web UI shows) and fetching with WinInet: name, size and
  description columns, progress, cancel, and `tar.exe` extraction for archives
  when `tar.exe` is available (it is not on stock Windows 7).
- **Model offload on switch**: selecting a different model in the GUI, or
  changing the model in the web UI, now evicts the previous one from the
  engine cache (`TtsEngineEvict` / `POST /api/unload`). Only the selected model
  stays resident, so two 63 MB sessions never sit in memory together by
  accident. Closing the web page sends a `pagehide` beacon that frees the model
  too.
- **HTTP/1.1 keep-alive** in `tts_server.exe`: one connection now serves a
  whole browser session (bounded at 200 requests or 15 s idle) instead of one
  TCP handshake per click. Requests that arrive pipelined in a single TCP
  segment are both read (the extra bytes are held over, not dropped).
- **Thread-per-connection server**: a sequential accept loop would have
  stalled every client behind one idle keep-alive socket; each connection now
  gets its own thread. Synthesis is still serialised inside the engine cache.
- New API routes: `GET /api/help` (this list, as plain text),
  `GET /api/catalog` (serves `models.json`, what the download tab and web UI
  show), `GET/POST /api/dict` (list, add/replace, delete dictionary entries),
  `POST /api/unload` (free resident models: `{}` = all, `{"keep":m}` = all but
  m, `{"model":m}` = just m - the beacon the web UI sends on pagehide).
- `say.exe` gains `--help`/`-h`, `--version`, `--list-models`, `--play` (play
  the result through the sound device), `--no-dict` (skip the dictionary), and
  auto-detection of the model (`models\` is scanned when `--model` is omitted)
  and of the output file (`out.wav`). The espeak-ng-data fallback now also
  looks next to the executable, not just beside the model.
- `scripts/protocol_test.py` - a transport-level test suite (23 checks):
  keep-alive reuse, pipelining, HTTP/1.0 and `Connection: close` handling,
  honest 411/413 answers, and the unload/catalog/help/dict endpoints including
  the beacon-style unload. Runs as part of `make test`.
- The web UI embeds the Sahel font as base64 (spliced in by `embed_ui.py` from
  `sahel.ttf`), so Persian renders correctly on a Windows 7 machine with no
  fonts installed and without an extra HTTP request.
- **Release packaging** (`scripts/make_dist.py`, `make release`): builds three
  version-stamped archives into `dist/` - `win7-tts-<ver>-runtime.zip` (program
  only), `win7-tts-<ver>-models.zip` (voices only, to drop onto an existing
  install), and `win7-tts-<ver>-full.zip` (program + models, offline out of the
  box) - plus `SHA256SUMS.txt` and the legacy `win7-tts-runtime-only.zip` name.
  The layout is curated (no `src/`, `.github/` or build litter) and always
  carries `models.json` and `sahel.ttf`, which the GUI download tab and the
  Persian UI need. `make_slim_zip.py` is now a thin wrapper so CI keeps working
  unchanged.

### Changed

- The web UI has been redesigned (dark theme, fa/en toggle in the header, an
  installed-models panel with a "loaded" badge, a download catalog with sizes
  and links, and an API reference panel). It still serves over one keep-alive
  connection and keeps the `pagehide` unload beacon.
- `models.json` now lives at the package root and is generated from the real
  upstream file sizes. The server exposes it via `/api/catalog`; the GUI reads
  it for the download tab; both share one source of truth.
- The server loads the model once and warms it, so the first request is
  1-2 s and later ones ~0.3 s. This was already true; the unload endpoint now
  makes the lifecycle explicit.

### Fixed

- **`tokens.txt` path was passed to the engine uninitialised** (`create_engine`
  built `model_onnx` and `data_dir` but never filled the `tokens` buffer), so
  the config carried whatever was on the stack. It only worked when the slot
  happened to be zeroed. Now the path is built explicitly.
- **`SO_RCVTIMEO` was set with a POSIX `struct timeval`** (16 bytes) instead of
  a Windows `DWORD` of milliseconds. Windows truncates to the first 4 bytes, so
  the "20 second" receive timeout was actually 20 *milliseconds* - invisible in
  the old one-read-per-connection design, fatal for keep-alive (any gap between
  two requests longer than a rounding error ended the connection).
- `TtsEngineAcquire` had a slot-reservation race: it released the table lock
  before loading, so two threads asking for different models at the same
  moment could pick the same free slot (and two threads asking for the same
  model could load it twice). A dedicated loader lock now serialises loading
  without blocking generation.
- `say.exe` lost the espeak-ng data directory when run from a folder other than
  the package root.

[1.1.0]: https://github.com/karim23657/offline-persian-tts-win7/releases/tag/v1.1.0

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
