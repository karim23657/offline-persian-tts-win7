#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Tkinter front end for win7-tts.

This is the optional, friendlier-to-hack alternative to the bundled native
`tts_gui.exe`.  Both drive the same engine; this one is easier to extend if you
would rather add features in Python.

IMPORTANT - Windows 7 and Python
-------------------------------
Windows 7 cannot run any Python newer than 3.8, because Microsoft dropped OS
support in 3.9.  So use **Python 3.8.10**, the last release with Win7 support:

    https://www.python.org/downloads/release/python-3810/

During installation tick "tcl/tk and IDLE", otherwise tkinter is missing.
Python 3.8 bundles Tcl/Tk 8.6, which handles Persian text fine.

If you do not want to install Python at all, just use runtime\\tts_gui.exe -
it has no dependencies beyond the files already in runtime\\.

How it works
------------
Rather than importing the sherpa-onnx Python module (which would need a
matching build for your Python version and re-introduces the Windows 7 loader
problem), this script drives `runtime\\say.exe` as a subprocess.  say.exe links
the C API directly and takes UTF-8, so Persian survives regardless of the
system code page.  Trade-off: the model reloads per call, so batching is
slower than the native GUI - use scripts\\say_batch.cmd for long lists.
"""

import os
import queue
import subprocess
import sys
import threading
import time
import wave

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
except ImportError:
    sys.stderr.write(
        "tkinter is missing.\n\n"
        "Install Python 3.8.10 for Windows 7 and tick 'tcl/tk and IDLE'.\n"
        "Or just run runtime\\tts_gui.exe, which needs no Python at all.\n")
    raise SystemExit(1)

# Generous but finite: even a very large model on a slow machine finishes.
TTS_TIMEOUT = 600

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RUNTIME = os.path.join(ROOT, "runtime")
SAY = os.path.join(RUNTIME, "say.exe")
MODELS = os.path.join(ROOT, "models")


def find_models():
    """Every folder under models\\ that holds both a .onnx and tokens.txt."""
    out = []
    if not os.path.isdir(MODELS):
        return out
    for name in sorted(os.listdir(MODELS)):
        d = os.path.join(MODELS, name)
        if not os.path.isdir(d):
            continue
        has_onnx = any(f.lower().endswith(".onnx") for f in os.listdir(d))
        if has_onnx and os.path.exists(os.path.join(d, "tokens.txt")):
            out.append(name)
    return out


def wav_duration(path):
    try:
        with wave.open(path, "rb") as w:
            return w.getnframes() / float(w.getframerate())
    except Exception:
        return 0.0


def play(path):
    """Best-effort playback that works on every Windows version."""
    if os.name != "nt":
        try:
            import shutil
            for exe in ("xdg-open", "open", "afplay"):
                if shutil.which(exe):
                    subprocess.Popen([exe, path])
                    return
        except Exception:
            pass
        return
    try:
        import winsound
        winsound.PlaySound(path, winsound.SND_FILENAME)
    except Exception:
        pass


class App(object):
    def __init__(self, root):
        self.root = root
        self.root.title("win7-tts  -  local Persian text to speech")
        self.models = find_models()
        self.busy = False
        self.last_wav = None
        # Worker threads must never touch Tk.  They drop results here and the
        # UI thread drains this queue on a timer; calling root.after() from a
        # thread is not thread-safe in Tk and can abort the interpreter.
        self.results = queue.Queue()

        root.columnconfigure(0, weight=1)
        root.rowconfigure(3, weight=1)

        frm_top = ttk.Frame(root, padding=(10, 8, 10, 4))
        frm_top.grid(row=0, column=0, sticky="ew")
        frm_top.columnconfigure(1, weight=1)

        ttk.Label(frm_top, text="Model:").grid(row=0, column=0, sticky="w", padx=(0, 8))
        self.model = tk.StringVar()
        combo = ttk.Combobox(frm_top, textvariable=self.model, state="readonly",
                             values=self.models, width=44)
        combo.grid(row=0, column=1, sticky="ew")
        if "vits-piper-fa_IR-gyro-medium" in self.models:
            self.model.set("vits-piper-fa_IR-gyro-medium")
        elif self.models:
            self.model.set(self.models[0])
        else:
            messagebox.showwarning(
                "No models found",
                "No model is installed.\n\n"
                "Run scripts\\get_model.cmd to download one.")

        ttk.Label(frm_top, text="Speed:").grid(row=0, column=2, sticky="e", padx=(12, 6))
        self.speed = tk.DoubleVar(value=1.0)
        self.speed_lbl = ttk.Label(frm_top, text="1.00x", width=7, anchor="e")
        self.speed_lbl.grid(row=0, column=3, sticky="e")
        ttk.Scale(frm_top, from_=0.5, to=2.0, orient="horizontal",
                  variable=self.speed, length=160,
                  command=self._on_speed).grid(row=0, column=4, sticky="w")

        ttk.Label(root, text="Text:", padding=(10, 2, 10, 2)).grid(row=1, column=0, sticky="w")

        self.text = tk.Text(root, height=12, wrap="word", undo=True, padx=8, pady=6,
                            font=("Segoe UI", 13))
        self.text.grid(row=3, column=0, sticky="nsew", padx=10)
        # Alignment is a *tag* attribute on a Tk Text widget, not a widget
        # option, so apply a right-justified tag to everything as it is edited.
        # Persian is right-to-left and reads far better this way.
        self.text.tag_configure("rtl", justify="right")

        def _align(_event=None):
            self.text.tag_remove("rtl", "1.0", "end")
            self.text.tag_add("rtl", "1.0", "end-1c")
            return None

        self.text.bind("<<Modified>>", self._on_modified)
        sb = ttk.Scrollbar(root, orient="vertical", command=self.text.yview)
        sb.grid(row=3, column=1, sticky="ns")
        self.text.configure(yscrollcommand=sb.set)

        frm_btn = ttk.Frame(root, padding=(10, 8))
        frm_btn.grid(row=4, column=0, sticky="ew", columnspan=2)

        self.btn_gen = ttk.Button(frm_btn, text="Generate", command=self.generate)
        self.btn_gen.grid(row=0, column=0, padx=(0, 6))
        self.btn_play = ttk.Button(frm_btn, text="Play", command=self.on_play, state="disabled")
        self.btn_play.grid(row=0, column=1, padx=6)
        self.btn_open = ttk.Button(frm_btn, text="Open folder", command=self.on_open,
                                   state="disabled")
        self.btn_open.grid(row=0, column=2, padx=6)

        self.status = tk.StringVar(value="Ready.")
        ttk.Label(root, textvariable=self.status, padding=(10, 4, 10, 8),
                  anchor="w").grid(row=5, column=0, sticky="ew", columnspan=2)

        self.progress = ttk.Progressbar(root, mode="indeterminate")
        self.text.focus_set()
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(80, self._drain_results)

    def _drain_results(self):
        """UI-thread timer: apply anything the worker thread produced."""
        try:
            while True:
                kind, payload = self.results.get_nowait()
        except queue.Empty:
            kind, payload = None, None

        if kind == "done":
            self._on_done(payload)
        elif kind == "failed":
            self._on_failed(payload)

        self.root.after(80, self._drain_results)

    def _on_modified(self, event=None):
        """Keep the whole paragraph right-justified as the user types."""
        if not self.text.edit_modified():
            return None
        self.text.tag_remove("rtl", "1.0", "end")
        self.text.tag_add("rtl", "1.0", "end-1c")
        self.text.edit_modified(False)
        return None

    def _on_speed(self, val):
        self.speed_lbl.configure(text="%.2fx" % float(val))

    def _set_busy(self, busy):
        self.busy = busy
        state = "disabled" if busy else "normal"
        self.btn_gen.configure(state=state)
        if not busy and self.last_wav and os.path.exists(self.last_wav):
            self.btn_play.configure(state="normal")
            self.btn_open.configure(state="normal")
        else:
            self.btn_play.configure(state="disabled")
            self.btn_open.configure(state="disabled")

    def generate(self):
        if self.busy:
            return
        text = self.text.get("1.0", "end").strip()
        if not text:
            messagebox.showinfo("win7-tts", "Type something to say first.")
            return
        if not self.model.get():
            messagebox.showwarning("win7-tts", "No model selected.")
            return

        out = os.path.join(ROOT, "out.wav")
        model_dir = os.path.join(MODELS, self.model.get())
        self._set_busy(True)
        self.status.set("Loading model and synthesising, please wait...")
        self.progress.start(12)

        threading.Thread(target=self._work,
                         args=(text, model_dir, out), daemon=True).start()

    def _work(self, text, model_dir, out):
        try:
            # say.exe takes UTF-8 from a file, which sidesteps every code page
            # problem on Windows 7.
            tmp = os.path.join(ROOT, "_tts_input.txt")
            with open(tmp, "w", encoding="utf-8") as fh:
                fh.write(text)

            cmd = [SAY, "--model", model_dir, "--out", out,
                   "--text-file", tmp, "--speed", "%.2f" % self.speed.get()]

            # Capture say.exe's output through a file rather than a pipe.  A
            # pipe can hang forever here: anything the engine spawns inherits
            # the write end and never closes it, so communicate() never returns.
            log = os.path.join(ROOT, "_tts_output.txt")
            output = ""
            try:
                with open(log, "wb") as fh:
                    proc = subprocess.Popen(cmd, stdout=fh, stderr=subprocess.STDOUT)
                    try:
                        # Bounded: a wedged engine must not freeze the GUI
                        # forever with the window greyed out.
                        proc.wait(timeout=TTS_TIMEOUT)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                        raise RuntimeError(
                            "the engine did not finish within %d seconds; the "
                            "model may be too large for this machine"
                            % TTS_TIMEOUT)
                with open(log, "rb") as fh:
                    output = fh.read().decode("utf-8", "replace")
            except Exception as exc:
                proc = None
                output = "could not run the engine: %s" % exc
            finally:
                for leftover in (tmp, log):
                    try:
                        os.remove(leftover)
                    except OSError:
                        pass

            rc = proc.returncode if proc is not None else -1
            if rc != 0 or not os.path.exists(out):
                detail = output.strip() or "TTS failed (exit code %d)." % rc
                self.results.put(("failed", detail))
                return
            self.results.put(("done", (out, wav_duration(out))))
        except Exception as exc:  # pragma: no cover - defensive
            self.results.put(("failed", str(exc)))

    def _on_done(self, payload):
        out, dur = payload
        self.progress.stop()
        self.last_wav = out
        self._set_busy(False)
        self.status.set("Done: %.2f s of audio -> %s" % (dur, out))
        threading.Thread(target=play, args=(out,), daemon=True).start()

    def _on_failed(self, why):
        self.progress.stop()
        self._set_busy(False)
        self.status.set("Failed.")
        messagebox.showerror("win7-tts", why)

    def on_play(self):
        if self.last_wav:
            self.status.set("Playing...")
            threading.Thread(target=play, args=(self.last_wav,), daemon=True).start()

    def on_open(self):
        if self.last_wav:
            path = os.path.dirname(self.last_wav) or ROOT
            try:
                if os.name == "nt":
                    os.startfile(path)
                else:
                    subprocess.Popen(["xdg-open", path])
            except Exception:
                pass

    def on_close(self):
        self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
