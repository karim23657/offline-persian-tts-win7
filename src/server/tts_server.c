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
 *   POST /api/warmup       load a model now and return how long it took
 *   POST /api/synthesize   JSON in, WAV out
 *   GET  /api/synthesize   same, but query parameters (handy for <audio src>)
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

/* --------------------------------------------------------------- synthesis */

typedef struct {
    WavWriter wav;
    int sample_rate;
} SynthCtx;

static void on_samples(const float *samples, int count, void *user) {
    SynthCtx *ctx = (SynthCtx *)user;
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

static void serve_connection(int listener) {
    HttpConn conn;
    HttpRequest req;
    int rc;

    if (!http_accept(listener, &conn)) return;

    /* Give a slow or broken client a bounded window, then drop it. */
    {
        struct timeval tv;
        tv.tv_sec = 20;
        tv.tv_usec = 0;
        setsockopt(conn.sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(conn.sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    }

    rc = http_read_request(&conn, &req);
    if (rc == 1) {
        InterlockedIncrement(&g_requests);
        logf_("  %s %s (%lu bytes)", req.method, req.path,
              (unsigned long)req.body_len);
        route(&conn, &req);
    }
    http_free_request(&req);
    shutdown(conn.sock, SD_BOTH);
    closesocket(conn.sock);
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
        serve_connection(listener);
        if (g_quiet) continue;
    }
    return 0;
}
