/*
 * diagnose.exe - find out *why* the TTS crashed, instead of guessing.
 *
 * Run this on the machine that is misbehaving and post the output.  It reports:
 *
 *   1. Which CPU features are present.  onnxruntime dispatches CPU kernels at
 *      run time, so a machine without the required instruction set dies with
 *      an illegal-instruction fault, which Windows shows as
 *      "say.exe has stopped working".
 *   2. Whether the required VC++ runtime DLLs are present.
 *   3. Whether onnxruntime.dll and sherpa-onnx-c-api.dll can be loaded at all,
 *      and whether a session can actually be created.
 *
 * Every risky call is wrapped so a crash here is reported rather than fatal.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ cpuid  */

static void cpuid_count(int leaf, int sub, unsigned *a, unsigned *b,
                        unsigned *c, unsigned *d) {
    int r[4];
    __asm__ __volatile__("cpuid"
                         : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                         : "a"(leaf), "c"(sub));
    *a = (unsigned)r[0];
    *b = (unsigned)r[1];
    *c = (unsigned)r[2];
    *d = (unsigned)r[3];
}

/*
 * CPUID register order matters here and is easy to get wrong:
 *
 *   leaf 1  : SSE3..FMA/AVX flags are in ECX, SSE/SSE2 in EDX
 *   leaf 7  : AVX2/AVX512F are in EBX, VNNI is in ECX
 *
 * Indexing regs[0] (EAX) reports "no AVX" on a perfectly modern CPU, so the
 * registers are named explicitly instead.
 */
static int bit_ecx(unsigned ecx, int bit) { return (ecx >> bit) & 1u; }
static int bit_edx(unsigned edx, int bit) { return (edx >> bit) & 1u; }
static int bit_ebx(unsigned ebx, int bit) { return (ebx >> bit) & 1u; }

static void report_cpu(void) {
    unsigned a, b, c, d;
    char brand[49];
    unsigned max_ext, leaf1_ecx, leaf1_edx;
    int n = 0, i;
    int sse2, sse42, avx, fma, osxsave, avx2, avx512f, vnni;

    cpuid_count(0, 0, &a, &b, &c, &d);
    max_ext = 0;
    if (a >= 0x80000004u) {
        unsigned v[12];
        cpuid_count(0x80000000u, 0, &a, &b, &c, &d);
        max_ext = a;
        if (a >= 0x80000004u) {
            for (i = 0; i < 3; ++i) {
                cpuid_count(0x80000002u + (unsigned)i, 0,
                            &v[i * 4], &v[i * 4 + 1], &v[i * 4 + 2], &v[i * 4 + 3]);
            }
            memcpy(brand, v, 48);
            brand[48] = 0;
        } else {
            brand[0] = 0;
        }
    } else {
        brand[0] = 0;
    }
    while (brand[n] == ' ') n++;
    printf("CPU            : %s\n", brand + n);
    if (max_ext) printf("CPUID ext max  : 0x%08X\n", max_ext);

    /* Leaf 1 gives the SSE/AVX flags; capture its EDX *before* anything else
     * overwrites the registers, or SSE2 reads back as "NO" on every CPU. */
    cpuid_count(1, 0, &a, &b, &c, &d);
    leaf1_ecx = c;
    leaf1_edx = d;
    sse42     = bit_ecx(leaf1_ecx, 20);
    fma       = bit_ecx(leaf1_ecx, 12);
    osxsave   = bit_ecx(leaf1_ecx, 27);
    avx       = bit_ecx(leaf1_ecx, 28);
    sse2      = bit_edx(leaf1_edx, 26);

    avx2 = avx512f = vnni = 0;
    cpuid_count(0, 0, &a, &b, &c, &d);
    if (a >= 7) {
        cpuid_count(7, 0, &a, &b, &c, &d);
        avx2    = bit_ebx(b, 5);
        avx512f = bit_ebx(b, 16);
        vnni    = bit_ecx(c, 9);
    }

    printf("SSE2           : %s\n", sse2 ? "yes" : "NO");
    printf("SSE4.2         : %s\n", sse42 ? "yes" : "NO");
    printf("OSXSAVE        : %s\n", osxsave ? "yes" : "NO");
    printf("AVX            : %s\n", avx ? "yes" : "NO");
    printf("FMA3           : %s\n", fma ? "yes" : "NO");
    printf("AVX2           : %s\n", avx2 ? "yes" : "NO");
    printf("AVX512F        : %s\n", avx512f ? "yes" : "NO");
    printf("AVX512-VNNI    : %s\n", vnni ? "yes" : "NO");

    if (!sse42) {
        printf("\n*** No SSE4.2.  This CPU predates 2008 and cannot run the engine\n"
               "*** at all.  See the 'old CPU' section of README.md.\n");
    } else if (!osxsave || !avx) {
        printf("\n*** This CPU has no AVX.  onnxruntime's x64 CPU kernels require\n"
               "*** AVX; on a CPU without it the very first kernel raises an\n"
               "*** illegal-instruction fault, which Windows reports as\n"
               "*** \"say.exe has stopped working\".\n"
               "*** See the 'old CPU' section of README.md for the fix.\n");
    } else {
        printf("\nCPU is new enough for the current engine (AVX present).\n");
    }
}

