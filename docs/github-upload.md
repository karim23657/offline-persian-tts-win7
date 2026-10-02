# Publishing this to GitHub

The repository is `win7-tts/`. Nothing large is committed - `runtime/` and
`models/` are build products and are git-ignored - so a normal push is small.
The two zip archives are **release assets**, not repository files.

## What is in git, and what is not

| Path | In git? | Why |
| --- | --- | --- |
| `src/`, `scripts/`, `docs/`, `Makefile` | yes | the actual project |
| `src/shim/imports.txt` | yes | generated, but committing it shows reviewers exactly which Windows APIs the shim must satisfy |
| `runtime/` | no | 24 MB of upstream DLLs plus our builds; `make` recreates it |
| `models/` | no | 79 MB model; users fetch it with `scripts\get_model.cmd` |
| `build/`, `dist/` | no | intermediates and archives |
| `dist/*.zip` | no | release assets, uploaded separately |

---

## One-time setup

```bash
cd /content/win7-tts
git init                       # already done if .git exists
git add .
git status                     # check nothing big slipped in
```

Sanity check before committing - nothing here should appear:

```bash
git status --porcelain | awk '{print $2}' | grep -E '^(runtime|models|build|dist)/' && echo "STOP: large files staged"
```

Create the remote (do this once; if the repo already exists on GitHub, skip):

```bash
gh repo create win7-tts --public --description \
  "Offline Persian (and any language) TTS for Windows 7 - sherpa-onnx plus a Win7 API shim" \
  --source=. --remote=origin
```

or by hand: <https://github.com/new> -> name it `win7-tts`, **do not** tick
"add a README" (we already have one).

## First commit and push

```bash
git config user.name  "Your Name"
git config user.email "you@example.com"

git checkout -b main
git add .
git commit -m "Initial release: Windows 7 TTS package

sherpa-onnx 1.13.8 patched for Windows 7 with w7shim.dll, a say.exe front end
that passes text as UTF-8 through the C API, a native Win32 GUI, a Tkinter
GUI, and scripts/diagnose.cmd."
git push -u origin main
```

If `git push` asks for credentials, use a
[personal access token](https://github.com/settings/tokens) (classic, `repo`
scope) as the password, not your account password.

---

## Releasing

Build the archives:

```bash
make release        # -> dist/win7-tts.zip and dist/win7-tts-runtime-only.zip
ls -lh dist/
```

Tag and push:

```bash
git tag -a v1.0.0 -m "First release: sherpa-onnx 1.13.8"
git push origin v1.0.0
```

Upload the assets. `gh` handles this in one step:

```bash
gh release create v1.0.0 \
  dist/win7-tts.zip \
  dist/win7-tts-runtime-only.zip \
  --title "v1.0.0" \
  --notes "$(cat <<'EOF'
Offline text-to-speech for Windows 7.

- `win7-tts.zip` (73 MB) - everything, model included, works offline.
- `win7-tts-runtime-only.zip` (8.6 MB) - program only; run
  `scripts\get_model.cmd gyro` once to fetch the model.

**Requirements:** 64-bit Windows 7 SP1, a CPU with AVX (2011 or newer), and the
Visual C++ 2015-2022 x64 redistributable.

**If anything crashes, run `scripts\diagnose.cmd` and include the output.**

Run from the command line with:
    scripts\say.cmd --file hello.txt
EOF
)"
```

Or upload by hand: <https://github.com/karim23657/win7-tts/releases/new> ->
choose the tag -> drag both zips into the assets box -> publish.

---

## After every release

```bash
make release
gh release upload v1.0.0 dist/win7-tts.zip --clobber
```

## Notes

- **CI** (`.github/workflows/ci.yml`) runs on every push: it builds, runs the
  shim self-test, synthesises Persian audio and verifies no un-redirected
  Windows 8+ imports remain. Watch for it before tagging.
- **Release assets are not size-limited by git**, so a 73 MB zip is fine there
  even though the same file could not be committed.
- **The model is never uploaded.** It is fetched from GitHub Releases or
  HuggingFace by `scripts\get_model.cmd`, which keeps this repository small and
  leaves the model under its own licence.
- If you fork this, update the model URLs in `scripts\get_model.cmd` to your own
  mirror or HuggingFace account.
