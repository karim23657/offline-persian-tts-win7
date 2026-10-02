# Upgrading to a new sherpa-onnx release

The engine is the only upstream dependency, and upgrading it touches exactly
one pinned file. The whole procedure is:

```bash
# 1. change the version
$EDITOR src/shim/sherpa-version.env      # SHERPA_VERSION=1.13.9

# 2. re-download, re-scan, rebuild
make upgrade && make && make test && make release
```

`make upgrade` re-fetches the release, regenerates `imports.txt` from the new
binaries and clears the patch stamp so the rebuild really happens.

Expect ~5 minutes. Read the failure modes below first.

---

## Why `imports.txt` is generated, not hand-written

This is the single most important thing to get right.

Windows resolves imports by name at load time. The official binaries are built
with a Windows 10 SDK and statically import functions Windows 7 lacks. If a new
release adds even **one** such import that `imports.txt` does not mention, the
package builds cleanly, passes every test on Wine and Windows 10 - and then
fails on Windows 7 with:

```
The procedure entry point <NewFunction> could not be located in the dynamic
library KERNEL32.dll.
```

That is a confusing, late, user-reported failure. Generating the list from the
actual binaries removes that entire class of problem, which is why
`scan_imports.py` exists and why `imports.txt` is regenerated on every build.

`patch_pe.py` is the second line of defence: it refuses to patch a binary
unless the shim exports every name being redirected.

---

## Procedure in full

### 1. Pin the new version

```bash
$EDITOR src/shim/sherpa-version.env
```

```diff
-SHERPA_VERSION=1.13.8
+SHERPA_VERSION=1.13.9
```

`SHERPA_WIN_ASSET` uses `$(SHERPA_VERSION)` so the version appears once.

### 2. Fetch and rescan

```bash
make upgrade
```

It prints the count of functions found per binary. **Read those numbers.**

A large jump (say 105 -> 140) means the new release added imports; expect to
write new fallbacks. A collapse to a tiny number means something went wrong -
usually the fetch silently returned an HTML error page, or the asset name
pattern changed.

```bash
head -3 src/shim/imports.txt        # sanity-check the header
git diff --stat src/shim/imports.txt
```

The diff should show added or removed names, nothing else.

### 3. Check what is new

```bash
git diff src/shim/imports.txt | tr ' ' '\n' | grep '^+' | sort > /tmp/new.txt
```

For each newly required function, ask: **does Windows 7 have this?**

- If yes - nothing to do. `win7shim_resolve()` finds it at runtime.
- If no - it needs a fallback in `src/shim/win7shim_fallback.c`, plus an entry
  in the `win7shim_get_fallback()` dispatch table.

Without that fallback the shim still loads (the slot becomes
`xor eax,eax; ret`), so the symptom is a quiet wrong answer rather than a
crash - which is harder to notice. Prefer failing loudly during review.

### 4. Rebuild and test

```bash
make            # regenerates imports -> shim -> patch -> front ends
make test       # must print RESULT: ALL PASS
```

### 5. Smoke test real audio

```bash
python3 -c "open('/tmp/t.txt','w',encoding='utf-8').write('سلام دنیا')"
WINEPREFIX=/tmp/wp7 WINEDEBUG=-all wine runtime/say.exe \
    --model models/vits-piper-fa_IR-gyro-medium \
    --out /tmp/out.wav --text-file /tmp/t.txt
python3 -c "import wave; w=wave.open('/tmp/out.wav'); \
    print('%.2f s' % (w.getnframes()/w.getframerate()))"
```

Expect a few seconds of audio. 0.03 s means the text reached the engine as
`????`; silence means the model or its phonemiser broke.

Also run the GUI once (`Xvfb`/`openbox`, see [win7.md](win7.md)) - a change in
onnxruntime can move the crash from load time into the first inference.

### 6. Package and verify from a clean extract

```bash
make release
mkdir /tmp/verify && cd /tmp/verify && unzip -q ../../dist/win7-tts.zip
cd win7-tts && WINEPREFIX=/tmp/wp7 WINEDEBUG=-all \
    wine runtime/say.exe --model models/vits-piper-fa_IR-gyro-medium \
        --out out.wav --text-file /tmp/t.txt
```

A stale zip shipped twice in this project. Always test the extract.

### 7. Commit

```bash
git add src/shim/sherpa-version.env src/shim/imports.txt
git add src/shim/win7shim*.c src/shim/gen_shim.py      # if fallbacks changed
git commit -m "Upgrade to sherpa-onnx 1.13.9"
```

Update `CHANGELOG.md` with the version, what changed, and any new fallbacks.

---

## Failure modes

| Symptom | Cause | Fix |
| --- | --- | --- |
| `scan_imports` finds ~0 imports | you patched the downloaded originals, so `KERNEL32.dll` no longer appears | `make upgrade` re-fetches a clean copy; never patch `build/sherpa/unpacked/` |
| `HTTP Error 404` on fetch | the asset name pattern changed upstream | list the assets: `curl -s https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/tags/vX.Y.Z \| grep -o '"name": "sherpa-onnx-v[^"]*win-x64[^"]*"'` |
| `shim is missing N export(s)` | `imports.txt` is stale | `make upgrade` |
| Audio is 0.03 s | text reached the engine as `????` | pass it with `--text-file`, not `--text` |
| Crash only on real Windows 7 | a fallback is wrong and Wine hid it | see below |
| `.cmd` behaves oddly | a UTF-8 BOM crept in | re-save UTF-8 **without** BOM, CRLF |

---

## If a Windows 7-only crash appears

The single hardest thing about this project is that **Wine exports the
Windows 8+ APIs the shim exists to replace**, so the fallbacks never run under
Wine. A green CI run does not prove them.

When a user reports a crash that you cannot reproduce:

1. Get `scripts\diagnose.cmd` output. It prints the CPU features and a live
   exit code; that is how the FLS race was located.
2. Reproduce with the fallback forced, not forwarded. The shim exports
   `win7shim_get_fallback(name)` precisely so a test can call the Windows 7
   implementation directly. Add a case to
   `src/shim/test/test_shim.c`.
3. If it is a race, **prove the test catches the old code** - deliberately
   widen the window with a `Sleep()` and watch the old version fail. A test
   that passes against both versions protects nothing.
