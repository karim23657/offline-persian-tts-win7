/*
 * tts_gui.exe - a small native Windows 7 GUI for the local Persian TTS.
 *
 * Design notes
 * ------------
 * Native Win32 rather than Tkinter on purpose: Windows 7 cannot run any Python
 * newer than 3.8, and requiring an interpreter would undercut the point of a
 * package that is meant to just run.  This links the sherpa-onnx C API
 * directly, so there is no extra dependency at all.
 *
 * Two things that matter for this use case:
 *
 *  - Everything user-facing is wide (W) API.  Persian typed into the text box
 *    is UTF-16 already, and it is converted to UTF-8 (what the sherpa-onnx API
 *    documents) only at the boundary.  No code page games, so the text renders
 *    correctly in the edit box *and* reaches the synthesiser intact.
 *
 *  - Synthesis runs on a worker thread.  Loading a model takes a few seconds and
 *    generating is CPU-bound, so doing it on the UI thread would freeze the
 *    window and make Windows paint it as "not responding".
 */

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sherpa-onnx/c-api/c-api.h"
#include "tts_engine.h"

#ifndef IDC_ARROW
#define IDC_ARROW 32512
#endif

#define ID_GENERATE  1001
#define ID_PLAY      1002
#define ID_STOP      1003
#define ID_SAVE      1004
#define ID_SAVEAS     1010
#define ID_MODEL     1005
#define ID_TEXT      1006
#define ID_STATUS    1007
#define ID_SPEED     1008
#define ID_PROGRESS  1009

#define WM_APP_DONE  (WM_APP + 1)
#define WM_APP_FAIL  (WM_APP + 2)
#define WM_APP_PROG  (WM_APP + 3)

#define MAX_PATHLEN 4096

/* ------------------------------------------------------------------ state */

typedef struct {
    HWND hwnd;
    HWND edit;        /* multi-line text box                        */
    HWND combo;       /* model chooser                              */
    HWND status;      /* status bar                                 */
    HWND progress;    /* progress bar                               */
    HWND btn_gen;
    HWND btn_play;
    HWND btn_stop;
    HWND btn_save;   /* "Save as..." - chooses the output file  */
    HWND btn_open;   /* "Open folder" - reveals the output     */
    HWND speed;       /* speed slider                               */
    HFONT font;
    HFONT bold_font;
    wchar_t model_dir[MAX_PATHLEN];
    wchar_t wav_path[MAX_PATHLEN];
    int generating;
} App;

/* Payload handed from the worker thread to the UI thread. */
typedef struct {
    App *app;
    wchar_t wav_path[MAX_PATHLEN];  /* wide, for the UI */
    char wav_narrow[MAX_PATHLEN];   /* narrow, for the sherpa-onnx API */
    double seconds;
    int sample_rate;
    int sid;
    float speed;
    float length_scale;
    char text_utf8[1];              /* over-allocated: the real UTF-8 text follows */
} Job;

/* ------------------------------------------------------------------ utf8  */

/* Convert UTF-16 to UTF-8; returns a malloc'd NUL-terminated buffer. */
static char *utf16_to_utf8(const wchar_t *in) {
    int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, NULL, 0, NULL, NULL);
    char *out;
    if (n <= 0) return NULL;
    out = (char *)malloc((size_t)n);
    if (!out) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, in, -1, out, n, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* --------------------------------------------------------- model listing */

