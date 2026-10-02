# how_to.md — what was done here, and why

A hand-off for whoever works on this next. It records the decisions, the dead
ends, and the mistakes, because the mistakes are the part that is expensive to
rediscover.

Companion documents: [`README.md`](../README.md) (user-facing) and
[`win7.md`](win7.md) (how to build the Wine-as-Win7 test environment).

---

## 1. The task, restated

> "Create or find something llama.cpp-like that runs on Windows 7 to run piper
> models without any error. I want a local version to try models from
> <https://huggingface.co/spaces/karim23657/Persian-TTS-sherpa>."

Read carefully: "**without any error**" and "**on Windows 7**" were the
substance of the request, not incidental context. Most of the work below exists
solely because of that sentence.

---

## 2. Investigation before code

The first real decision was to measure the target before writing anything.

**Model set.** Fetched `app_utils.py` from the Space to get the exact model
list (14 models) and their sources. They split into two groups:

- `k2-fsa/sherpa-onnx` release tarballs (`gyro`, `amir`, `reza`, `haaniye`)
- raw files from `karim23657/persian-tts-vits` on HuggingFace (the rest)

**Runtime.** Chose **sherpa-onnx** over Piper/piper-tts: it is what the Space
uses, it has a Windows release with a C API, and its models are already
converted to ONNX. The "llama.cpp-like" analogy maps cleanly — one engine
binary, one ONNX file, no runtime to install.

**Why the shared build, not the standalone exe.** sherpa-onnx publishes
`sherpa-onnx-non-streaming-tts-x64-*.exe` (single file, self-contained) *and* a
`win-x64-shared` tarball. The standalone exe is a **GUI-subsystem** binary: it
hung on `pipe_read` under Wine and offers no C API. The shared build has
console binaries plus `sherpa-onnx-c-api.dll`. **Chose the shared build.** The
trade-off is that it needs the VC++ redistributable, which is not on stock
Windows 7 — documented prominently, but it was the right call.

**The actual blocker.** Disassembling the Windows binaries showed they
statically import functions Windows 7 does not have. Confirmed by inspecting
the import table (`objdump -x`), not by guessing:

```
GetSystemTimePreciseAsFileTime, CreateFile2, InitializeCriticalSectionEx,
GetProcessMitigationPolicy, FlsAlloc/Free/GetValue/SetValue,
InitializeConditionVariable/Wake*/SleepConditionVariableSRW,
PathCchRemoveBackslash/PathCchRemoveFileSpec,
CreateDXGIFactory2  (from dxgi.dll)
api-ms-win-core-path-l1-1-0.dll   (an API-set stub, Windows 8+)
```

There is no upstream Win7 build and no way to add exports to the real
`KERNEL32.dll`. That settled the architecture.

---

## 3. Architecture and why each piece exists

```
user input
   │
   ├─ tts_gui.exe  (native Win32)  ─┐
   ├─ gui.py      (Tkinter)       ─┼─► say.exe ──► sherpa-onnx-c-api.dll
   └─ say.cmd / say_batch.cmd     ─┘                     │
                                                          ▼
                                            sherpa-onnx-offline-tts.exe
                                                          │
                                    w7shim.dll ──► onnxruntime.dll ──► model.onnx
```

| Piece | Why it exists |
| --- | --- |
| `w7shim.dll` | supplies the Win8-only APIs the loader demands |
| `patch_pe.py` | repoints those imports at the shim |
| `say.exe` | passes text as **UTF-8 via the C API**, bypassing `argv` entirely |
| `tts_gui.exe` | GUI with **no dependencies at all** |
| `gui.py` | the same GUI in Python, for people who prefer to hack on it |
| `diagnose.exe` | turns "it crashes" into a specific exception code |
| `imports.txt` + `gen_shim.py` | regenerates the shim when upstream changes |

### The design decision that mattered most: never use `argv` for text

`sherpa-onnx-offline-tts.exe` has a plain `main()`, so Windows hands it
arguments in the **ANSI code page**. On a Persian Windows 7 that is 1256; on a
stock one, 1252 or 437. Neither can represent Persian, so the engine receives
`????` and emits 30 ms of near-silence.

