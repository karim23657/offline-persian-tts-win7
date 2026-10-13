/*
 * tts_server.exe - a local web interface and JSON API for win7-tts.
 *
 * Why a native server rather than Flask
 * -------------------------------------
 * Windows 7 cannot run anything newer than Python 3.8, so a Flask deployment
 * means pinning the whole stack (Werkzeug, Jinja, ...) to versions that still
 * support 3.8 - real friction on the machines this package targets.  The
 * engine is already C, so a Python wrapper would only add a bridge.
 *
 * What makes it fast
 * ------------------
 * The engine cache in tts_engine.c keeps models resident between requests and
 * warms each one up on load, so:
 *
 *   request 1 : ~1-2 s   (load + warm-up, once)
 *   request 2+: ~0.3 s   for a short sentence
 *
 * Routes
 * ------
 *   GET  /                 the single-page UI
 *   GET  /api/health       status, uptime, how many models are resident
 *   GET  /api/models       installed models, with a "loaded" flag
 *   GET  /api/catalog      models.json - everything available to download
 *   GET  /api/help         this list, as plain text
 *   GET  /api/dict         the pronunciation dictionary (user.dict)
 *   POST /api/dict         add {"word":..,"repl":..} or {"word":..,"delete":true}
 *   POST /api/warmup       load a model now and return how long it took
 *   POST /api/unload       free resident models: {} = all, {"keep":m} = all but
 *                          m, {"model":m} = just m (used on pagehide)
 *   POST /api/synthesize   JSON in, WAV out
 *   GET  /api/synthesize   same, but query parameters (handy for <audio src>)
 *
 * Connections are HTTP/1.1 with keep-alive: one socket serves a whole browser
 * session (bounded at 200 requests or 15 s idle), instead of one TCP
 * handshake per click.
 *
 * Security
 * --------
 * Binds 127.0.0.1 only by default and has no authentication.  --host can widen
 * it, which prints a warning: anyone who can reach the port can make the
 * machine speak.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tts_engine.h"
#include "dict.h"
#include "http_min.h"
#include "json_min.h"
#include "wav_writer.h"
#include "ui_html.h"

#define MAX_PATHLEN 4096
#define MAX_ARGS    16

static int   g_port = 8756;
static char  g_host[64] = "127.0.0.1";
static wchar_t g_models_dir[MAX_PATHLEN];
static int   g_preload = 0;
static int   g_quiet = 0;
static DWORD g_start = 0;
static volatile LONG g_requests = 0;

static void logf_(const char *fmt, ...) {
    va_list ap;
    if (g_quiet) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------ model list */

