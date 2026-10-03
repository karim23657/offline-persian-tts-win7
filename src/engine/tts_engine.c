#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>

#include "tts_engine.h"
#include "sherpa-onnx/c-api/c-api.h"

#define MAX_PATHLEN 4096

struct TtsEngine {
    const SherpaOnnxOfflineTts *tts;
    wchar_t model_dir[MAX_PATHLEN];
    DWORD last_used;          /* tick count for LRU */
    int sample_rate;
    int reference;            /* kept resident regardless of LRU pressure */
};

/* One cache for the whole process; the GUI and the server each have one. */
static struct {
    TtsEngine slots[MAX_CACHED_ENGINES];
    int count;
    CRITICAL_SECTION cs;      /* guards the table AND serialises generation */
    CRITICAL_SECTION init_lock;
    LONG ready;
} g_cache;

static DWORD g_tick = 0;

void TtsEngineInit(void) {
    if (InterlockedCompareExchange(&g_cache.ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_cache.cs);
        InitializeCriticalSection(&g_cache.init_lock);
        memset(g_cache.slots, 0, sizeof(g_cache.slots));
        g_cache.count = 0;
    }
}

/* Find the single .onnx in a model directory. Returns 0 on success. */
static int find_model_file(const wchar_t *dir, wchar_t *out, size_t cap) {
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pattern[MAX_PATHLEN];
    int found = 0;

    _snwprintf(pattern, MAX_PATHLEN, L"%s\\*.onnx", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                _snwprintf(out, cap, L"%s\\%s", dir, fd.cFileName);
                found = 1;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return found ? 0 : -1;
}

static int has_tokens(const wchar_t *dir) {
    wchar_t p[MAX_PATHLEN];
    _snwprintf(p, MAX_PATHLEN, L"%s\\tokens.txt", dir);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* Locate espeak-ng-data: beside the model, else next to the executable. */
static int find_data_dir(const wchar_t *model_dir, wchar_t *out, size_t cap) {
    wchar_t probe[MAX_PATHLEN];
    HMODULE self = GetModuleHandleW(NULL);

    _snwprintf(probe, MAX_PATHLEN, L"%s\\espeak-ng-data\\phontab", model_dir);
    if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
        _snwprintf(out, cap, L"%s\\espeak-ng-data", model_dir);
        return 1;
    }
    /* ../models/espeak-ng-data, relative to the executable's folder */
    if (self) {
        wchar_t exe[MAX_PATHLEN];
        wchar_t *slash;
        GetModuleFileNameW(self, exe, MAX_PATHLEN);
        slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = L'\0';
            _snwprintf(probe, MAX_PATHLEN, L"%s\\..\\models\\espeak-ng-data\\phontab", exe);
            if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
                _snwprintf(out, cap, L"%s\\..\\models\\espeak-ng-data", exe);
                return 1;
            }
        }
    }
    wcscpy(out, L"espeak-ng-data");
    return 0;
}