`chcp 65001` is not a dependable fix — the code page must already be UTF-8
before the process starts, and anything launching the engine without a console
(a shortcut, Task Scheduler, another program) has no code page at all.

So `say.exe` links the sherpa-onnx C API directly and hands it UTF-8, which is
what that API documents. This also turned out **faster**, because the model is
loaded once and can synthesise many sentences per invocation.

### How the shim works

`patch_pe.py` rewrites import descriptors so `KERNEL32.dll` and
`api-ms-win-core-path-l1-1-0.dll` resolve to `w7shim.dll`. Each redirected
import is a 6-byte trampoline (`jmp qword ptr [slot]`) whose slot is filled at
`DLL_PROCESS_ATTACH` with:

1. the **real** function, if it exists on the running system (probing the
   canonical DLL, then `ntdll.dll` — this is how `EncodePointer` resolves via
   `RtlEncodePointer`);
2. otherwise a **genuine Win7 implementation** (`win7shim_fallback.c`);
3. otherwise `xor eax,eax; ret`, so the DLL *always* loads.

For `dxgi.dll` the name is shorter than `w7shim.dll`, so it cannot be
overwritten in place. Because the descriptor's `Name` is just an RVA to a
string, the patcher repoints it at the `w7shim.dll` string another descriptor
already wrote. Sharing one name string is legal.

**Before writing**, `patch_pe.py` verifies the shim exports every name being
redirected and refuses otherwise. It caught a real gap during development
(`DisableThreadLibraryCalls`).
---

## 4. Dead ends

Kept here so nobody repeats them.

**Renaming the shim to something 8 characters or shorter** so `"dxgi.dll"`
could be overwritten in place. Unnecessary once I realised the `Name` field is
an RVA; the shorter name would also have been less readable.

**Building sherpa-onnx from source targeting Win7.** A multi-hour CMake +
k2-fsa build. A two-file import redirect achieves the same thing in an hour and
survives upstream upgrades.

**Using `chcp 65001` to fix Persian.** Appears to work, silently corrupts text
when the launcher has no console. Superseded by the C API approach.

**Python `sherpa-onnx` on Win7.** Needs a matching wheel, reintroduces the
loader problem, and Windows 7 caps Python at 3.8. Rejected; the Tkinter GUI is
offered as an *optional* extra instead, never the default.

**Importing the sherpa-onnx Python module in `gui.py`.** Same objection, so
`gui.py` drives `say.exe` as a subprocess.

**Raw-byte scanning for AVX opcodes** (`0xC4`, `0x62`) in `onnxruntime.dll`.
Produced nonsense — those pairs are common in data. Only disassembly
(`objdump -d -M intel`) is trustworthy.

---

## 5. My own bugs, and what they taught me

An honest list. Every one of these shipped at least once.

### 5.1 The FLS initialisation race — shipped, caused the user's crash

`w7shim` rebuilds the Win8 `Fls*` family on `TlsAlloc`. The table was set up
like this:

```c
if (InterlockedCompareExchange(&g_fls_init, 1, 0) == 0) {   /* publish "ready" */
    InitializeCriticalSection(&g_fls_lock);                /* ...then work */
}
```

onnxruntime allocates FLS slots from several threads while building a session.
Thread A publishes "ready" then initialises the lock; thread B sees "ready",
skips init, and enters an **uninitialised `CRITICAL_SECTION`** → access
violation (`0xC0000005`) during model load. Fixed with
`InitOnceExecuteOnce` (Vista+, valid on Win7).

*Why testing missed it:* under Wine the host exports `FlsAlloc`, so the shim
forwards and its own implementation never executes. A 100% clean Wine run hid
a crash that only Windows 7 can produce. See `win7.md`.

*Proof it was real:* rebuilt the old code with `Sleep(50)` widening the window —
it hung every run. The fixed version exits 0. A 16-thread × 500-iteration
regression test now runs in `test_shim.c`.

