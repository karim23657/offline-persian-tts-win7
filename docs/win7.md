# Testing Windows 7 binaries on Linux (Wine as a Win7 emulator)

Everything here was worked out the hard way while building the `win7-tts`
package. It exists because a Windows-only CI box is usually unavailable, and
because *some* classes of bug are invisible unless you deliberately make the
emulator behave like Windows 7.

Read [the caveat section](#the-one-thing-that-will-bite-you) before trusting a
green test run.

---

## 1. Install the tools

Ubuntu/Debian:

```bash
apt-get update
apt-get install -y --no-install-recommends \
    g++-mingw-w64-x86-64 \
    wine64 xvfb x11-utils x11-apps openbox imagemagick xdotool zip
```

| Package | Why |
| --- | --- |
| `g++-mingw-w64-x86-64` | cross-compiles the shim, `say.exe`, the GUI |
| `wine64` | runs the Windows binaries |
| `xvfb` + `openbox` | GUI testing — see [section 5](#5-testing-guis) |
| `x11-utils`, `x11-apps`, `imagemagick` | `xwininfo`, `xwd`, `import` for screenshots |
| `xdotool` | typing into / clicking the GUI under test |

---

## 2. Create a Windows 7 prefix

```bash
export WINEPREFIX=/tmp/wp7
export WINEDEBUG=-all          # silence the ALSA/sdl noise; see gotchas
wineboot -i
```

`wine64` on Ubuntu installs **only** `/usr/lib/wine/wine64`, with no `wine`
wrapper on `PATH`. Create one:

```bash
printf '#!/bin/sh\nexec /usr/lib/wine/wine64 "$@"\n' > /usr/local/bin/wine
chmod +x /usr/local/bin/wine
```

Now tell Wine it is Windows 7. Both of these matter — the registry key alone
leaves the reported version inconsistent:

```bash
wine reg add 'HKCU\Software\Wine' /v Version /d win7 /f
wine reg add 'HKLM\Software\Microsoft\Windows NT\CurrentVersion' \
    /v CurrentVersion /d '6.1' /f
```

Verify — this must print **6.1.7601**:

```bash
wine cmd /c ver
# Microsoft Windows 6.1.7601
```

> Always `export WINEPREFIX` and `WINEDEBUG=-all` in the *same shell* as the
> command. Forgetting them silently gives you a different prefix, or a wall of
> unrelated output.

---

## 3. Run something

```bash
wine ./sherpa-onnx-offline-tts.exe --help
```

### Gotchas that will waste your time

- **`WINEDEBUG=-all` is not optional in scripts.** ALSA errors flood stderr and
  bury real messages. `2>/dev/null` also hides the program's own diagnostics —
  prefer `WINEDEBUG=-all` plus `grep`.
- **NUL bytes.** Wine writes `\0` into captured output. Use
  `tr -d '\000' < file.log` before `grep`, or pass `grep -a`.
- **Console apps only.** A *GUI-subsystem* `.exe` (subsystem 2) launched under
  Wine can block forever on `pipe_read`. Check with
  `objdump -p foo.exe | grep Subsystem`. Use a console build for CI; a GUI
  binary must go through the Xvfb path in section 5.
- **Toolchain timeouts.** Shell calls here cap at ~30 s. Run anything longer
  with `nohup ... &` and poll the log.
---

## 4. Inspecting Windows binaries from Linux

```bash
objdump -p app.exe | grep -oE 'DLL Name: .*'          # what it imports
objdump -p app.exe | grep -A400000 'Import Tables'   # full import detail
objdump -x  app.exe                                   # everything
objdump -d -M intel onnxruntime.dll \
  | grep -oE '\b(v[a-z0-9]+)\b' | sort | uniq -c | sort -rn
```

That last command is how you find out what a CPU actually needs. For
`onnxruntime.dll` it showed `vfmadd231ps` ×7636 and `vpdpbusd` ×1074 — FMA3 and
AVX512-VNNI — which is what suggested checking AVX on the target CPU.

Do **not** infer instruction-set requirements by grepping raw bytes for `0xC4`
or `0x62` prefixes. Those byte pairs are common in data sections; only
disassembly is trustworthy.

---

## 5. Testing GUIs

A GUI test needs a display *and* a window manager. Without a WM, windows are
created but never mapped and `import` returns a blank image — which looks
exactly like a rendering bug.

```bash
Xvfb :77 -screen 0 1000x700x24 &
DISPLAY=:77 openbox &
export DISPLAY=:77

wine ./tts_gui.exe &

# find the window
xwininfo -root -tree | grep -i 'your window title'
# 0xc00001 "win7-tts ...": ("tts_gui.exe" "tts_gui.exe")  772x526

# screenshot it
import -window 0xc00001 /tmp/shot.png
```

Drive it:

```bash
xdotool windowactivate --sync 0xc00001
xdotool type --window 0xc00001 --delay 40 'some text'
xdotool mousemove --window 0xc00001 60 447 mousedown 1 mouseup 1
```

Notes:

- `xvfb-run` creates its own `XAUTHORITY`; screenshots from another shell need
  `XAUTHORITY=/tmp/xvfb-run.XXXX/Xauthority`. Starting `Xvfb` by hand avoids
  this entirely.
- Mouse coordinates are **window-relative**. A click that does nothing usually
  means the button is not where you think — verify with a screenshot first.
- `xdotool type` needs fonts. On a bare container Persian shows as tofu boxes;
  that is a *font* problem, not an encoding problem. Prove text handling with a
  round trip (`say.exe --text-file`), not by reading a screenshot.
- **The screenshot is the evidence.** Attach it when reporting GUI work.

---

## The one thing that will bite you

**Wine exports the Windows 8+ APIs that this whole shim exists to replace.**

`wine` reports a Win7 version, but its `kernel32.dll` still exports
`GetSystemTimePreciseAsFileTime`, `CreateFile2`, `FlsAlloc`, the
`CONDITION_VARIABLE` family, and `dxgi.dll` exports `CreateDXGIFactory2`.

A shim that resolves a name with `GetProcAddress` and forwards on success will,
under Wine, **always forward** — so its Windows 7 fallback code never executes,
not even once. A pure-Wine test run of `win7-tts` passed 100% clean while a real
access-violation race sat inside the FLS implementation, reachable only on
Windows 7. It was found by the user on real hardware, not by me.

Consequences:

1. **Never claim a Win7-only path is "tested" because it ran under Wine.**
2. Force the fallback directly. `w7shim.dll` exports a test hook for this:
   ```c
   typedef FARPROC (WINAPI *PFN_GET_FALLBACK)(const char *name);
   PFN_GET_FALLBACK gf = (PFN_GET_FALLBACK)GetProcAddress(
       GetModuleHandleA("w7shim.dll"), "win7shim_get_fallback");
   FARPROC impl = gf("FlsAlloc");   /* the real Win7 implementation */
   ```
   `src/shim/test/test_shim.c` does this for every fallback.
3. Emulators also produce *false* failures:
   - Wine's `RtlFls*` returns index 0 and fails on the next call — do not
     build on it.
   - Wine's `CreateDXGIFactory1` faults on an unknown IID — test argument
     validation instead of calling through with a bogus GUID.
   - Wine marshals `argv` from the Linux locale, so Persian on a command line
     becomes `????`. That is a Wine artefact, **not** a Win7 bug — test text
     through a UTF-8 file.

### Make races reproducible

A real race may not fire in a short test. Widen the window deliberately to prove
both the bug and the fix:

```c
/* buggy: "ready" published before the lock exists */
if (InterlockedCompareExchange(&ready, 1, 0) == 0) {
    Sleep(50);                    /* make the window observable */
    InitializeCriticalSection(&lock);
}
/* fix: InitOnceExecuteOnce - cannot be entered concurrently */
```

With the `Sleep(50)` the old code hung every run; with `InitOnceExecuteOnce` it
exited 0. Without the sleep neither version failed, and the bug would have
"passed" CI forever. **If you fix a race, prove the test catches the old code.**

---

## 6. Cross-compiling

```bash
x86_64-w64-mingw32-gcc -shared -O2 -Wall -o w7shim.dll \
    win7shim.c win7shim_fallback.c \
    gen/win7shim_table.c gen/win7shim_thunks.S gen/win7shim.def \
    -lkernel32 -static-libgcc

# console app linking the sherpa-onnx C API
x86_64-w64-mingw32-gcc -O2 -Wall -o say.exe say.c -Iinclude \
    -L../../runtime -lsherpa-onnx-c-api

# GUI app
x86_64-w64-mingw32-gcc -O2 -Wall -mwindows -municode -o tts_gui.exe gui.c \
    -Iinclude -L../../runtime -lsherpa-onnx-c-api \
    -lcomctl32 -lwinmm -lshell32 -luuid -lole32
```

MinGW is a modern (Win10-era) toolchain, so **anything compiled with it may
import Windows 8+ APIs** and need patching. It also has wrong prototypes in
places — `CreateFile2` is declared with 5 parameters when the real API takes 8.
When a declaration looks wrong, check the real ABI and call through a
`GetProcAddress`-resolved prototype.

---

## 7. PE patching notes

Reading the import table by hand (see `src/shim/patch_pe.py`):

- `e_lfanew` at 0x3C → PE signature → COFF header → optional header.
- PE32+ (`magic 0x20b`): `NumberOfRvaAndSizes` at optional-header offset
  **108**, data directories from **112**. Index 0 = exports, 1 = imports.
- Map RVA → file offset through the section table (`vaddr`/`rawptr`); they are
  not the same thing.
- `IMAGE_IMPORT_DESCRIPTOR` is 20 bytes; `Name` is at offset **+12** and is an
  **RVA to a string**, so it can be repointed anywhere valid.

Two bugs I hit, both mine:

1. Indexing data directories from `dd_off + 8` instead of `dd_off` produced
   nonsense and a fault on the first descriptor.
2. Advancing an import thunk by re-deriving its offset each iteration walked off
   the end of the section.

If a descriptor's DLL name is *shorter* than the name you want to substitute
(`dxgi.dll` → `w7shim.dll`), you cannot overwrite it in place. Repoint its
`Name` RVA at a string another descriptor already holds — sharing one name
string between descriptors is legal.

---

## 8. Full checklist

```bash
# 1. environment
export WINEPREFIX=/tmp/wp7 WINEDEBUG=-all
wine cmd /c ver                     # must say 6.1.7601

# 2. does it even load?
WINEDEBUG=-all wine ./say.exe --help

# 3. shim self-test (forces the fallbacks, not Wine's exports)
cd src/shim
python gen_shim.py imports.txt --outdir gen
x86_64-w64-mingw32-gcc -shared -O2 -o w7shim.dll win7shim.c win7shim_fallback.c \
    gen/win7shim_table.c gen/win7shim_thunks.S gen/win7shim.def \
    -lkernel32 -static-libgcc
x86_64-w64-mingw32-gcc -O2 -I. -o test/test_shim.exe test/test_shim.c
cp w7shim.dll test/ && python patch_pe.py test/test_shim.exe --inplace --shim test/w7shim.dll
cd test && WINEDEBUG=-all wine ./test_shim.exe      # expect: RESULT: ALL PASS

# 4. real synthesis through the shipped front end
cd ../..
printf 'سلام دنیا' > /tmp/t.txt
WINEDEBUG=-all wine runtime/say.exe --model models/vits-piper-fa_IR-gyro-medium \
    --out /tmp/out.wav --text-file /tmp/t.txt

# 5. GUI, with evidence
Xvfb :77 -screen 0 1000x700x24 & DISPLAY=:77 openbox &
DISPLAY=:77 WINEDEBUG=-all wine runtime/tts_gui.exe &
DISPLAY=:77 xwininfo -root -tree | grep -i win7-tts
DISPLAY=:77 import -window <id> /tmp/gui.png
```

---

## 9. What this still cannot tell you

- **Real kernel/hardware behaviour.** Wine reimplements the loader and the API
  surface; it is not Windows. Only the user can confirm a genuine Win7 SP1 box.
- **Drivers and audio.** `Failed to create DXGI factory` is expected and
  harmless — there is no GPU.
- **Font availability.** Persian renders as boxes without a suitable font.
- **Locale behaviour.** Wine takes `argv` from the Linux locale, so it cannot
  reproduce a Persian ANSI code page.

This is why the package ships `scripts\diagnose.cmd`. A tool that prints CPU
features, DLL presence and a live exit code turns a vague "it crashes" into a
specific `0xC0000005` in one round trip — that is how the FLS race was finally
located, three messages into a diagnostic exchange. When a target platform
cannot be reproduced locally, **ship the user a probe rather than a guess.**