static const SherpaOnnxOfflineTts *create_engine(const wchar_t *model_dir, float length_scale,
                                                 int *sample_rate, wchar_t *error,
                                                 size_t errlen) {
    wchar_t model_onnx[MAX_PATHLEN], tokens[MAX_PATHLEN], data_dir[MAX_PATHLEN];
    char n_model[MAX_PATHLEN], n_tokens[MAX_PATHLEN], n_data[MAX_PATHLEN];
    SherpaOnnxOfflineTtsVitsModelConfig vits;
    SherpaOnnxOfflineTtsModelConfig model;
    SherpaOnnxOfflineTtsConfig config;
    const SherpaOnnxOfflineTts *tts;
    SYSTEM_INFO si;
    int threads;

    if (find_model_file(model_dir, model_onnx, MAX_PATHLEN) != 0) {
        _snwprintf(error, errlen, L"no .onnx model file found in this folder");
        return NULL;
    }
    if (!has_tokens(model_dir)) {
        _snwprintf(error, errlen, L"tokens.txt is missing from this folder");
        return NULL;
    }
    find_data_dir(model_dir, data_dir, MAX_PATHLEN);

    if (!WideCharToMultiByte(CP_UTF8, 0, model_onnx, -1, n_model, MAX_PATHLEN, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, tokens, -1, n_tokens, MAX_PATHLEN, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, data_dir, -1, n_data, MAX_PATHLEN, NULL, NULL)) {
        _snwprintf(error, errlen, L"could not convert the model paths to UTF-8");
        return NULL;
    }

    /* Cap threads: ORT scales badly past the physical core count, and this
     * runs alongside a browser or GUI on a machine that may only have 2-4. */
    GetSystemInfo(&si);
    threads = (int)si.dwNumberOfProcessors;
    if (threads > 8) threads = 8;
    if (threads < 1) threads = 1;

    memset(&vits, 0, sizeof(vits));
    vits.model = n_model;
    vits.lexicon = "";
    vits.tokens = n_tokens;
    vits.data_dir = n_data;
    vits.dict_dir = "";
    vits.noise_scale = 0.667f;
    vits.noise_scale_w = 0.8f;
    vits.length_scale = length_scale;

    memset(&model, 0, sizeof(model));
    model.vits = vits;
    model.num_threads = threads;
    model.debug = 0;
    model.provider = "cpu";

    memset(&config, 0, sizeof(config));
    config.model = model;
    config.rule_fsts = "";
    config.max_num_sentences = 2;

    tts = SherpaOnnxCreateOfflineTts(&config);
    if (!tts) {
        _snwprintf(error, errlen, L"the engine rejected this model (see stderr for detail)");
        return NULL;
    }

    /*
     * Warm-up.  Discarding the audio forces ORT to allocate its arenas and pick
     * kernels now rather than during the user's first request.  It also gives
     * us the sample rate, which the streaming WAV writer needs up front.
     */
    {
        SherpaOnnxGenerationConfig gc;
        const SherpaOnnxGeneratedAudio *warm;
        memset(&gc, 0, sizeof(gc));
        gc.sid = 0;
        gc.speed = 1.0f;
        gc.silence_scale = 0.2f;
        warm = SherpaOnnxOfflineTtsGenerateWithConfig(tts, "a", &gc, NULL, NULL);
        if (warm) {
            if (sample_rate) *sample_rate = warm->sample_rate;
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(warm);
        }
    }
    return tts;
}

/* Evict the least recently used non-pinned engine. Caller holds the lock. */
static void evict_lru_locked(void) {
    int i, victim = -1;
    DWORD oldest = 0xFFFFFFFFu;
    for (i = 0; i < MAX_CACHED_ENGINES; ++i) {
        if (!g_cache.slots[i].tts) continue;
        if (g_cache.slots[i].reference) continue;
        if (g_cache.slots[i].last_used < oldest) {
            oldest = g_cache.slots[i].last_used;
            victim = i;
        }
    }
    if (victim >= 0) {
        SherpaOnnxDestroyOfflineTts(g_cache.slots[victim].tts);
        memset(&g_cache.slots[victim], 0, sizeof(g_cache.slots[victim]));
        if (g_cache.count > 0) g_cache.count--;
    }
}

TtsEngine *TtsEngineAcquire(const wchar_t *model_dir, wchar_t *error, size_t errlen) {
    int i, free_slot = -1;
    TtsEngine *e;

    TtsEngineInit();
    EnterCriticalSection(&g_cache.cs);

    /* Reuse if already loaded - this is the whole point of the cache. */
    for (i = 0; i < MAX_CACHED_ENGINES; ++i) {
        if (g_cache.slots[i].tts &&
            _wcsicmp(g_cache.slots[i].model_dir, model_dir) == 0) {
            e = &g_cache.slots[i];
            e->last_used = ++g_tick;
            return e;   /* lock still held: it serialises generation */
        }
    }

    for (i = 0; i < MAX_CACHED_ENGINES; ++i) {
        if (!g_cache.slots[i].tts) { free_slot = i; break; }
    }
    if (free_slot < 0) {
        evict_lru_locked();
        for (i = 0; i < MAX_CACHED_ENGINES; ++i) {
            if (!g_cache.slots[i].tts) { free_slot = i; break; }
        }
    }
    if (free_slot < 0) {
        LeaveCriticalSection(&g_cache.cs);
        _snwprintf(error, errlen, L"too many models cached at once");
        return NULL;
    }

    LeaveCriticalSection(&g_cache.cs);   /* loading is slow; do not block others */

    {
        TtsEngine tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.tts = create_engine(model_dir, 1.0f, &tmp.sample_rate, error, errlen);
        if (!tmp.tts) return NULL;
        wcscpy(tmp.model_dir, model_dir);
        tmp.last_used = ++g_tick;
        tmp.reference = 1;   /* protect the model we are about to hand out */

        EnterCriticalSection(&g_cache.cs);
        g_cache.slots[free_slot] = tmp;
        g_cache.count++;
        e = &g_cache.slots[free_slot];
    }
    return e;
}