/* Every subdirectory of models\ that holds a .onnx is offered in the combo. */
static void populate_models(App *app) {
    wchar_t base[MAX_PATHLEN];
    wchar_t pattern[MAX_PATHLEN];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    GetModuleFileNameW(NULL, base, MAX_PATHLEN);
    {
        wchar_t *slash = wcsrchr(base, L'\\');
        if (slash) {
            *slash = L'\0';
            _snwprintf(base + wcslen(base), MAX_PATHLEN - wcslen(base),
                       L"\\..\\models");
        }
    }
    _snwprintf(pattern, MAX_PATHLEN, L"%s\\*", base);

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            _snwprintf(app->model_dir, MAX_PATHLEN, L"%s\\%s", base, fd.cFileName);
            {
                wchar_t probe[MAX_PATHLEN];
                _snwprintf(probe, MAX_PATHLEN, L"%s\\tokens.txt", app->model_dir);
                if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
                    SendMessageW(app->combo, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (SendMessageW(app->combo, CB_GETCOUNT, 0, 0) > 0) {
        /* Prefer gyro when it is present, otherwise take the first entry. */
        LRESULT idx = SendMessageW(app->combo, CB_FINDSTRINGEXACT, -1, (LPARAM)L"vits-piper-fa_IR-gyro-medium");
        if (idx == CB_ERR) idx = 0;
        SendMessageW(app->combo, CB_SETCURSEL, (WPARAM)idx, 0);
    }
}

/* --------------------------------------------------------------- synthesis */

/*
 * Build the engine from a model directory.  Mirrors say.exe so both front ends
 * behave identically.
 */
/*
 * Synthesis worker.
 *
 * The engine comes from the shared cache in tts_engine.c, so it stays loaded and
 * warm between generations.  It used to be rebuilt on *every* click, and
 * loading the 63 MB model dominated the wait - now only the very first
 * generation pays that cost, and it can be paid up front by --preload.
 */

typedef struct {
    App *app;
    float *samples;      /* collected so the wav can be written at the end */
    int count;
    int cap;
} ProgressCtx;

/*
 * Matches TtsSampleSink (samples, count, progress, user).  It does two jobs:
 * forward progress to the UI thread, and collect the samples so the worker can
 * write the .wav once generation has finished.
 */
static void on_samples(const float *samples, int count, float progress, void *user) {
    ProgressCtx *ctx = (ProgressCtx *)user;
    if (count > 0) {
        if (ctx->count + count > ctx->cap) {
            int cap = ctx->cap ? ctx->cap : 65536;
            float *grown;
            while (cap < ctx->count + count) cap *= 2;
            grown = (float *)realloc(ctx->samples, (size_t)cap * sizeof(float));
            if (grown) {
                ctx->samples = grown;
                ctx->cap = cap;
            }
        }
        if (ctx->count + count <= ctx->cap) {
            memcpy(ctx->samples + ctx->count, samples, (size_t)count * sizeof(float));
            ctx->count += count;
        }
    }
    PostMessageW(ctx->app->hwnd, WM_APP_PROG, (WPARAM)(int)(progress * 100.0f), 0);
}

static DWORD WINAPI worker(LPVOID param) {
    Job *job = (Job *)param;
    TtsEngine *engine;
    ProgressCtx pctx;
    wchar_t werr[512];
    int n_samples = 0;
    char err[512];

    memset(&pctx, 0, sizeof(pctx));
    pctx.app = job->app;

    engine = TtsEngineAcquire(job->app->model_dir, werr,
                              sizeof(werr) / sizeof(werr[0]));
    if (!engine) {
        PostMessageW(job->app->hwnd, WM_APP_FAIL, 0, (LPARAM)werr);
        free(job);
        return 1;
    }

    if (!TtsEngineGenerate(engine, job->text_utf8, job->speed, job->sid,
                           on_samples, &pctx, &n_samples, err, sizeof(err))) {
        TtsEngineTouch(engine);
        TtsEngineRelease();
        PostMessageW(job->app->hwnd, WM_APP_FAIL, 0, (LPARAM)L"No audio was produced.");
        free(job);
        return 1;
    }

    job->sample_rate = TtsEngineSampleRate(job->app->model_dir);
    if (job->sample_rate <= 0) job->sample_rate = 22050;
    if (n_samples <= 0) n_samples = pctx.count;
    job->seconds = (double)n_samples / (double)job->sample_rate;

    TtsEngineTouch(engine);
    TtsEngineRelease();   /* release the generation lock Acquire took */

    /*
     * Write the file here, on the worker thread, and only report success once
     * the audio is really on disk.  Doing it later (or not at all) makes the
     * status bar claim a file exists when it does not.
     */
    if (!SherpaOnnxWriteWave(pctx.samples, n_samples, job->sample_rate,
                             job->wav_narrow)) {
        free(pctx.samples);
        PostMessageW(job->app->hwnd, WM_APP_FAIL, 0,
                     (LPARAM)L"Could not write the .wav file.");
        free(job);
        return 1;
    }
    free(pctx.samples);

    PostMessageW(job->app->hwnd, WM_APP_DONE, 0, (LPARAM)job);
    return 0;
}

/*
 * Load a model in the background so the first Generate feels instant.
 * Failures are ignored here: the real Generate will report them properly.
 */
static DWORD WINAPI preload_thread(LPVOID param) {
    App *app = (App *)param;
    wchar_t werr[512];
    TtsEngine *engine = TtsEngineAcquire(app->model_dir, werr, 512);
    if (engine) {
        TtsEngineTouch(engine);
    }
    TtsEngineRelease();
    return 0;
}


/* Defined with the worker below; declared here so WM_CREATE can start it. */
static DWORD WINAPI preload_thread(LPVOID param);

/* -------------------------------------------------------------------- UI  */

static void set_status(App *app, const wchar_t *text) {
    SendMessageW(app->status, SB_SETTEXTW, 0, (LPARAM)text);
}

static void set_controls_enabled(App *app, int generating) {
    EnableWindow(app->btn_gen, !generating);
    EnableWindow(app->btn_save, !generating);
    EnableWindow(app->btn_open, !generating);
    EnableWindow(app->btn_play, !generating);
    EnableWindow(app->edit, !generating);
    EnableWindow(app->combo, !generating);
    ShowWindow(app->btn_stop, generating ? SW_SHOW : SW_HIDE);
    app->generating = generating;
}

/* Ask for the output path, then run the synthesiser. */
static void start_generate(App *app) {
    Job *job;
    char *utf8;
    int len;
    LRESULT idx;
    int i;

    /* Which model folder did the user pick? */
    idx = SendMessageW(app->combo, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) {
        MessageBoxW(app->hwnd, L"No model is installed.\n\nRun scripts\\get_model.cmd to download one.",
                    L"win7-tts", MB_ICONWARNING);
        return;
    }
    {
        wchar_t base[MAX_PATHLEN], picked[MAX_PATHLEN];
        GetModuleFileNameW(NULL, base, MAX_PATHLEN);
        {
            wchar_t *slash = wcsrchr(base, L'\\');
            if (slash) {
                *slash = L'\0';
                _snwprintf(base + wcslen(base), MAX_PATHLEN - wcslen(base), L"\\..\\models");
            }
        }
        SendMessageW(app->combo, CB_GETLBTEXT, (WPARAM)idx, (LPARAM)picked);
        _snwprintf(app->model_dir, MAX_PATHLEN, L"%s\\%s", base, picked);
    }

    /* Pull the text out of the edit control. */
    len = GetWindowTextLengthW(app->edit);
    if (len <= 0) {
        MessageBoxW(app->hwnd, L"Type something to say first.", L"win7-tts", MB_ICONINFORMATION);
        SetFocus(app->edit);
        return;
    }
    {
        wchar_t *text = (wchar_t *)malloc((size_t)(len + 1) * sizeof(wchar_t));
        if (!text) return;
        GetWindowTextW(app->edit, text, len + 1);

        utf8 = utf16_to_utf8(text);
        if (!utf8) {
            free(text);
            MessageBoxW(app->hwnd, L"Could not convert the text.", L"win7-tts", MB_ICONERROR);
            return;
        }
        /* Drop a UTF-8 BOM: the synthesiser would read it as a character. */
        if ((unsigned char)utf8[0] == 0xEF && (unsigned char)utf8[1] == 0xBB &&
            (unsigned char)utf8[2] == 0xBF) {
            memmove(utf8, utf8 + 3, strlen(utf8 + 3) + 1);
        }

        /*
         * Output goes next to the executable as out.wav unless the user has
         * already chosen somewhere with "Save as...".  Asking for a path on
         * every single utterance would be tedious.
         */
        if (!app->wav_path[0]) {
            wchar_t exe[MAX_PATHLEN];
            GetModuleFileNameW(NULL, exe, MAX_PATHLEN);
            {
                wchar_t *slash = wcsrchr(exe, L'\\');
                if (slash) *slash = L'\0'; else wcscpy(exe, L".");
            }
            _snwprintf(app->wav_path, MAX_PATHLEN, L"%s\\out.wav", exe);
        }

        job = (Job *)malloc(sizeof(Job) + strlen(utf8) + 1);
        if (!job) {
            free(utf8);
            free(text);
            return;
        }
        memset(job, 0, sizeof(Job));
        job->app = app;
        wcscpy(job->wav_path, app->wav_path);
        job->sid = 0;
        job->length_scale = 1.0f;
        {
            /* Slider is 0..100 mapping to speed 0.5 .. 2.0 */
            LRESULT pos = SendMessageW(app->speed, TBM_GETPOS, 0, 0);
            job->speed = (float)pos / 100.0f;
        }
        strcpy(job->text_utf8, utf8);

        free(utf8);
        free(text);
    }

    /* The sherpa-onnx API takes narrow paths; the UI keeps the wide one. */
    if (!WideCharToMultiByte(CP_UTF8, 0, job->wav_path, -1, job->wav_narrow,
                             MAX_PATHLEN, NULL, NULL)) {
        free(job);
        MessageBoxW(app->hwnd, L"Could not convert the output path.",
                    L"win7-tts", MB_ICONERROR);
        return;
    }

    set_controls_enabled(app, 1);
    SendMessageW(app->progress, PBM_SETPOS, 0, 0);
    set_status(app, L"Loading model, please wait...");

    i = (int)(INT_PTR)CreateThread(NULL, 0, worker, job, 0, NULL);
    if (i == 0) {
        free(job);
        set_controls_enabled(app, 0);
        MessageBoxW(app->hwnd, L"Could not start the worker thread.",
                    L"win7-tts", MB_ICONERROR);
    }
}

/* Play the wav we just produced. */
static void play_wav(App *app) {
    if (GetFileAttributesW(app->wav_path) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(app->hwnd, L"Generate some speech first.",
                    L"win7-tts", MB_ICONINFORMATION);
        return;
    }
    if (!PlaySoundW(app->wav_path, NULL, SND_FILENAME | SND_ASYNC)) {
        MessageBoxW(app->hwnd, L"Could not play the audio file.\n\n"
                               L"If this is the first time, double-click a .wav in "
                               L"Explorer to check audio output works.",
                    L"win7-tts", MB_ICONWARNING);
    } else {
        set_status(app, L"Playing...");
    }
}

static void layout(App *app, int width, int height) {
    const int margin = 12;
    const int label_w = 74;
    int y;

    /* --- model row --- */
    y = margin;
    MoveWindow(GetDlgItem(app->hwnd, ID_MODEL - 1), margin, y + 3, label_w, 20, TRUE);
    MoveWindow(app->combo, margin + label_w, y, width - margin * 2 - label_w, 24, TRUE);

    /* --- text box --- */
    y += 32;
    MoveWindow(GetDlgItem(app->hwnd, ID_TEXT - 1), margin, y + 3, label_w, 20, TRUE);
    MoveWindow(app->edit, margin + label_w, y,
               width - margin * 2 - label_w, height - y - 132, TRUE);

    /* --- speed row --- */
    y = height - 124;
    MoveWindow(app->speed, margin + label_w, y, 200, 24, TRUE);
    {
        wchar_t buf[64];
        LRESULT pos = SendMessageW(app->speed, TBM_GETPOS, 0, 0);
        _snwprintf(buf, 64, L"%.2fx", (double)pos / 100.0);
        SetDlgItemTextW(app->hwnd, ID_SPEED + 100, buf);
        MoveWindow(GetDlgItem(app->hwnd, ID_SPEED + 100),
                   margin + label_w + 208, y + 3, 48, 20, TRUE);
    }

    /* --- buttons --- */
    {
        const int bw = 108, bh = 30, gap = 8;
        int x = margin;
        MoveWindow(app->btn_gen, x, y + 34, bw, bh, TRUE); x += bw + gap;
        MoveWindow(app->btn_play, x, y + 34, bw, bh, TRUE); x += bw + gap;
        MoveWindow(app->btn_stop, x, y + 34, bw, bh, TRUE); x += bw + gap;
        MoveWindow(app->btn_save, x, y + 34, bw, bh, TRUE); x += bw + gap;
        MoveWindow(app->btn_open, x, y + 34, bw, bh, TRUE);
    }

    /*
     * Controls are created before their labels, so on Win7 the default z-order
     * paints the labels underneath.  Lift them so the row captions are visible.
     */
    BringWindowToTop(GetDlgItem(app->hwnd, ID_MODEL - 1));
    BringWindowToTop(GetDlgItem(app->hwnd, ID_TEXT - 1));
    BringWindowToTop(GetDlgItem(app->hwnd, ID_SPEED - 1));
    BringWindowToTop(GetDlgItem(app->hwnd, ID_SPEED + 100));

    /* --- progress + status --- */
    MoveWindow(app->progress, margin, height - 46, width - margin * 2, 10, TRUE);
    MoveWindow(app->status, 0, height - 30, width, 22, TRUE);
}

/* Static label helper: creates a borderless text label. */
static HWND make_label(HWND parent, const wchar_t *text, int id) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
}

static void create_controls(App *app, HINSTANCE inst) {
    app->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    app->bold_font = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    make_label(app->hwnd, L"Model", ID_MODEL - 1);
    app->combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST |
                                 CBS_HASSTRINGS,
                                 0, 0, 200, 400, app->hwnd, (HMENU)(INT_PTR)ID_MODEL, inst, NULL);

    make_label(app->hwnd, L"Text", ID_TEXT - 1);
    app->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                ES_AUTOVSCROLL | ES_WANTRETURN,
                                0, 0, 200, 200, app->hwnd, (HMENU)(INT_PTR)ID_TEXT, inst, NULL);

    make_label(app->hwnd, L"Speed", ID_SPEED - 1);
    app->speed = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                 WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                                 0, 0, 200, 24, app->hwnd, (HMENU)(INT_PTR)ID_SPEED, inst, NULL);
    /* Range 50..200: the slider value is speed * 100, so 100 is exactly 1.00x. */
    SendMessageW(app->speed, TBM_SETRANGE, TRUE, MAKELPARAM(50, 200));
    SendMessageW(app->speed, TBM_SETPOS, TRUE, 100);

    make_label(app->hwnd, L"1.00x", ID_SPEED + 100);

    app->btn_gen = CreateWindowExW(0, L"BUTTON", L"&Generate and save...",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, app->hwnd, (HMENU)(INT_PTR)ID_GENERATE, inst, NULL);
    app->btn_play = CreateWindowExW(0, L"BUTTON", L"&Play",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, app->hwnd, (HMENU)(INT_PTR)ID_PLAY, inst, NULL);
    app->btn_stop = CreateWindowExW(0, L"BUTTON", L"Stop",
                                    WS_CHILD | BS_PUSHBUTTON,
                                    0, 0, 0, 0, app->hwnd, (HMENU)(INT_PTR)ID_STOP, inst, NULL);
    app->btn_save = CreateWindowExW(0, L"BUTTON", L"&Save as...",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, app->hwnd, (HMENU)(INT_PTR)ID_SAVEAS, inst, NULL);
    app->btn_open = CreateWindowExW(0, L"BUTTON", L"&Open folder",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, app->hwnd, (HMENU)(INT_PTR)ID_SAVE, inst, NULL);

    app->progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                    WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                    0, 0, 0, 0, app->hwnd, (HMENU)(INT_PTR)ID_PROGRESS, inst, NULL);
    SendMessageW(app->progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    app->status = CreateWindowExW(0, L"msctls_statusbar32", L"",
                                  WS_CHILD | WS_VISIBLE | SBT_TOOLTIPS,
                                  0, 0, 0, 0, app->hwnd, (HMENU)(INT_PTR)ID_STATUS, inst, NULL);

    SendMessageW(app->btn_gen, WM_SETFONT, (WPARAM)app->font, TRUE);
    SendMessageW(app->btn_play, WM_SETFONT, (WPARAM)app->font, TRUE);
    SendMessageW(app->btn_stop, WM_SETFONT, (WPARAM)app->font, TRUE);
    SendMessageW(app->btn_save, WM_SETFONT, (WPARAM)app->font, TRUE);
    SendMessageW(app->btn_open, WM_SETFONT, (WPARAM)app->font, TRUE);
    SendMessageW(app->combo, WM_SETFONT, (WPARAM)app->font, TRUE);
    SendMessageW(app->edit, WM_SETFONT, (WPARAM)app->font, TRUE);
    SendMessageW(app->speed, WM_SETFONT, (WPARAM)app->font, TRUE);
    SendMessageW(app->status, WM_SETFONT, (WPARAM)app->font, TRUE);
}