### 5.2 `Say.exe` passing ANSI bytes as UTF-8

`say.exe` has a narrow `main()`, so `argv` is ANSI. I passed it straight to the
API, so non-ASCII `--text` was invalid UTF-8 — the engine warned
`Non UTF8 encoded string is received` and phonemised garbage. Fixed with
`MultiByteToWideChar(CP_ACP)` → `WideCharToMultiByte(CP_UTF8)`.

### 5.3 An infinite loop in UTF-8 → UTF-16

The lead byte of a multi-byte sequence was never advanced, so Persian text
looped forever writing `U+FFFD` past the buffer → access violation. Only showed
up with real Persian input.

### 5.4 Tkinter: `root.after()` from a worker thread

Not thread-safe in Tk; it aborted the interpreter. Replaced with a
`queue.Queue` drained by a UI-thread timer.

### 5.5 Tkinter: `communicate()` on a pipe can hang forever

Anything the engine spawns inherits the pipe's write end, so it never closes.
Now writes to a file and waits with an explicit timeout.

### 5.6 `CreateThread` return value cast to `int`

Truncated a 64-bit pointer. Harmless here, wrong in principle.

### 5.7 My own diagnostic lied twice

The CPU feature detector indexed CPUID registers wrongly — first EAX instead of
ECX/EDX, then reading leaf 1's EDX *after* leaf 7 had overwritten it. It
reported "no AVX" on a modern Xeon, and later "no SSE2" on the user's machine.
**I would have blamed the user's CPU for a bug in my tool.** If the tool that
decides the answer might itself be wrong, verify the tool before concluding.

### 5.8 Tests that could not fail

- A `--run` mode printed "CRASH: exception 0x00000001" for an ordinary non-zero
  exit code.
- A thread stress test counted "128-slot table exhausted" as failure, which
  3200 allocations into 128 slots guarantees.
- A `CreateDXGIFactory2` test passed `NULL` where implementations write through,
  faulting inside Wine.

A test that cannot fail is worse than no test, because it manufactures
confidence. Check that each new assertion can actually fail.

---

## 6. Two things the emulator got wrong in both directions

**False negatives** (looked fine, was broken): Wine supplies every Win8 API, so
shim fallbacks never run.

**False positives** (looked broken, was fine): Wine marshals `argv` from the
Linux locale, so Persian on a command line becomes `????`. That is not a Win7
bug. Wine's `RtlFls*` is also unreliable enough that rebuilding FLS on ntdll's
implementation failed — it now uses Win32 `TlsAlloc` instead.

---

## 7. What I would do differently

1. **Build the diagnostic tool first**, before any shim code. It would have
   ruled out the CPU hypothesis in one run instead of three messages, and it is
   what finally produced the `0xC0000005` that located the race.
2. **Suspect my own code first.** Both "the user's machine is odd" conclusions
   here were wrong; in each case the bug was in something I wrote.
3. **Force fallback paths in tests from the start.** The
   `win7shim_get_fallback` hook existed only after the FLS crash; it should have
   existed from the first commit.
4. **Prove a race test catches the old code** before shipping the fix.

---

## 8. Current state, honestly

**Verified** under Wine-as-Win7, and for the shim by forcing fallbacks directly:
shim self-test (`RESULT: ALL PASS`, including a 16-thread FLS stress test);
`--text` and `--text-file` in both ASCII and Persian; batch mode; both GUIs
rendering and producing audio; end-to-end packaging from a clean extract.

**Confirmed by the user on real Windows 7 SP1:** the FLS fix works
(`exit code: 0x00000000`, audio produced from the `haaniye` model).

**Not verified:**

- On genuine hardware, anything other than that one confirmation.
- The Tkinter GUI end-to-end. Its logic, layout and the exact command line it
  issues were each verified separately, but `subprocess` cannot execute the
  Windows `say.exe` on a Linux test machine. If it misbehaves, use `gui.cmd` or
  the command line.
- Windows 7 audio playback paths (`PlaySound`, `winsound`).