static int model_is_valid(const wchar_t *dir) {
    wchar_t pattern[MAX_PATHLEN];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int has_onnx = 0;
    wchar_t tokens[MAX_PATHLEN];

    _snwprintf(tokens, MAX_PATHLEN, L"%s\\tokens.txt", dir);
    if (GetFileAttributesW(tokens) == INVALID_FILE_ATTRIBUTES) return 0;

    _snwprintf(pattern, MAX_PATHLEN, L"%s\\*.onnx", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { has_onnx = 1; break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return has_onnx;
}

static int model_count(void) {
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATHLEN];
    HANDLE h;
    int n = 0;

    _snwprintf(pattern, MAX_PATHLEN, L"%s\\*", g_models_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
                wchar_t dir[MAX_PATHLEN];
                _snwprintf(dir, MAX_PATHLEN, L"%s\\%s", g_models_dir, fd.cFileName);
                if (model_is_valid(dir)) n++;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* Pick a model by name (the folder name). Falls back to the only/first model. */
static int resolve_model(const char *want, wchar_t *out, size_t cap) {
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATHLEN];
    HANDLE h;
    int fallback_set = 0;
    wchar_t fallback[MAX_PATHLEN];

    /* Exact folder-name match first, so a typo does not silently fall back. */
    if (want && *want) {
        wchar_t wname[512];
        int n = MultiByteToWideChar(CP_UTF8, 0, want, -1, wname, 512);
        if (n > 0) {
            _snwprintf(pattern, MAX_PATHLEN, L"%s\\%s", g_models_dir, wname);
            if (model_is_valid(pattern)) {
                wcscpy(out, pattern);
                return 1;
            }
        }
    }

    _snwprintf(pattern, MAX_PATHLEN, L"%s\\*", g_models_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t dir[MAX_PATHLEN];
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf(dir, MAX_PATHLEN, L"%s\\%s", g_models_dir, fd.cFileName);
            if (model_is_valid(dir)) {
                if (!fallback_set) {
                    wcscpy(fallback, dir);
                    fallback_set = 1;
                }
                if (want && *want) {
                    wchar_t wname[512];
                    int n = MultiByteToWideChar(CP_UTF8, 0, want, -1, wname, 512);
                    if (n > 0 && _wcsicmp(wname, fd.cFileName) == 0) {
                        wcscpy(out, dir);
                        FindClose(h);
                        return 1;
                    }
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (fallback_set) {
        wcscpy(out, fallback);
        return 1;
    }
    return 0;
}

/* The pronunciation dictionary lives next to models.json, i.e. at the package
   root (<models dir>\..\user.dict), so GUI, server and CLI share one file. */
static void user_dict_path(wchar_t *out, size_t cap) {
    _snwprintf(out, cap, L"%s\\..\\user.dict", g_models_dir);
}

/* --------------------------------------------------------------- synthesis */

typedef struct {
    WavWriter wav;
    int sample_rate;
} SynthCtx;

static void on_samples(const float *samples, int count, float progress, void *user) {
    SynthCtx *ctx = (SynthCtx *)user;
    (void)progress;
    wav_push_samples(&ctx->wav, samples, (size_t)count);
}

/* Build the WAV for `text` and return it, or NULL with an error message. */
static unsigned char *synthesize(const char *text, const wchar_t *model_dir,
                                 float speed, int sid, size_t *out_len,
                                 char *error, size_t errlen) {
    TtsEngine *engine;
    wchar_t werr[512];
    SynthCtx ctx;
    const unsigned char *hdr;
    size_t hdr_len = 0;
    unsigned char *result;
    int n_samples = 0;

    engine = TtsEngineAcquire(model_dir, werr, sizeof(werr) / sizeof(werr[0]));
    if (!engine) {
        WideCharToMultiByte(CP_UTF8, 0, werr, -1, error, (int)errlen, NULL, NULL);
        return NULL;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.sample_rate = TtsEngineSampleRate(model_dir);
    if (ctx.sample_rate <= 0) ctx.sample_rate = 22050;
    wav_init(&ctx.wav, ctx.sample_rate);

    if (!TtsEngineGenerate(engine, text, speed, sid, on_samples, &ctx,
                           &n_samples, error, errlen)) {
        wav_free(&ctx.wav);
        TtsEngineTouch(engine);
        TtsEngineRelease();
        return NULL;
    }
    TtsEngineTouch(engine);
    TtsEngineRelease();   /* release the generation lock Acquire took */

    if (ctx.wav.len == 0) {
        snprintf(error, errlen, "no audio samples were produced");
        wav_free(&ctx.wav);
        return NULL;
    }

    /* Header last: it now carries the exact data size. */
    hdr = wav_header(&ctx.wav, &hdr_len);
    result = (unsigned char *)malloc(hdr_len + ctx.wav.len);
    if (result) {
        memcpy(result, hdr, hdr_len);
        memcpy(result + hdr_len, ctx.wav.buf, ctx.wav.len);
        *out_len = hdr_len + ctx.wav.len;
    } else {
        snprintf(error, errlen, "out of memory building the response");
    }
    wav_free(&ctx.wav);
    return result;
}

/* Percent-decode a query-string value, with '+' meaning space. */
static int url_decode(const char *in, char *out, int out_size) {
    int n = 0;
    while (*in && n + 1 < out_size) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = in[1], lo = in[2], v;
            hi = (hi >= '0' && hi <= '9') ? hi - '0' : (hi | 32) - 'a' + 10;
            lo = (lo >= '0' && lo <= '9') ? lo - '0' : (lo | 32) - 'a' + 10;
            if (hi < 0 || hi > 15 || lo < 0 || lo > 15) break;
            v = hi * 16 + lo;
            out[n++] = (char)v;
            in += 3;
            continue;
        }
        out[n++] = (*in == '+') ? ' ' : *in;
        in++;
    }
    out[n] = 0;
    return n;
}

/* ----------------------------------------------------------------- routes */

static void send_json_error(HttpConn *conn, int status, const char *msg) {
    char esc[1024];
    char body[1200];
    json_escape(msg, esc, sizeof(esc));
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", esc);
    http_send_full(conn, status, "application/json; charset=utf-8", body, strlen(body));
}

static void handle_models(HttpConn *conn) {
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATHLEN];
    HANDLE h;
    char *body;
    size_t cap = 8192, len = 0;
    int first = 1;

    body = (char *)malloc(cap);
    if (!body) { send_json_error(conn, 500, "out of memory"); return; }
    len += (size_t)snprintf(body + len, cap - len, "{\"models\":[");

    _snwprintf(pattern, MAX_PATHLEN, L"%s\\*", g_models_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                wchar_t dir[MAX_PATHLEN];
                char name[512], esc[1024];
                int n;
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                _snwprintf(dir, MAX_PATHLEN, L"%s\\%s", g_models_dir, fd.cFileName);
                if (!model_is_valid(dir)) continue;
                n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name),
                                        NULL, NULL);
                if (n <= 0) continue;
                json_escape(name, esc, sizeof(esc));
                if (len + strlen(esc) + 64 > cap) break;
                len += (size_t)snprintf(body + len, cap - len,
                                        "%s{\"name\":\"%s\",\"loaded\":%s}",
                                        first ? "" : ",", esc,
                                        TtsEngineIsLoaded(dir) ? "true" : "false");
                first = 0;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    snprintf(body + len, cap - len, "]}");
    http_send_full(conn, 200, "application/json; charset=utf-8", body, strlen(body));
    free(body);
}

static void handle_health(HttpConn *conn) {
    char body[512];
    DWORD uptime = (GetTickCount() - g_start) / 1000;
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"uptime_s\":%lu,\"models_installed\":%d,"
             "\"models_loaded\":%d,\"requests\":%ld,\"port\":%d}",
             (unsigned long)uptime, model_count(), TtsEngineResidentCount(),
             (long)InterlockedIncrement(&g_requests) - 1, g_port);
    http_send_full(conn, 200, "application/json; charset=utf-8", body, strlen(body));
}

static void handle_warmup(HttpConn *conn, const HttpRequest *req) {
    char want[512] = "";
    wchar_t model_dir[MAX_PATHLEN];
    wchar_t werr[512];
    TtsEngine *engine;
    DWORD t0, ms;
    char body[512];

    json_get_string(req->body ? req->body : "{}", "model", want, sizeof(want));
    if (!resolve_model(want, model_dir, MAX_PATHLEN)) {
        send_json_error(conn, 404, "no model found; run scripts\\get_model.cmd gyro");
        return;
    }
    t0 = GetTickCount();
    engine = TtsEngineAcquire(model_dir, werr, sizeof(werr) / sizeof(werr[0]));
    ms = GetTickCount() - t0;
    if (!engine) {
        char utf8[512];
        WideCharToMultiByte(CP_UTF8, 0, werr, -1, utf8, sizeof(utf8), NULL, NULL);
        send_json_error(conn, 500, utf8);
        return;
    }
    TtsEngineRelease();   /* warm-up already happened inside Acquire */

    {
        wchar_t leaf[MAX_PATHLEN];
        char name[512], esc[1024];
        wchar_t *slash = wcsrchr(model_dir, L'\\');
        wcscpy(leaf, slash ? slash + 1 : model_dir);
        WideCharToMultiByte(CP_UTF8, 0, leaf, -1, name, sizeof(name), NULL, NULL);
        json_escape(name, esc, sizeof(esc));
        snprintf(body, sizeof(body),
                 "{\"model\":\"%s\",\"loaded_ms\":%lu,\"sample_rate\":%d}",
                 esc, (unsigned long)ms, TtsEngineSampleRate(model_dir));
    }
    http_send_full(conn, 200, "application/json; charset=utf-8", body, strlen(body));
}

static void handle_synthesize(HttpConn *conn, const HttpRequest *req) {
    char text[65536];
    text[0] = 0;   /* never pass uninitialised memory to the engine */
    char model[512] = "";
    model[0] = 0;
    char error[512] = "";
    double speed = -1.0;
    int sid = 0;
    wchar_t model_dir[MAX_PATHLEN];
    unsigned char *wav;
    size_t wav_len = 0;
    const char *json = req->body ? req->body : "{}";

    /* Text comes from the body for POST; the query string is a convenience for
       GET (so <audio src="..."> works) but is capped by the URL length. */
    if (!json_get_string(json, "text", text, sizeof(text)) && req->query[0]) {
        /* crude query parsing: text=...&model=... */
        char q[2048];
        char *tok, *save = NULL;
        strncpy(q, req->query, sizeof(q) - 1);
        q[sizeof(q) - 1] = 0;
        for (tok = strtok_r(q, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
            char *eq = strchr(tok, '=');
            char *val;
            int len;
            static char decoded[1024];
            if (!eq) continue;
            *eq = 0;
            val = eq + 1;
            len = url_decode(val, decoded, sizeof(decoded));
            /* decoded is bounded to 1024, so these always fit their targets. */
            if (!strcmp(tok, "text")) snprintf(text, sizeof(text), "%s", decoded);
            else if (!strcmp(tok, "model")) snprintf(model, sizeof(model), "%s", decoded);
            else if (!strcmp(tok, "sid")) sid = atoi(decoded);
            else if (!strcmp(tok, "speed")) speed = atof(decoded);
            (void)len;
        }
    }
    if (!text[0]) { send_json_error(conn, 400, "no text supplied"); return; }
    json_get_string(json, "model", model, sizeof(model));
    /* A GET request may already have taken speed from the query string, so only
       fall back to 1.0 when neither source supplied a usable value. */
    {
        double body_speed = -1.0;
        if (json_get_double(json, "speed", &body_speed) && body_speed > 0.3) {
            speed = body_speed;
        }
        if (speed < 0.3) speed = 1.0;
    }
    if (!json_get_int(json, "sid", &sid)) sid = 0;
    if (speed <= 0.3 || speed > 3.0) speed = 1.0;

    /* Pronunciation dictionary: substitute words before synthesis.  Reloaded
       per request on purpose - the file is a few hundred bytes, and that way
       an entry added in the GUI applies to the very next web request. */
    {
        wchar_t dpath[MAX_PATHLEN];
        user_dict_path(dpath, MAX_PATHLEN);
        if (DictLoad(dpath) == 0 && DictCount() > 0) {
            int hits = DictApply(text, sizeof(text));
            if (hits && getenv("WIN7TTS_DEBUG")) {
                logf_("  dictionary: %d substitution(s)", hits);
            }
        }
    }

    if (!resolve_model(model, model_dir, MAX_PATHLEN)) {
        send_json_error(conn, 404, "no model installed; run scripts\\get_model.cmd gyro");
        return;
    }

    if (getenv("WIN7TTS_DEBUG")) {
        /* Show exactly what the engine will see, so a wrong-encoding bug is
           obvious instead of looking like "the model is bad". */
        char dump[512];
        int i, n = (int)strlen(text);
        if (n > 120) n = 120;
        for (i = 0; i < n; ++i) {
            unsigned char c = (unsigned char)text[i];
            dump[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
        dump[n] = 0;
        logf_("  text(%d bytes) = \"%s\"", (int)strlen(text), dump);
    }

    wav = synthesize(text, model_dir, (float)speed, sid, &wav_len,
                     error, sizeof(error));
    if (!wav) {
        send_json_error(conn, 500, error);
        return;
    }
    http_send_full(conn, 200, "audio/wav", (const char *)wav, wav_len);
    free(wav);
}

/* Free resident models - the "offload" half of the model-switch policy. */
static void handle_unload(HttpConn *conn, const HttpRequest *req) {
    char model[512] = "", keep[512] = "";
    char body[256];
    int n = 0;
    const char *json = (req->body && req->body_len) ? req->body : "{}";

    json_get_string(json, "model", model, sizeof(model));
    json_get_string(json, "keep", keep, sizeof(keep));

    if (model[0]) {
        /* Exact folder name only: unloading must never fall back to "the
           first model" the way synthesize does - that would free the wrong
           session.  A non-resident path is a no-op (returns 0). */
        wchar_t wname[512], path[MAX_PATHLEN];
        if (MultiByteToWideChar(CP_UTF8, 0, model, -1, wname, 512) > 0) {
            _snwprintf(path, MAX_PATHLEN, L"%s\\%s", g_models_dir, wname);
            n = TtsEngineEvict(path);
        }
    } else {
        wchar_t wkeep[MAX_PATHLEN];
        wkeep[0] = 0;
        if (keep[0]) {
            wchar_t wname[512];
            if (MultiByteToWideChar(CP_UTF8, 0, keep, -1, wname, 512) > 0) {
                _snwprintf(wkeep, MAX_PATHLEN, L"%s\\%s", g_models_dir, wname);
            }
        }
        n = TtsEngineEvictOthers(wkeep);
    }
    snprintf(body, sizeof(body), "{\"unloaded\":%d,\"resident\":%d}",
             n, TtsEngineResidentCount());
    http_send_full(conn, 200, "application/json; charset=utf-8", body, strlen(body));
    if (!g_quiet) {
        logf_("  unloaded %d model(s), %d resident", n, TtsEngineResidentCount());
    }
}

/* Serve models.json, so the web UI can show what is available to download
   without duplicating the catalogue anywhere else. */
static void handle_catalog(HttpConn *conn) {
    wchar_t path[MAX_PATHLEN];
    FILE *f;
    long size;
    char *buf;

    _snwprintf(path, MAX_PATHLEN, L"%s\\..\\models.json", g_models_dir);
    f = _wfopen(path, L"rb");
    if (!f) {
        send_json_error(conn, 404, "models.json not found (see the program folder)");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) {
        fclose(f);
        send_json_error(conn, 500, "models.json is empty or too large");
        return;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        send_json_error(conn, 500, "out of memory");
        return;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        send_json_error(conn, 500, "could not read models.json");
        return;
    }
    fclose(f);
    buf[size] = 0;
    http_send_full(conn, 200, "application/json; charset=utf-8", buf, (size_t)size);
    free(buf);
}

/* GET lists the dictionary; POST adds/replaces or deletes an entry and
   writes user.dict, so the GUI's dictionary tab and the web stay in step. */
static void handle_dict(HttpConn *conn, const HttpRequest *req, int is_get) {
    wchar_t path[MAX_PATHLEN];
    char *body;
    size_t cap = 8192, len = 0;
    int i, first = 1;

    user_dict_path(path, MAX_PATHLEN);

    if (!is_get) {
        char word[DICT_MAX_WORD] = "", repl[DICT_MAX_REPL] = "";
        const char *json = (req->body && req->body_len) ? req->body : "{}";
        const char *del_flag = json_find(json, "delete");
        char out[128];

        DictLoad(path);
        if (!json_get_string(json, "word", word, sizeof(word)) || !word[0]) {
            send_json_error(conn, 400, "expected {\"word\":..., \"repl\":...}");
            return;
        }
        if (del_flag && (*del_flag == 't' || *del_flag == '1')) {
            DictRemove(word);
        } else {
            if (!json_get_string(json, "repl", repl, sizeof(repl)) || !repl[0]) {
                send_json_error(conn, 400, "expected {\"word\":..., \"repl\":...}");
                return;
            }
            if (!DictSet(word, repl)) {
                send_json_error(conn, 400, "entry too long or dictionary is full");
                return;
            }
        }
        if (DictSave(path) != 0) {
            send_json_error(conn, 500, "could not write user.dict");
            return;
        }
        snprintf(out, sizeof(out), "{\"count\":%d}", DictCount());
        http_send_full(conn, 200, "application/json; charset=utf-8",
                       out, strlen(out));
        return;
    }

    DictLoad(path);
    body = (char *)malloc(cap);
    if (!body) { send_json_error(conn, 500, "out of memory"); return; }
    len += (size_t)snprintf(body + len, cap - len, "{\"entries\":[");
    for (i = 0; i < DictCount(); ++i) {
        const DictEntry *e = DictGet(i);
        char ew[4 * DICT_MAX_WORD], er[4 * DICT_MAX_REPL];
        char sw[4 * DICT_MAX_WORD + 16], sr[4 * DICT_MAX_REPL + 16];
        if (!e) break;
        json_escape(e->word, ew, sizeof(ew));
        json_escape(e->repl, er, sizeof(er));
        snprintf(sw, sizeof(sw), "\"word\":\"%s\"", ew);
        snprintf(sr, sizeof(sr), "\"repl\":\"%s\"", er);
        if (len + strlen(sw) + strlen(sr) + 8 >= cap) break;
        len += (size_t)snprintf(body + len, cap - len, "%s{%s,%s}",
                                first ? "" : ",", sw, sr);
        first = 0;
    }
    snprintf(body + len, cap - len, "]}");
    http_send_full(conn, 200, "application/json; charset=utf-8", body, strlen(body));
    free(body);
}

/* Documentation you can curl: every route, every parameter, one line each. */
static void handle_help(HttpConn *conn) {
    static const char help[] =
        "win7-tts HTTP API\n"
        "=================\n"
        "\n"
        "GET  /api/health      JSON status: uptime, installed/loaded models\n"
        "GET  /api/models      installed models: [{\"name\":..,\"loaded\":..}]\n"
        "GET  /api/catalog     models.json - every model available to download\n"
        "GET  /api/help        this text\n"
        "GET  /api/dict        pronunciation dictionary (user.dict) entries\n"
        "POST /api/dict        add:    {\"word\":\"..\",\"repl\":\"..\"}\n"
        "                      delete: {\"word\":\"..\",\"delete\":true}\n"
        "POST /api/warmup      {\"model\":\"name\"} -> loads it, reports ms\n"
        "POST /api/unload      free memory: {} = everything,\n"
        "                      {\"keep\":\"name\"} = everything but that model,\n"
        "                      {\"model\":\"name\"} = just that model.\n"
        "                      The web UI calls this when the page closes.\n"
        "POST /api/synthesize  {\"text\":\"..\",\"model\":\"name\",\"speed\":1.0,\n"
        "                      \"sid\":0} -> audio/wav\n"
        "GET  /api/synthesize?text=..  same via query string (for <audio src>)\n"
        "\n"
        "Synthesis honours user.dict (word substitution) when present.\n"
        "Connections: HTTP/1.1 keep-alive, max 200 requests / 15 s idle.\n";
    http_send_full(conn, 200, "text/plain; charset=utf-8", help, sizeof(help) - 1);
}

static void route(HttpConn *conn, HttpRequest *req) {
    int is_post = (strcmp(req->method, "POST") == 0);
    int is_get = (strcmp(req->method, "GET") == 0);

    if (strcmp(req->path, "/api/health") == 0) {
        if (!is_get && !is_post) { http_send_status(conn, 405, "text/plain", "GET or POST only"); return; }
        handle_health(conn);
        return;
    }
    if (strcmp(req->path, "/api/models") == 0) {
        if (!is_get) { http_send_status(conn, 405, "text/plain", "GET only"); return; }
        handle_models(conn);
        return;
    }
    if (strcmp(req->path, "/api/warmup") == 0) {
        if (!is_post) { http_send_status(conn, 405, "text/plain", "POST only"); return; }
        handle_warmup(conn, req);
        return;
    }
    if (strcmp(req->path, "/api/unload") == 0) {
        if (!is_post) { http_send_status(conn, 405, "text/plain", "POST only"); return; }
        handle_unload(conn, req);
        return;
    }
    if (strcmp(req->path, "/api/catalog") == 0) {
        if (!is_get) { http_send_status(conn, 405, "text/plain", "GET only"); return; }
        handle_catalog(conn);
        return;
    }
    if (strcmp(req->path, "/api/dict") == 0) {
        if (!is_get && !is_post) {
            http_send_status(conn, 405, "text/plain", "GET or POST only");
            return;
        }
        handle_dict(conn, req, is_get);
        return;
    }
    if (strcmp(req->path, "/api/help") == 0) {
        if (!is_get && !is_post) {
            http_send_status(conn, 405, "text/plain", "GET or POST only");
            return;
        }
        handle_help(conn);
        return;
    }
    if (strcmp(req->path, "/api/synthesize") == 0) {
        if (!is_get && !is_post) { http_send_status(conn, 405, "text/plain", "GET or POST only"); return; }
        handle_synthesize(conn, req);
        return;
    }
    if (strcmp(req->path, "/") == 0 || strcmp(req->path, "/index.html") == 0) {
        if (!is_get) { http_send_status(conn, 405, "text/plain", "GET only"); return; }
        http_send_full(conn, 200, "text/html; charset=utf-8",
                       win7_tts_ui_html, win7_tts_ui_html_len);
        return;
    }
    if (strcmp(req->path, "/app.js") == 0) {
        if (!is_get) { http_send_status(conn, 405, "text/plain", "GET only"); return; }
        http_send_full(conn, 200, "application/javascript; charset=utf-8",
                       win7_tts_ui_app_js, win7_tts_ui_app_js_len);
        return;
    }
    http_send_status(conn, 404, "text/plain", "no such endpoint");
}

/*
 * One thread per connection.
 *
 * Keep-alive means a client can sit idle *between* requests, and the old
 * sequential accept loop would stall every other client behind that idle
 * socket - browsers open several connections at once and would have queued
 * for up to the idle timeout each.  Threads only overlap on I/O: synthesis
 * is serialised inside the engine cache, so this buys concurrency for
 * waiting, not for CPU.
 *
 * The HttpConn is heap-allocated at accept time and owned by the thread
 * from here on (copied by value, so the accept loop can allocate the next).
 */
static DWORD WINAPI connection_thread(LPVOID param) {
    HttpConn conn = *(HttpConn *)param;
    HttpRequest req;
    int rc, served = 0;

    free(param);

    /*
     * Bounded windows: a keep-alive connection idles out after 15 s (the
     * value advertised in its Keep-Alive header), and a slow or broken
     * client can never wedge the server.
     *
     * Windows' SO_RCVTIMEO/SO_SNDTIMEO take a DWORD of *milliseconds* - not
     * the POSIX struct timeval.  Passing a timeval here (as this code did
     * before keep-alive existed) truncated to 15 *milliseconds*, so any gap
     * between two requests longer than a rounding error ended the
     * connection.  Single-read-per-connection never noticed; a browser does.
     */
    {
        DWORD timeout_ms = 15000;
        setsockopt(conn.sock, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&timeout_ms, sizeof(timeout_ms));
        setsockopt(conn.sock, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&timeout_ms, sizeof(timeout_ms));
    }

    for (;;) {
        rc = http_read_request(&conn, &req);
        if (getenv("WIN7TTS_DEBUG")) {
            fprintf(stderr, "  read -> rc=%d wserr=%d\n", rc,
                    (rc == 1) ? 0 : WSAGetLastError());
            fflush(stderr);
        }
        /* Answer the requests that are well-formed but not usable as-is,
           instead of dropping them with the connection. */
        if (rc == 2) {
            http_send_status(&conn, 411, NULL, "Length Required");
            break;
        }
        if (rc == 3) {
            http_send_status(&conn, 413, NULL, "Payload Too Large");
            break;
        }
        if (rc != 1) break;

        InterlockedIncrement(&g_requests);
        logf_("  %s %s (%lu bytes)", req.method, req.path,
              (unsigned long)req.body_len);

        /* Decide for *this* request before routing: the response headers are
           written inside route(), and they must match this decision. */
        conn.keep = !req.conn_close && (req.http11 || req.conn_keep10);
        route(&conn, &req);
        http_free_request(&req);

        if (!conn.keep) break;
        if (++served >= 200) break;   /* the bound promised in Keep-Alive */
    }
    shutdown(conn.sock, SD_BOTH);
    closesocket(conn.sock);
    if (conn.pend) free(conn.pend);
    return 0;
}

/*
 * Split a wide command line into argv-style pointers.  Returns the count.
 * Handles quoted arguments and backslash-escaped quotes.
 */
static int split_wide_args(wchar_t **out, int max_args) {
    wchar_t *p = GetCommandLineW();
    int n = 0;

    if (*p == L'"') {                       /* skip the quoted program path */
        p++;
        while (*p && *p != L'"') p++;
        if (*p) p++;
    } else {
        while (*p && *p != L' ') p++;
    }
    while (*p && n < max_args - 1) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        out[n++] = p;
        if (*p == L'"') {
            p++;
            while (*p && *p != L'"') {
                if (*p == L'\\' && p[1] == L'"') p++;
                p++;
            }
            if (*p) p++;
        } else {
            while (*p && *p != L' ' && *p != L'\t') p++;
        }
        if (*p) {
            *p = L'\0';
            p++;
        }
    }
    out[n] = NULL;
    return n;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    int i, port = 0, listener;
    wchar_t exe[MAX_PATHLEN], *slash;
    wchar_t *args[MAX_ARGS];
    int argc;
    (void)inst; (void)prev; (void)show; (void)cmdline;

    g_start = GetTickCount();
    TtsEngineInit();

    /*
     * Split the raw wide command line ourselves.  MinGW does not reliably
     * export __wargc/__wargv, and GetCommandLineW is the one source of truth.
     */
    argc = split_wide_args(args, MAX_ARGS);
    /* split_wide_args consumes the program path, so args[0] is the first
       *option* -- start at 0, not 1. */

    for (i = 0; i < argc; ++i) {
        if (!wcscmp(args[i], L"--port") && i + 1 < argc) {
            g_port = _wtoi(args[++i]);
        } else if (!wcscmp(args[i], L"--host") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, args[++i], -1, g_host, sizeof(g_host),
                                NULL, NULL);
        } else if (!wcscmp(args[i], L"--models") && i + 1 < argc) {
            wcscpy(g_models_dir, args[++i]);
        } else if (!wcscmp(args[i], L"--preload")) {
            g_preload = 1;
        } else if (!wcscmp(args[i], L"--quiet")) {
            g_quiet = 1;
        } else if (!wcscmp(args[i], L"--help") || !wcscmp(args[i], L"-h")) {
            wprintf(L"tts_server - local web interface for win7-tts\n\n"
                     L"  --port <n>     port to listen on (default 8756)\n"
                     L"  --host <a>     address to bind (default 127.0.0.1)\n"
                     L"  --models <d>   models folder (default ..\\models)\n"
                     L"  --preload      load the first model before serving\n"
                     L"  --quiet        do not log requests\n");
            return 0;
        }
    }

    /* Default models folder sits next to the executable: <runtime>/../models */
    if (!g_models_dir[0]) {
        GetModuleFileNameW(NULL, exe, MAX_PATHLEN);
        slash = wcsrchr(exe, L'\\');
        if (slash) *slash = L'\0';
        wcscpy(g_models_dir, exe);
        wcscat(g_models_dir, L"\\..\\models");
    }

    listener = http_listen(g_host, g_port, &port);
    if (!listener) {
        wprintf(L"Could not listen on %hs:%d\n", g_host, g_port);
        return 1;
    }
    g_port = port;

    fprintf(stderr, "win7-tts web interface\n");
    fprintf(stderr, "  open    http://%s:%d/\n", g_host, port);
    fprintf(stderr, "  models  %ls\n", g_models_dir);
    if (strcmp(g_host, "127.0.0.1") != 0) {
        fprintf(stderr, "  WARNING: bound to %s, not just loopback. Anyone who can\n"
                        "           reach this port can make this machine speak.\n",
                g_host);
    }
    if (model_count() == 0) {
        fprintf(stderr, "  no models installed - run scripts\\get_model.cmd gyro\n");
    }
    fflush(stderr);

    if (g_preload) {
        wchar_t dir[MAX_PATHLEN];
        if (resolve_model(NULL, dir, MAX_PATHLEN)) {
            wchar_t werr[512];
            DWORD t0 = GetTickCount();
            fprintf(stderr, "  preloading...");
            if (TtsEngineAcquire(dir, werr, 512)) {
                TtsEngineRelease();
                fprintf(stderr, " done in %lu ms\n",
                        (unsigned long)(GetTickCount() - t0));
            } else {
                fprintf(stderr, " failed\n");
            }
        }
    }

    for (;;) {
        HttpConn *c = (HttpConn *)malloc(sizeof(HttpConn));
        HANDLE th;
        if (!c) {
            Sleep(50);      /* out of memory: back off instead of spinning */
            continue;
        }
        if (!http_accept(listener, c)) {
            free(c);
            Sleep(10);      /* a failed accept must not turn into a busy loop */
            continue;
        }
        th = CreateThread(NULL, 0, connection_thread, c, 0, NULL);
        if (th) {
            CloseHandle(th);
        } else {
            connection_thread(c);   /* no thread available: serve inline */
        }
    }
    return 0;
}
