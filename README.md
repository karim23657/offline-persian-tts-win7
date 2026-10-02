# win7-tts — fast local Persian (and any language) TTS on Windows 7

A self-contained, offline text-to-speech package for **Windows 7**, built around
[sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx). It runs the same Piper/VITS
models that the HuggingFace Space
[`karim23657/Persian-TTS-sherpa`](https://huggingface.co/spaces/karim23657/Persian-TTS-sherpa)
serves, but entirely on your own machine — no Python, no internet at run time.

Think of it as the TTS equivalent of a llama.cpp single-file build: one engine
binary, one ONNX model, and a command that turns text into a `.wav`.

For anyone picking this up afterwards:

- [`docs/how_to.md`](docs/how_to.md) - how it works, what was tried and
  rejected, and the bugs to avoid.
- [`docs/compiling.md`](docs/compiling.md) - how everything is compiled for
  Windows 7.
- [`docs/upgrading-sherpa.md`](docs/upgrading-sherpa.md) - how to move to a new
  sherpa-onnx release.
- [`docs/win7.md`](docs/win7.md) - the Wine-as-Windows-7 test environment.
- [`docs/github-upload.md`](docs/github-upload.md) - publishing to GitHub.

---

## Why this exists

Two separate things break on Windows 7, and both had to be solved.

### 1. The official binaries will not even start

sherpa-onnx publishes Windows builds compiled with a Windows 10 SDK and a
VS2022 toolchain. They statically import functions that Windows 7 does not
have — `GetSystemTimePreciseAsFileTime`, `CreateFile2`,
`InitializeCriticalSectionEx`, `GetProcessMitigationPolicy`, the `Fls*` and
`CONDITION_VARIABLE` families, `PathCchRemove*` — and they import them from
`api-ms-win-core-path-l1-1-0.dll`, an API-set stub that does not exist before
Windows 8. `onnxruntime.dll` additionally imports `CreateDXGIFactory2` from
`dxgi.dll`, which is DXGI 1.2 and also Windows 8 only.

On Windows 7 the loader aborts the process before a single line of code runs:

```
The procedure entry point GetSystemTimePreciseAsFileTime could not be located
in the dynamic library KERNEL32.dll.
```

There is no way to add an export to the real `KERNEL32.dll`, so the fix shipped
here is `runtime/w7shim.dll` plus a small PE patch (see **How the Win7 fix
works** below).

### 2. Persian text arrives as `????`

`sherpa-onnx-offline-tts.exe` is a plain `main()` program, so Windows hands it
its arguments in the current **ANSI code page**. On a stock Windows 7 that page
is 1252 or 437 — neither can represent Persian — so the engine receives a row
of `?` and emits 30 ms of near-silence instead of speech.

`chcp 65001` is not a dependable fix: the code page must already be UTF-8 before
the process starts, and anything that launches the engine without a console (a
shortcut, Task Scheduler, another program) has no code page at all.

So `runtime/say.exe` does not pass text through `argv` at all. It links
sherpa-onnx's C API directly and hands the engine **UTF-8**, which is what that
API documents. Persian works regardless of the system code page, and because the
model is loaded once it is also markedly faster than launching the engine per
sentence.
---

## Quick start

Unzip anywhere, then either start the graphical interface:

```bat
scripts\gui.cmd
```

...or use the command line:

```bat
scripts\say.cmd --file hello.txt
```

`hello.txt` is a plain **UTF-8** file containing one Persian sentence:

```
سلام دنیا! این یک آزمایش روی ویندوز سی و دو است.
```

That writes `out.wav` (22.05 kHz, mono, 16-bit) next to the folder.

Or from a normal command prompt, ASCII only:

```bat
scripts\say.cmd "hello world"
```

---

## Commands

| Command | What it does |
| --- | --- |
| `scripts\gui.cmd` | Start the graphical interface (no install needed). |
| `scripts\say.cmd "text"` | Speak text to `out.wav`. |
| `scripts\say.cmd "text" mine.wav` | Speak text to `mine.wav`. |
| `scripts\say.cmd --file text.txt` | Speak a UTF-8 file to `out.wav`. **Use this for Persian.** |
| `scripts\say.cmd --file text.txt mine.wav` | Speak a UTF-8 file to `mine.wav`. |
| `scripts\say_batch.cmd lines.txt` | One `.wav` per line of a UTF-8 file, in a `lines\` folder. |
| `scripts\get_model.cmd` | Interactive menu to download other models. |
| `scripts\get_model.cmd list` | Show every model and its source URL. |
| `scripts\diagnose.cmd` | Crash/CPU diagnostic. **Run this first if anything fails.** |

`say.cmd` and `say_batch.cmd` accept `--speed`, `--sid`, `--threads` and
`--scale` too; pass them straight through, e.g.:

```bat
scripts\say.cmd --file text.txt out.wav --speed 1.2
```

If `say.cmd` cannot find a model it tells you to run `scripts\get_model.cmd gyro`.

### Two messages that look alarming but are not

```
Failed to create DXGI factory.
```
ONNX Runtime probes for a GPU at start-up. There is none, and none is wanted -
everything runs on the CPU. The message comes from onnxruntime itself and
cannot be silenced.

```
Skip unknown phonemes. Unicode codepoint: \U+0259.
```
The `haaniye` (Mimic3) model has a small phoneme vocabulary and simply does not
contain every sound Persian can produce. Those sounds are skipped and the rest
is spoken normally. You will see it most often with that model; `gyro` is much
cleaner.

```
Non UTF8 encoded string is received.
```
Should not appear any more. If you still see it, pass the text with `--file`
rather than `--text` and tell me, because it means the text reached the engine
in the wrong encoding.

---

## Driving the engine directly

`runtime\sherpa-onnx-offline-tts.exe` is the unmodified sherpa-onnx CLI (with
its Win7 import problem patched). It is ASCII-only, because of the code page
issue above — use `say.exe` for anything else:

```bat
runtime\sherpa-onnx-offline-tts.exe ^
  --vits-model=models\vits-piper-fa_IR-gyro-medium\fa_IR-gyro-medium.onnx ^
  --vits-tokens=models\vits-piper-fa_IR-gyro-medium\tokens.txt ^
  --vits-data-dir=models\vits-piper-fa_IR-gyro-medium\espeak-ng-data ^
  --output-filename=out.wav "hello"
```

---

## Models

The default is **`gyro`** (`fa_IR-gyro-medium`), a Persian Piper model at
22.05 kHz. It is the best-sounding of the set and is already included.

`scripts\get_model.cmd` can fetch the others. All of them come from the same
sources as the HuggingFace Space:

| Name | Model | Notes |
| --- | --- | --- |
| `gyro` | `fa_IR-gyro-medium` | Piper, 22 kHz. **Default, included.** |
| `amir` | `fa_IR-amir-medium` | Piper, 22 kHz. |
| `reza` | `fa_en` reza/ibrahim | Piper medium, Persian + English. |
| `haaniye` | `mimic3-fa-haaniye_low` | Mimic3, low quality. |
| `ganji` | `vits-piper-fa-ganji` | Small, 16 kHz. |
| `ganji-adabi` | `vits-piper-fa-ganji-adabi` | Literary Persian. |
| `negoo` | Kamtera female VITS | |
| `arash` | Kamtera male1 VITS | |
| `keyan` | Kamtera male VITS | |
| `matab` | Kamtera female1 VITS | |
| `shiva` | female GPTInformal VITS | Conversational Persian. |
| `bahman` | SmartGitiCorp male VITS | |
| `mms` | MMS multilingual `fas` | No espeak data needed. |

After downloading a different model, point `say.cmd` at it by editing the
`MODEL_DIR` line near the top of the script:

```bat
set "MODEL_DIR=%CD%\models\vits-piper-fa_IR-amir-medium"
```

The engine expects the single `.onnx` file plus `tokens.txt` in that folder, so
any other Piper/VITS Persian model will work too.

---

## Folder layout

```
win7-tts\
  README.md                      this file
  CHANGELOG.md
  Makefile                       build / test / package
  build.cmd                      the same, on a Windows machine
  docs\                          how_to, compiling, upgrading, win7, github
  src\shim\                       w7shim.dll + patch_pe.py + self-test
  src\say\, src\gui\, src\diag\   sources for say.exe, tts_gui.exe, diagnose.exe
  build\, dist\                  intermediates and release archives (git-ignored)
  runtime\          the engine - keep these together
    sherpa-onnx-offline-tts.exe   stock sherpa-onnx CLI, Win7 imports patched
    sherpa-onnx-c-api.dll          C API used by say.exe
    sherpa-onnx-cxx-api.dll
    onnxruntime.dll                inference engine
    onnxruntime_providers_shared.dll
    say.exe                        Unicode-safe front end
    diagnose.exe                   CPU + crash diagnostic used by diagnose.cmd
    w7shim.dll                     the Windows 7 compatibility shim
  models\
    vits-piper-fa_IR-gyro-medium\  the default model
  scripts\
    gui.cmd                        launch the GUI
    gui.py                         the same GUI in Tkinter (needs Python 3.8)
    say.cmd                        speak text / a UTF-8 file
    say_batch.cmd                  one wav per line
    get_model.cmd                  download other models
  tools\
    shim\                          source of w7shim.dll + the PE patcher
    say\                           source of say.exe
    gui\                           source of tts_gui.exe
```

---

## The graphical interface

`scripts\gui.cmd` starts `runtime\tts_gui.exe`: pick a model, type text, move
the speed slider, press **Generate**, and the speech is written to `out.wav`
and played back. **Open folder** reveals the file, **Save as...** picks a
different one, and **Stop** cuts off playback.

Everything the user types is handled with the wide-character API and converted
to UTF-8 only at the boundary, so Persian appears correctly in the text box and
reaches the engine intact. Synthesis runs on a worker thread, so the window
stays responsive and shows a progress bar rather than going grey.

`tts_gui.exe` is native Win32 and needs **nothing** installed - no Python, no
runtime redistributable beyond the one the engine already wants. That matters
on Windows 7, which can only run Python up to 3.8.

### If you prefer Tkinter

`scripts\gui.py` is the same interface in Python, easier to hack on. It needs
**Python 3.8.10** - the last release with Windows 7 support - installed with
the *tcl/tk and IDLE* option, otherwise tkinter is missing:

```bat
python scripts\gui.py
```

It drives `runtime\say.exe` as a subprocess, which means the model is reloaded
for every utterance. For long lists prefer `scripts\say_batch.cmd`, which loads
the model once.

`runtime\` must stay in one folder: `w7shim.dll` has to sit next to the binaries
that import from it.

---

## How the Win7 fix works

`runtime/w7shim.dll` is a ~120 KB DLL that redirects the imports the engine
could not otherwise satisfy.

1. **`patch_pe.py`** walks the PE import directory and repoints the DLL name of
   the `KERNEL32.dll`, `api-ms-win-core-path-l1-1-0.dll` and `dxgi.dll`
   descriptors at `w7shim.dll`. Nothing else moves, so the file layout and
   relocation tables stay valid. Before writing, it checks that the shim really
   does export every name being redirected, and refuses if not.

   The two cases are handled differently. `KERNEL32.dll` and
   `api-ms-win-core-path-l1-1-0.dll` are long enough that the name is simply
   overwritten in place with `w7shim.dll` and NUL padding. `dxgi.dll` is
   *shorter* than `w7shim.dll` and cannot be overwritten that way — instead the
   descriptor's name RVA (which is just a pointer to a string) is repointed at
   the `w7shim.dll` string an earlier descriptor already wrote. Several
   descriptors sharing one name string is perfectly legal.

2. **`w7shim.dll`** exports one function per redirected import. Each is a
   6-byte trampoline (`jmp qword ptr [slot]`) and, at `DLL_PROCESS_ATTACH`,
   every slot is filled with one of:
   - the **real** function, if it exists on the running system — looked up with
     `GetProcAddress` across the canonical DLL, `ntdll.dll`, and `kernel32.dll`,
     which is how `EncodePointer` and friends resolve (`RtlEncodePointer`);
   - a genuine **Windows 7 implementation**, for the functions that simply do
     not exist there:
     - `GetSystemTimePreciseAsFileTime` → `GetSystemTimeAsFileTime`
     - `InitializeCriticalSectionEx` → `InitializeCriticalSectionAndSpinCount`
     - `CreateFile2` → `CreateFileW`
     - `GetProcessMitigationPolicy` → reports "no mitigations"
     - `FlsAlloc`/`Free`/`GetValue`/`SetValue` → rebuilt on Win32 `TlsAlloc` and
       friends, which Windows 7 does have
     - `InitializeConditionVariable`/`Wake`/`SleepConditionVariableSRW` →
       rebuilt on `SRWLOCK` plus one event per waiter
     - `PathCchRemoveBackslash`/`PathCchRemoveFileSpec` → string handling;
       Windows 7's `shcore.dll` only has the older `Path*` API
     - `CreateDXGIFactory2` → forwarded to `CreateDXGIFactory1`, which Windows 7
       does have and which has an identical signature
   - or, in the worst case, a `xor eax,eax; ret` stub — so the DLL **always**
     loads rather than failing at process start.

### Rebuilding the shim

Needs MinGW-w64 (`x86_64-w64-mingw32-gcc`) and Python 3:

```bat
cd tools\shim
python gen_shim.py imports.txt --outdir gen
x86_64-w64-mingw32-gcc -shared -O2 -o w7shim.dll ^
    win7shim.c win7shim_fallback.c ^
    gen\win7shim_table.c gen\win7shim_thunks.S gen\win7shim.def ^
    -lkernel32 -static-libgcc
```

`imports.txt` lists the functions to redirect, one line per DLL. Regenerate it
from a fresh sherpa-onnx download whenever you update the runtime, e.g. with
`objdump -x` on each `.exe`/`.dll` and collecting the `KERNEL32.dll` and
`api-ms-win-core-path-l1-1-0.dll` entries.

### Verifying the shim

`tools\shim\test\` contains a self-test that calls every Windows 7 fallback
directly and checks it behaves correctly (timing advances, FLS round-trips, a
condition-variable waiter is actually released and also honours its timeout,
`PathCchRemoveFileSpec` strips the right component, and so on):

```bat
cd tools\shim
x86_64-w64-mingw32-gcc -O2 -I. -o test\test_shim.exe test\test_shim.c
copy w7shim.dll test\
python patch_pe.py test\test_shim.exe --inplace --shim test\w7shim.dll
test\test_shim.exe
```

Expected output ends with `RESULT: ALL PASS`. `CreateFile2` reports `[SKIP]`
when the host already exports its own non-Windows-8 variant, which is expected
under Wine and does not happen on real Windows 7.

---

## What was verified, and what was not

Verified by actually running it under Wine configured as **Windows 7**
(`HKCU\Software\Wine\Version = win7`):

- the shim loads and resolves every redirected import;
- all shim fallbacks pass the self-test (`RESULT: ALL PASS`), including
  `CreateDXGIFactory2`, which is resolved by name and called directly so that
  Wine's own working export cannot mask a broken fallback;
- `sherpa-onnx-offline-tts.exe` starts and prints its usage;
- `say.exe` / `say.cmd` load the gyro model and synthesise **real Persian
  audio** — checked as non-silent 16-bit PCM at 22.05 kHz, e.g. 3.26 s for one
  sentence;
- `say_batch.cmd` writes one correctly-sized `.wav` per input line;
- `--speed` changes the output duration as expected;
- `tts_gui.exe` launches, detects the installed model, renders its controls,
  and - driven through the real window - typed Persian text and produced a
  2.1 s `.wav`, ending with the progress bar full and the status reading
  "Playing...";
- `scripts\gui.py` imports, discovers models, renders the same layout with
  right-justified (RTL) text, and the exact command line it runs was confirmed
  to produce 2.2 s of Persian audio.

Three genuine bugs were found and fixed by that testing, all worth knowing
about:

- **The FLS initialisation race**, which caused the crash reported on Windows 7:
  `w7shim` rebuilt the Win8 `FlsAlloc` family on top of `TlsAlloc`, but its
  one-time setup published "ready" *before* initialising the lock it protects.
  onnxruntime allocates FLS slots from several threads while building a
  session, so a second thread could enter a `CRITICAL_SECTION` that was not yet
  initialised - an access violation (`0xC0000005`) during model load. It now
  uses `InitOnceExecuteOnce`, which cannot be entered concurrently. The
  self-test hammers this from 16 threads to keep it fixed. Reproducing the old
  code with a widened race window made it hang immediately, versus a clean exit
  with the fix.

  Note this is invisible under Wine, where the host *does* export `FlsAlloc`
  and the shim forwards instead of using its own implementation - which is
  exactly why a crash could appear on real Windows 7 only.

- `gui.py` originally called `root.after()` from the worker thread. That is not
  thread-safe in Tk and aborts the interpreter. It now uses a `queue.Queue`
  drained by a UI-thread timer.
- `gui.py` captured the engine's output through a pipe and `communicate()`,
  which can block forever because anything the engine spawns inherits the write
  end. It now writes to a file and waits with an explicit timeout.

Confirmed on real Windows 7 hardware by the user:

- the FLS initialisation race above was the cause of the reported crash, and the
  fix is confirmed: `scripts\diagnose.cmd` now reports
  `exit code: 0x00000000` / `output wav: created` with real audio from the
  `haaniye` model.

Not verified:

- The Tkinter front end was **not** driven all the way through to a generated
  `.wav` in this environment: `subprocess` cannot execute the Windows `say.exe`
  on the Linux test machine, and running it through Wine inside a Tk process
  misbehaves (the engine process gets stuck). Its logic, layout and the exact
  command line it issues were each verified separately, and both bugs above
  came out of that work - but the last hop, `Popen` on real Windows, is
  unexercised. If the Tk version misbehaves, use `scripts\gui.cmd`, which is
  fully verified, or the command line.

Not verified, because it needs real hardware:

- a genuine Windows 7 SP1 machine. Wine reimplements the loader faithfully, but
  it is not the kernel.
- the **Visual C++ 2015-2022 redistributable**. The engine imports
  `VCRUNTIME140`, `MSVCP140` and `api-ms-win-crt-*`. Install the x64
  redistributable first, or copy those DLLs from a machine that has it. This is
  the most likely remaining source of trouble on a real Win7 box.
- the `CreateDXGIFactory2` fallback could not be exercised against a real
  Windows 7 `dxgi.dll`, since Wine supplies its own. It was verified directly
  instead: the self-test resolves the Windows 7 implementation by name and
  calls it, and it returns a well-formed HRESULT rather than failing to link.

---

## If it crashes: run the diagnostic first

Before anything else, run:

```bat
scripts\diagnose.cmd
```

It prints your CPU's instruction-set support, checks that the VC++ runtime and
all `runtime\` files are present, tries to load onnxruntime, and then runs a
real synthesis — showing the Windows exception code if anything dies. **Paste
the whole output when reporting a problem**; the exception code identifies the
cause immediately.

| Exception | Meaning |
| --- | --- |
| `0xC000001D` | Illegal instruction — the CPU is too old (needs AVX). |
| `0xC0000005` | Access violation — `runtime\` is incomplete or the DLLs mismatch. |
| `0xC000007B` | Bad DLL image — usually a 32-bit DLL on 64-bit Windows. |
| `0xC0000135` | A DLL was not found — normally the Visual C++ runtime. |
| `0xC0000139` | Entry point not found — `w7shim.dll` missing or misplaced. |
| `0xC0000409` | Stack overrun — retry with `--threads 1`. |

`say.exe` also installs a crash handler, so instead of the bare "say.exe has
stopped working" dialog it now prints the exception code and a plain-English
explanation of what to do.

---

## CPU too old

The engine is ONNX Runtime 1.28, whose x64 CPU kernels **require AVX**. Any CPU
without AVX — in practice anything made before 2011 — raises an illegal
instruction fault and the process dies. Windows 7 machines are old enough that
this is a realistic possibility, and it cannot be worked around from the
outside, because the AVX instructions live inside `onnxruntime.dll`.

`scripts\diagnose.cmd` reports `AVX : NO` if that is your situation. Options,
best first:

1. **Use a different machine.** Anything with a Core i5/i7 from 2011 onwards,
   or any AMD CPU from Bulldozer (2011) onwards, is fine.
2. **Use an older engine.** sherpa-onnx releases from around v1.10 bundle an
   older ONNX Runtime that still has non-AVX code paths. Download such a release
   from the [releases page](https://github.com/k2-fsa/sherpa-onnx/releases),
   replace `runtime\onnxruntime.dll` with the one from that archive, then
   re-apply the patch (below). It will be slower but should run.
3. **Use Piper instead.** The original [`piper`](https://github.com/rhasspy/piper)
   releases have a CPU-only ONNX build that predates the AVX requirement, at the
   cost of no GUI from this package.

---

## Troubleshooting

**`The procedure entry point ... could not be located`**
`w7shim.dll` is missing or not beside the binary that needs it. Keep the whole
`runtime\` folder together.

**`The code execution cannot proceed because VCRUNTIME140.dll was not found`**
Install the Visual C++ 2015-2022 **x64** redistributable.

**`The procedure entry point CreateDXGIFactory2 could not be located ... dxgi.dll`**
You are running an older build from before dxgi redirection was added. Re-extract
the current archive, or re-patch the runtime:
`python tools\shim\patch_pe.py runtime\onnxruntime.dll --inplace --shim runtime\w7shim.dll`

**Audio is a tiny click, or the text prints as `????`**
Persian was passed straight to `sherpa-onnx-offline-tts.exe`. Use
`scripts\say.cmd --file text.txt` with a UTF-8 file.

**`could not create the TTS engine`**
Model path problem. Check `MODEL_DIR` in `say.cmd`, and confirm the model folder
contains both the `.onnx` file and `tokens.txt`.

**Very slow synthesis**
VITS is CPU-bound. Lower `--threads` on a small machine, or switch to the smaller
`ganji` (16 kHz) model.

---

## Credits and licences

- **sherpa-onnx** (k2-fsa) — Apache-2.0. The engine, the C API, and the import
  redirect technique are all built on its release binaries.
- **ONNX Runtime** (Microsoft) — MIT.
- **espeak-ng** phonemiser data — GPL-3.0 with a linking exception, shipped
  inside the model folders.
- **Piper / VITS** models — each carries its own licence; see the repositories
  linked by `scripts\get_model.cmd list`.
- The Persian model set mirrors the HuggingFace Space
  [`karim23657/Persian-TTS-sherpa`](https://huggingface.co/spaces/karim23657/Persian-TTS-sherpa)
  and the `karim23657/persian-tts-vits` repository.