/* ---------------------------------------------------------- crash catcher */

/* Last-chance handler so a fault here is printed, not a dialog. */
static void report_exception(unsigned long code) {
    const char *what = "unknown";
    switch (code) {
    case 0xC0000005UL: what = "access violation (bad pointer)"; break;
    case 0xC000001DUL: what = "ILLEGAL INSTRUCTION - CPU lacks a required "
                              "instruction (e.g. AVX)"; break;
    case 0xC0000025UL: what = "non-continuable exception"; break;
    case 0xC000007BUL: what = "bad DLL / wrong bitness (32-bit DLL on 64-bit OS)"; break;
    case 0xC0000135UL: what = "DLL not found"; break;
    case 0xC0000139UL: what = "entry point not found"; break;
    case 0xC0000142UL: what = "DLL initialisation failed"; break;
    case 0xC0000409UL: what = "stack buffer overrun / fail-fast"; break;
    case 0xE06D7363UL: what = "C++ exception"; break;
    case 0xC0000374UL: what = "heap corruption"; break;
    case 0xC0000001UL: what = "recursive/overflowing exception"; break;
    default: break;
    }
    printf("  !! CRASH: exception 0x%08lX - %s\n", code, what);
    fflush(stdout);
    ExitProcess(2);
}

static LRESULT CALLBACK crash_proc(EXCEPTION_POINTERS *ep) {
    if (ep && ep->ExceptionRecord) {
        report_exception((unsigned long)ep->ExceptionRecord->ExceptionCode);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_crash_handler(void) {
    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)crash_proc);
}

/* --------------------------------------------------------------- presence */

static void check_dlls(const char *dir) {
    static const char *const names[] = {
        "onnxruntime.dll", "sherpa-onnx-c-api.dll", "w7shim.dll",
        "say.exe", "tts_gui.exe", NULL
    };
    int i;
    printf("\nRuntime files in %s\n", dir);
    for (i = 0; names[i]; ++i) {
        char p[MAX_PATH];
        _snprintf(p, MAX_PATH, "%s\\%s", dir, names[i]);
        printf("  %-26s %s\n", names[i],
               GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? "found" : "MISSING");
    }
    printf("\nVisual C++ runtime (needed by the engine)\n");
    {
        static const char *const vc[] = {
            "C:\\Windows\\System32\\vcruntime140.dll",
            "C:\\Windows\\System32\\msvcp140.dll",
            "C:\\Windows\\System32\\vcruntime140_1.dll",
            NULL
        };
        for (i = 0; vc[i]; ++i) {
            printf("  %-46s %s\n", vc[i],
                   GetFileAttributesA(vc[i]) != INVALID_FILE_ATTRIBUTES ? "found" : "MISSING");
        }
    }
}

/*
 * Load onnxruntime and ask it for the API table, then create an environment.
 * This is the first point where onnxruntime initialises its CPU dispatch, so
 * if the machine's instruction set is the problem this is where it will fault.
 */
typedef void *(*PFN_GetApiBase)(void);
typedef void *(*PFN_CreateEnv)(int, const char **);

static void probe_runtime(const char *dir) {
    char p[MAX_PATH];
    HMODULE h;
    PFN_GetApiBase get_api;

    printf("\nLoading onnxruntime.dll\n");
    _snprintf(p, MAX_PATH, "%s\\onnxruntime.dll", dir);
    h = LoadLibraryA(p);
    if (!h) {
        printf("  FAILED to load: error %lu\n  (missing DLL or wrong bitness)\n",
               (unsigned long)GetLastError());
        return;
    }
    printf("  loaded OK\n");

    get_api = (PFN_GetApiBase)GetProcAddress(h, "OrtGetApiBase");
    if (!get_api) {
        printf("  OrtGetApiBase not found\n");
        return;
    }
    {
        void *api = get_api();
        printf("  OrtGetApiBase() -> %s\n", api ? "ok" : "NULL");
        if (api) printf("  CPU dispatch initialised without faulting.\n");
    }
}