/* -------------------------------------------------------- window proc    */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App *app = (App *)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        app = (App *)cs->lpCreateParams;
        app->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
        create_controls(app, (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        populate_models(app);
        TtsEngineInit();
        layout(app, 760, 520);
        set_controls_enabled(app, 0);
        set_status(app, L"Ready. Type Persian or any other text and press Generate.");
        SetFocus(app->edit);
        /*
         * Load the default model in the background straight away.  The window
         * is usable immediately and the first Generate does not have to wait
         * for a 63 MB model to come off disk.
         */
        CreateThread(NULL, 0, preload_thread, app, 0, NULL);
        return 0;
    }

    case WM_COMMAND:
        if (!app) break;
        switch (LOWORD(wp)) {
        case ID_GENERATE: start_generate(app); return 0;
        case ID_PLAY:     play_wav(app); return 0;
        case ID_STOP:     PlaySoundW(NULL, NULL, SND_PURGE); return 0;
        case ID_SAVEAS: {
            wchar_t name[MAX_PATHLEN];
            OPENFILENAMEW ofn;
            if (app->wav_path[0]) wcscpy(name, app->wav_path);
            else _snwprintf(name, MAX_PATHLEN, L"out.wav");
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = app->hwnd;
            ofn.lpstrFilter = L"Wave audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
            ofn.lpstrFile = name;
            ofn.nMaxFile = MAX_PATHLEN;
            ofn.lpstrTitle = L"Choose where to save the speech";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                wcscpy(app->wav_path, name);
                set_status(app, L"Output file chosen.");
            }
            return 0;
        }
        case ID_SAVE: {
            if (GetFileAttributesW(app->wav_path) == INVALID_FILE_ATTRIBUTES) {
                set_status(app, L"Generate some speech first.");
                return 0;
            }
            /* Reveal the last file we wrote. */
            {
                wchar_t cmd[MAX_PATHLEN + 64];
                set_status(app, L"Opening the output folder...");
                _snwprintf(cmd, MAX_PATHLEN + 64, L"/select,\"%s\"", app->wav_path);
                ShellExecuteW(hwnd, L"open", L"explorer.exe", cmd, NULL, SW_SHOWNORMAL);
            }
            return 0;
        }
        case ID_MODEL:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                return 0;
            }
            break;
        }
        break;

    case WM_HSCROLL:
        if (!app) break;
        if ((HWND)lp == app->speed || LOWORD(wp) == TB_THUMBPOSITION ||
            LOWORD(wp) == TB_ENDTRACK) {
            wchar_t buf[64];
            LRESULT pos = SendMessageW(app->speed, TBM_GETPOS, 0, 0);
            _snwprintf(buf, 64, L"%.2fx", (double)pos / 100.0);
            SetDlgItemTextW(hwnd, ID_SPEED + 100, buf);
        }
        return 0;

    case WM_APP_PROG:
        if (!app) break;
        SendMessageW(app->progress, PBM_SETPOS, (WPARAM)wp, 0);
        return 0;

    case WM_APP_DONE: {
        Job *job = (Job *)lp;

        if (!job) {   /* background preload finished */
            set_status(app, L"Ready - the model is loaded and warm.");
            return 0;
        }
        wchar_t msg[MAX_PATHLEN + 128];
        if (!app) break;
        SendMessageW(app->progress, PBM_SETPOS, 100, 0);
        _snwprintf(msg, MAX_PATHLEN + 128,
                   L"Done: %.2f s of audio at %d Hz -> %s",
                   job->seconds, job->sample_rate, job->wav_path);
        set_status(app, msg);
        set_controls_enabled(app, 0);
        play_wav(app);
        free(job);
        return 0;
    }

    case WM_APP_FAIL: {
        const wchar_t *why = (const wchar_t *)lp;
        if (!app) break;
        set_controls_enabled(app, 0);
        SendMessageW(app->progress, PBM_SETPOS, 0, 0);
        set_status(app, L"Failed.");
        MessageBoxW(hwnd, why ? why : L"Unknown error", L"win7-tts", MB_ICONERROR);
        return 0;
    }

    case WM_SIZE:
        if (app) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            layout(app, rc.right, rc.bottom);
        }
        return 0;

    case WM_CLOSE:
        PlaySoundW(NULL, NULL, SND_PURGE);
        if (MessageBoxW(hwnd, L"Close win7-tts?", L"win7-tts",
                        MB_ICONQUESTION | MB_YESNO) == IDYES) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        if (app) {
            if (app->font) DeleteObject(app->font);
            if (app->bold_font) DeleteObject(app->bold_font);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    WNDCLASSEXW wc;
    App app;
    MSG msg;

    (void)prev;
    (void)cmdline;
    memset(&app, 0, sizeof(app));

    /* Common controls for the status bar / progress bar / trackbar. */
    {
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
    }
    /* Unicode + visual styles, so the buttons are not Win95 grey. */
    SetProcessDPIAware();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Win7TtsGui";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"win7-tts  -  local Persian text to speech",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, 780, 560,
                               NULL, NULL, inst, &app);
    if (!app.hwnd) return 1;

    ShowWindow(app.hwnd, show);
    UpdateWindow(app.hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(app.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