**Known prerequisites the user must satisfy:** VC++ 2015-2022 **x64**
redistributable (the engine imports `VCRUNTIME140`/`MSVCP140`, absent from
stock Win7), and a CPU with AVX (2011+). `diagnose.exe` checks both.

---

## 9. Maintenance tasks

### Upgrading sherpa-onnx

1. Download the new `win-x64-shared` release.
2. Regenerate `src/shim/imports.txt` — this is the step that must not be
   skipped, because a new version may import functions nobody has redirected:
   ```bash
   python - <<'PY'
   import subprocess, re, glob
   base = glob.glob('/path/to/new/release/')[0]
   REDIR = {'KERNEL32.dll', 'api-ms-win-core-path-l1-1-0.dll', 'dxgi.dll'}
   merged = set()
   for p in [base+'bin/sherpa-onnx-offline-tts.exe', base+'lib/onnxruntime.dll',
             base+'lib/sherpa-onnx-c-api.dll']:
       out = subprocess.run(['objdump','-x',p],capture_output=True,text=True).stdout
       cur = None
       for line in out.split('Import Tables')[-1].split('\n'):
           m = re.search(r'DLL Name: (\S+)', line)
           if m: cur = m.group(1); continue
           m = re.match(r'\t[0-9a-f]+\s+\S+\s+(.+)$', line)
           if m and cur in REDIR: merged.add(m.group(1).strip())
   open('imports.txt','w').write('KERNEL32.dll '+' '.join(sorted(merged))+'\n')
   PY
   ```
3. `python gen_shim.py imports.txt --outdir gen`, rebuild, run
   `test/test_shim.exe`, re-patch the runtime, rebuild `say.exe` /
   `tts_gui.exe` against the **patched** DLLs, re-zip.

`patch_pe.py`'s coverage check will refuse to patch anything the shim does not
export. That refusal is the signal to regenerate the shim — do not remove the
check to get past it.

### Adding a model

`get_model.cmd` covers 13 models. Any other Piper/VITS Persian model works if
the folder contains one `.onnx` plus `tokens.txt`. Add it as another key in
`:download`, pointing at `KIND=hf` (single files) or `KIND=gh` (a tarball).

### Regenerating the zips

```bash
cd /content && zip -r -q win7-tts.zip win7-tts \
    -x 'win7-tts/*/__pycache__/*' 'win7-tts/*.wav'
```

Then **always** extract to a clean directory and run synthesis from the extract
before telling the user it is ready. Twice a stale zip shipped that way.

---

## 10. Where things are

| Path | Contents |
| --- | --- |
| `runtime/` | everything that must stay together; keep `w7shim.dll` beside the binaries that import it |
| `models/` | downloaded models, one folder each |
| `scripts/` | user-facing `.cmd` front ends + `gui.py` |
| `src/shim/` | `w7shim.dll` sources, `patch_pe.py`, `gen_shim.py`, `imports.txt`, self-test |
| `src/say/` | `say.exe` source + build notes |
| `src/gui/` | `tts_gui.exe` source + build notes |
| `src/diag/` | `diagnose.exe` source |
| `docs/` | how_to, compiling, upgrading-sherpa, win7, github-upload |
| `Makefile`, `build.cmd` | the reproducible build |

`*.cmd` files are UTF-8 with CRLF and **must not** gain a UTF-8 BOM — `cmd`
mis-parses a batch file whose first bytes are the BOM (`n++@echo off`). Persian
text reaches them via `chcp 65001`; the files themselves are ASCII-only, which
is deliberate.

---

## 11. If you only remember three things

1. **Wine cannot test this shim's fallbacks** — it supplies the very APIs the
   shim replaces. Force them via the test hook, or a clean run means nothing.
2. **Guess wrong about the target and you will blame the user's machine.** Both
   times here ("your CPU is too old", and my own CPUID tool lying), the fault
   was in code I wrote.
3. **Ship a probe with the product.** `scripts\diagnose.cmd` turned an
   unreproducible crash report into a precise `0xC0000005` and a locatable bug.
   When you cannot reproduce the failure, instrument for the person who can.
