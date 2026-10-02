/*
 * say.exe - fast local Persian (and any Unicode) TTS for Windows 7.
 *
 * Why a wrapper is needed
 * -----------------------
 * sherpa-onnx ships a console program, `sherpa-onnx-offline-tts.exe`, whose
 * text comes from argv.  A plain `main` receives argv in the current ANSI code
 * page.  On a stock Windows 7 that page is 1252 or 437, neither of which can
 * represent Persian, so the engine sees a row of '?' and produces a fraction of
 * a second of near-silence instead of speech.  `chcp 65001` is not a reliable
 * fix: the code page must already be UTF-8 before the process starts, and a
 * program that launches the engine (a shortcut, Task Scheduler, an app) has no
 * console at all.
 *
 * This tool sidesteps argv entirely.  It links sherpa-onnx's C API directly and
 * hands the engine UTF-8, which is what the API documents, so Persian works
 * regardless of the system code page.  It is also faster: the model is loaded
 * once and can synthesise many sentences from one invocation.
 *
 * Usage:
 *   say.exe --model <dir> --out <file.wav> --text "ascii"
 *   say.exe --model <dir> --out <file.wav> --text-file <utf8.txt>
 *   say.exe --model <dir> --out <dir>  --text-file lines.txt --split
 *
 * With --split, --text-file is treated as one sentence per non-empty line and
 * one .wav is written per line, named <n>_<out>.wav next to --out.
 *
 * Options:
 *   --speed <float>     speech rate, 1.0 is normal (default 1.0)
 *   --sid <int>         speaker id for multi-speaker models (default 0)
 *   --threads <int>     backend threads (default 2)
 *   --scale <float>     VITS length scale, >1 is slower (default 1.0)
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sherpa-onnx/c-api/c-api.h"

/*
 * Turn a hard crash into a readable explanation.
 *
 * The engine is a large ONNX runtime; if something goes wrong on an unusual
 * machine the process dies with a Windows error dialog saying "say.exe has
 * stopped working" and no hint about why.  Catching the exception lets us say
 * what actually happened - the commonest cause by far being a CPU that is too
 * old for the runtime's x64 kernels.
 */
static void report_crash(unsigned long code) {
    const char *why;
    switch (code) {
    case 0xC000001DUL:
        why = "This CPU does not support the instruction set the engine needs.\n\n"
              "onnxruntime's x64 CPU kernels require AVX. If this machine's CPU\n"
              "predates 2011 (Sandy Bridge), that is the cause. See the\n"
              "'CPU too old' section of README.md.";
        break;
    case 0xC0000005UL:
        why = "Access violation - the runtime DLLs do not match each other.\n\n"
              "Re-extract the archive so the runtime folder is complete, and make\n"
              "sure w7shim.dll sits next to the .exe and .dll files.";
        break;
    case 0xC000007BUL:
        why = "Bad DLL image - usually a 32-bit DLL on a 64-bit system.\n\n"
              "Check you installed the x64 Visual C++ redistributable.";
        break;
    case 0xC0000135UL:
        why = "A required DLL was not found.\n\n"
              "Install the Visual C++ 2015-2022 x64 redistributable.";
        break;
    case 0xC0000139UL:
        why = "An entry point was not found.\n\n"
              "w7shim.dll is missing or not beside the other runtime files.";
        break;
    case 0xC0000409UL:
        why = "Stack buffer overrun (fail-fast). Try --threads 1.";
        break;
    default:
        why = "The engine terminated unexpectedly.";
        break;
    }
    fprintf(stderr, "\nsay.exe: crashed - exception 0x%08lX\n\n%s\n",
            code, why);
    fflush(stderr);
    ExitProcess(3);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep && ep->ExceptionRecord) {
        report_crash((unsigned long)ep->ExceptionRecord->ExceptionCode);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#define MAX_PATH_LEN 4096

static void die(const char *msg) {
    fprintf(stderr, "say: %s\n", msg);
    exit(1);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* Drop a UTF-8 BOM and trim whitespace from both ends (in place). */
static char *trim_utf8(char *s) {
    char *p = s;
    size_t len = strlen(p);
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    {
        size_t l = strlen(p);
        while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\t' ||
                         p[l - 1] == '\r' || p[l - 1] == '\n')) {
            p[--l] = '\0';
        }
    }
    return p;
}

/* Build "<dir>\<name>" in UTF-8. */
static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    size_t dl = strlen(dir);
    if (dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) {
        snprintf(out, cap, "%s%s", dir, name);
    } else {
        snprintf(out, cap, "%s\\%s", dir, name);
    }
}