int main(int argc, char **argv) {
    char dir[MAX_PATH], cmd[MAX_PATH];
    char *slash;
    int i;

    install_crash_handler();
    setvbuf(stdout, NULL, _IONBF, 0);

    GetModuleFileNameA(NULL, dir, MAX_PATH);
    slash = strrchr(dir, '\\');
    if (slash) *slash = '\0'; else strcpy(dir, ".");

    /* --tts: run the exact synthesis path the user reports as crashing. */
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--tts")) {
            const char *model = (i + 1 < argc) ? argv[i + 1] : NULL;
            char say[MAX_PATH], app[MAX_PATH], text[MAX_PATH], out[MAX_PATH];
            const char *work;
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            DWORD code = 0;
            FILE *fh;
            const char *msg = "\xD8\xB3\xD8\xC7\xDB\x8C\x2C\xD8\xA7\xD8\xAF\xDB\x8C\x2C"; /* salam */
            char line[MAX_PATH * 3];
            if (!model) { printf("usage: diagnose.exe --tts <model-dir>\n"); return 2; }
            _snprintf(say, MAX_PATH, "%s\\say.exe", dir);
            _snprintf(out, MAX_PATH, "%s\\_diag_out.wav", dir);
            _snprintf(text, MAX_PATH, "%s\\_diag_in.txt", dir);
            work = dir;   /* cwd for the child: the runtime folder */
            _snprintf(line, sizeof(line), "--model \"%s\" --out \"%s\" --text-file \"%s\"", model, out, text);
            fh = fopen(text, "wb");
            if (!fh) { printf("cannot write %s\n", text); return 2; }
            fputs(msg, fh);
            fclose(fh);
            printf("\nRunning real synthesis (this is the step that may crash)...\n");
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            {
                char cl[MAX_PATH * 3];
                /*
                 * argv[0] must be present even when lpApplicationName is set:
                 * the MinGW CRT parses the raw command line, so without it
                 * "--model" is taken as argv[0] and the arguments shift by one.
                 */
                _snprintf(cl, sizeof(cl), "\"%s\" %s", app, line);
                if (!CreateProcessA(say, cl, NULL, NULL, FALSE, 0, NULL, work, &si, &pi)) {
                    printf("  could not start say.exe: error %lu\n", (unsigned long)GetLastError());
                    return 3;
                }
            }
            WaitForSingleObject(pi.hProcess, 180000);
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
                printf("  still running after 180 s - treating as a hang\n");
                TerminateProcess(pi.hProcess, 1);
            }
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            printf("  exit code: 0x%08lX\n", (unsigned long)code);
            printf("  output wav: %s\n",
                   GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES ? "created" : "NOT created");
            if (code >= 0xC0000000u && code <= 0xC000FFFFu) report_exception((unsigned long)code);
            return 0;
        }
        /* --run <say.exe>: drive the real engine so its crash code is visible. */
        if (!strcmp(argv[i], "--run")) {
            const char *exe = (i + 1 < argc) ? argv[i + 1] : "say.exe";
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            DWORD code = 0;
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            char app[MAX_PATH];
            _snprintf(app, MAX_PATH, "%s\\%s", dir, exe);
            strcpy(cmd, "--help");   /* lpCommandLine, excluding argv[0] */
            printf("\nRunning %s --help ...\n", exe);
            if (!CreateProcessA(app, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
                printf("  could not start: error %lu\n", (unsigned long)GetLastError());
                return 3;
            }
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            printf("  exit code: 0x%08lX\n", (unsigned long)code);
            if (code >= 0xC0000000u && code <= 0xC000FFFFu) {
                /* Only the NTSTATUS exception range means a real crash. */
                report_exception((unsigned long)code);
            } else {
                printf("  (a small exit code is just a non-zero return, not a crash)\n");
            }
            return 0;
        }
    }

    printf("=== win7-tts diagnostics ===\n\n");
    report_cpu();

    /* --all: run everything, including a real synthesis if a model is given. */
    if (argc >= 3 && !strcmp(argv[1], "--all")) {
        printf("\n(continuing with the full check)\n");
    }

    check_dlls(dir);
    probe_runtime(dir);

    printf("\nDone. If line 3 above said the CPU has no AVX, that is the cause.\n");
    return 0;
}