void TtsEngineTouch(TtsEngine *engine) {
    if (!engine) return;
    EnterCriticalSection(&g_cache.cs);
    engine->last_used = ++g_tick;
    engine->reference = 0;
    LeaveCriticalSection(&g_cache.cs);
}

void TtsEngineRelease(void) {
    LeaveCriticalSection(&g_cache.cs);
}

int TtsEngineResidentCount(void) {
    int n = 0, i;
    EnterCriticalSection(&g_cache.cs);
    for (i = 0; i < MAX_CACHED_ENGINES; ++i) {
        if (g_cache.slots[i].tts) n++;
    }
    LeaveCriticalSection(&g_cache.cs);
    return n;
}

int TtsEngineIsLoaded(const wchar_t *model_dir) {
    int i, found = 0;
    if (!g_cache.ready) return 0;
    EnterCriticalSection(&g_cache.cs);
    for (i = 0; i < MAX_CACHED_ENGINES; ++i) {
        if (g_cache.slots[i].tts && _wcsicmp(g_cache.slots[i].model_dir, model_dir) == 0) {
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_cache.cs);
    return found;
}

int TtsEngineSampleRate(const wchar_t *model_dir) {
    int i, rate = 0;
    if (!g_cache.ready) return 0;
    EnterCriticalSection(&g_cache.cs);
    for (i = 0; i < MAX_CACHED_ENGINES; ++i) {
        if (g_cache.slots[i].tts && _wcsicmp(g_cache.slots[i].model_dir, model_dir) == 0) {
            rate = g_cache.slots[i].sample_rate;
            break;
        }
    }
    LeaveCriticalSection(&g_cache.cs);
    return rate;
}

/* Context for the sherpa-onnx callback trampoline. */
typedef struct {
    TtsSampleSink sink;
    void *user;
    int called;          /* did the engine invoke the callback at all? */
} GenCtx;

/*
 * Progress callback.  The signature has FOUR parameters - samples, count,
 * progress and the user pointer.  A three-argument function compiles with only
 * a warning here and then misbehaves, because the progress float is read from
 * where the argument pointer should be.
 */
static int32_t WINAPI engine_on_samples(const float *samples, int32_t n,
                                        float progress, void *arg) {
    GenCtx *ctx = (GenCtx *)arg;
    if (ctx) {
        ctx->called = 1;
        if (ctx->sink) ctx->sink(samples, (int)n, progress, ctx->user);
    }
    return 1;
}

int TtsEngineGenerate(TtsEngine *engine, const char *text, float speed, int sid,
                      TtsSampleSink sink, void *user, int *out_samples,
                      char *err, size_t errlen) {
    SherpaOnnxGenerationConfig cfg;
    const SherpaOnnxGeneratedAudio *audio;
    GenCtx ctx;
    int rc = 0;

    if (!engine || !engine->tts) {
        snprintf(err, errlen, "engine is not loaded");
        return 0;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.sid = sid;
    cfg.speed = speed;
    cfg.silence_scale = 0.2f;

    ctx.sink = sink;
    ctx.user = user;
    ctx.called = 0;

    audio = SherpaOnnxOfflineTtsGenerateWithConfig(engine->tts, text, &cfg,
                                                  engine_on_samples, &ctx);
    if (!audio || audio->n <= 0) {
        snprintf(err, errlen, "the model produced no audio for this text");
        if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        return 0;
    }
    /*
     * If the engine never invoked the callback, push the whole buffer so the
     * caller still gets audio.  Guarding on `called` matters: pushing
     * unconditionally would append the same samples twice.
     */
    if (sink && audio->n > 0 && !ctx.called) {
        sink(audio->samples, (int)audio->n, 1.0f, user);
    }
    if (out_samples) *out_samples = audio->n;
    if (getenv("WIN7TTS_DEBUG")) {
        fprintf(stderr, "    engine produced %d samples, callback %s\n",
                audio->n, ctx.called ? "fired" : "never fired");
    }
    rc = 1;
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    return rc;
}

void TtsEngineShutdown(void) {
    int i;
    if (!g_cache.ready) return;
    EnterCriticalSection(&g_cache.cs);
    for (i = 0; i < MAX_CACHED_ENGINES; ++i) {
        if (g_cache.slots[i].tts) {
            SherpaOnnxDestroyOfflineTts(g_cache.slots[i].tts);
            memset(&g_cache.slots[i], 0, sizeof(g_cache.slots[i]));
        }
    }
    g_cache.count = 0;
    LeaveCriticalSection(&g_cache.cs);
}