int main(int argc, char **argv) {
    const char *model_dir = NULL, *out_path = NULL, *text_arg = NULL, *text_file = NULL;
    float speed = 1.0f, length_scale = 1.0f;
    int sid = 0, num_threads = 2, split = 0;
    int i;
    char *owned = NULL;
    SherpaOnnxOfflineTtsVitsModelConfig vits;
    SherpaOnnxOfflineTtsModelConfig model;
    SherpaOnnxOfflineTtsConfig config;
    SherpaOnnxGenerationConfig gen_cfg;
    const SherpaOnnxOfflineTts *tts;
    char model_onnx[MAX_PATH_LEN], tokens[MAX_PATH_LEN], data_dir[MAX_PATH_LEN];
    char probe[MAX_PATH_LEN], out_dir[MAX_PATH_LEN], out_base[MAX_PATH_LEN];
    int failures = 0, made = 0;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--text") && i + 1 < argc) text_arg = argv[++i];
        else if (!strcmp(argv[i], "--text-file") && i + 1 < argc) text_file = argv[++i];
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sid") && i + 1 < argc) sid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) length_scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--split")) split = 1;
        else {
            fprintf(stderr, "say: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!model_dir || !out_path || (!text_arg && !text_file)) {
        fprintf(stderr,
                "usage: say.exe --model <dir> --out <file.wav> "
                "(--text <text> | --text-file <file.txt>)\n"
                "       add --split to treat each line of the file as a sentence\n");
        return 1;
    }
    if (num_threads < 1) num_threads = 1;

    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)crash_handler);

    /* ---- locate the model files ---------------------------------------- */
    {
        /* Any single .onnx file in the directory is the model. */
        {
            /* Enumerate the directory to find the model file. */
            WIN32_FIND_DATAA fd;
            HANDLE h;
            char pattern[MAX_PATH_LEN];
            int found = 0;
            snprintf(pattern, sizeof(pattern), "%s\\*.onnx", model_dir);
            h = FindFirstFileA(pattern, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        join_path(model_onnx, sizeof(model_onnx), model_dir, fd.cFileName);
                        found = 1;
                        break;
                    }
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            if (!found) {
                fprintf(stderr, "say: no .onnx model found in '%s'\n", model_dir);
                return 1;
            }
        }
    }

    join_path(tokens, sizeof(tokens), model_dir, "tokens.txt");
    if (GetFileAttributesA(tokens) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "say: tokens.txt not found in '%s'\n", model_dir);
        return 1;
    }

    /* espeak-ng-data lives beside the model unless the model ships its own. */
    snprintf(probe, sizeof(probe), "%s\\espeak-ng-data\\phontab", model_dir);
    if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
        snprintf(data_dir, sizeof(data_dir), "%s\\espeak-ng-data", model_dir);
    } else {
        strcpy(data_dir, "espeak-ng-data");
    }

    /* ---- build the engine ------------------------------------------------ */
    memset(&vits, 0, sizeof(vits));
    vits.model = model_onnx;
    vits.lexicon = "";
    vits.tokens = tokens;
    vits.data_dir = data_dir;
    vits.dict_dir = "";
    vits.noise_scale = 0.667f;
    vits.noise_scale_w = 0.8f;
    vits.length_scale = length_scale;

    memset(&model, 0, sizeof(model));
    model.vits = vits;
    model.num_threads = num_threads;
    model.debug = 0;
    model.provider = "cpu";

    memset(&config, 0, sizeof(config));
    config.model = model;
    config.rule_fsts = "";
    config.max_num_sentences = 2;

    fprintf(stderr, "Loading model: %s\n", model_onnx);
    tts = SherpaOnnxCreateOfflineTts(&config);
    if (!tts) die("could not create the TTS engine (bad model path?)");
    fprintf(stderr, "Model ready.\n");

    memset(&gen_cfg, 0, sizeof(gen_cfg));
    gen_cfg.sid = sid;
    gen_cfg.speed = speed;
    gen_cfg.silence_scale = 0.2f;

    /* ---- work out where output files go ---------------------------------- */
    {
        const char *slash = strrchr(out_path, '/');
        const char *bslash = strrchr(out_path, '\\');
        const char *sep = (slash > bslash) ? slash : bslash;
        if (sep) {
            size_t n = (size_t)(sep - out_path);
            if (n == 0) n = 1;
            if (n >= sizeof(out_dir)) n = sizeof(out_dir) - 1;
            memcpy(out_dir, out_path, n);
            out_dir[n] = '\0';
            strncpy(out_base, sep + 1, sizeof(out_base) - 1);
            out_base[sizeof(out_base) - 1] = '\0';
        } else {
            strcpy(out_dir, ".");
            strncpy(out_base, out_path, sizeof(out_base) - 1);
        }
    }

    /* ---- synth: one file, or one file per line --------------------------- */
    if (!split) {
        char *text;
        if (text_file) {
            owned = read_file(text_file);
            if (!owned) {
                fprintf(stderr, "say: cannot read '%s'\n", text_file);
                return 1;
            }
            text = trim_utf8(owned);
        } else {
            /*
             * say.exe has a narrow main(), so argv arrives in the console's ANSI
             * code page (1256 on a Persian Windows 7), *not* UTF-8.  Handing
             * those bytes straight to the engine makes it reject the string
             * with "Non UTF8 encoded string is received" and phonemise
             * garbage.  Convert ANSI -> UTF-16 -> UTF-8 first.
             */
            int wlen = MultiByteToWideChar(CP_ACP, 0, text_arg, -1, NULL, 0);
            wchar_t *wtext;
            int n;
            if (wlen <= 0) die("could not read the --text argument");
            wtext = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
            if (!wtext) die("out of memory");
            MultiByteToWideChar(CP_ACP, 0, text_arg, -1, wtext, wlen);
            n = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, NULL, 0, NULL, NULL);
            if (n <= 0) { free(wtext); die("could not convert --text to UTF-8"); }
            owned = (char *)malloc((size_t)n);
            if (!owned) { free(wtext); die("out of memory"); }
            WideCharToMultiByte(CP_UTF8, 0, wtext, -1, owned, n, NULL, NULL);
            free(wtext);
            text = trim_utf8(owned);
        }
        if (!*text) die("the text to speak is empty");

        {
            const SherpaOnnxGeneratedAudio *audio =
                SherpaOnnxOfflineTtsGenerateWithConfig(tts, text, &gen_cfg, NULL, NULL);
            if (!audio || audio->n <= 0) {
                fprintf(stderr, "say: no audio was produced\n");
                SherpaOnnxDestroyOfflineTts(tts);
                return 1;
            }
            fprintf(stderr, "Generated %.2f s at %d Hz\n",
                    (double)audio->n / (double)audio->sample_rate, audio->sample_rate);
            if (SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, out_path)) {
                fprintf(stderr, "Wrote %s\n", out_path);
                made = 1;
            } else {
                fprintf(stderr, "say: could not write '%s'\n", out_path);
                failures++;
            }
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        }
        free(owned);
    } else {
        char *text = read_file(text_file ? text_file : "");
        char *line, *save;
        int n = 0;
        if (!text) die("cannot read the text file");
        trim_utf8(text);
        for (line = strtok_r(text, "\r\n", &save); line;
             line = strtok_r(NULL, "\r\n", &save)) {
            char one[MAX_PATH_LEN];
            char written[MAX_PATH_LEN];
            char stem[MAX_PATH_LEN];
            const SherpaOnnxGeneratedAudio *audio;
            char *t = trim_utf8(line);
            n++;
            if (!*t) continue;
            audio = SherpaOnnxOfflineTtsGenerateWithConfig(tts, t, &gen_cfg, NULL, NULL);
            if (!audio || audio->n <= 0) {
                fprintf(stderr, "  line %d: FAILED to generate\n", n);
                failures++;
                continue;
            }
            /* strip the extension from --out to build per-line names */
            strncpy(stem, out_base, sizeof(stem) - 1);
            stem[sizeof(stem) - 1] = '\0';
            {
                char *dot = strrchr(stem, '.');
                if (dot) *dot = '\0';
            }
            snprintf(one, sizeof(one), "%d_%s.wav", n, stem);
            join_path(written, sizeof(written), out_dir, one);
            if (SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, written)) {
                fprintf(stderr, "  line %d -> %s (%.2f s)\n", n, written,
                        (double)audio->n / (double)audio->sample_rate);
                made++;
            } else {
                fprintf(stderr, "  line %d: could not write %s\n", n, written);
                failures++;
            }
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        }
        free(text);
    }

    SherpaOnnxDestroyOfflineTts(tts);

    if (made == 0 && failures == 0) fprintf(stderr, "say: nothing to do\n");
    return failures ? 1 : 0;
}
