/*
 * Real Windows 7 implementations for the handful of functions the shim cannot
 * resolve because they simply do not exist on Windows 7.
 *
 * Compiled with MinGW, whose headers are Win10-era and therefore already
 * declare CONDITION_VARIABLE and friends; we only supply the bodies.
 */

#include <windows.h>
#include "win7shim.h"

/*
 * MinGW's headers are Win10-era, so CONDITION_VARIABLE and friends are
 * already declared even though Windows 7 does not export them.  Nothing to
 * define here; we only supply our own bodies.
 */

/* ---- implemented here ---------------------------------------------------- */

static VOID WINAPI shim_GetSystemTimePreciseAsFileTime(LARGE_INTEGER *out) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    if (out) {
        out->LowPart = ft.dwLowDateTime;
        out->HighPart = ft.dwHighDateTime;
    }
}

static BOOL WINAPI shim_InitializeCriticalSectionEx(CRITICAL_SECTION *cs, DWORD flags, DWORD spin) {
    (void)flags;
    return InitializeCriticalSectionAndSpinCount(cs, spin);
}

static HANDLE WINAPI shim_CreateFile2(LPCWSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl, PHANDLE out) {
    HANDLE h = CreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (out) *out = h;
    return h;
}

static BOOL WINAPI shim_GetProcessMitigationPolicy(HANDLE proc, DWORD policy,
                                                   PVOID buf, SIZE_T size) {
    (void)proc;
    (void)policy;
    /* Report "no mitigations in effect" with a zeroed structure. */
    if (buf && size) ZeroMemory(buf, size);
    return TRUE;
}

/* ---- FLS ------------------------------------------------------------------ */
/*
 * Fiber Local Storage arrived in Windows 8.  Windows 7 does have TLS, so the
 * FLS entry points are reimplemented on top of TlsAlloc/TlsSetValue and a small
 * table mapping each FLS index onto its own TLS index.
 *
 * Limitation: the per-FLS callback passed to FlsAlloc is recorded but never
 * invoked, because hooking thread exit on Windows 7 would require replacing the
 * caller's entry point.  In practice sherpa-onnx / onnxruntime allocate FLS
 * slots without callbacks.
 */

#define SHIM_FLS_MAX 128

typedef DWORD(WINAPI *PFLS_ALLOC_CB)(PVOID);

static CRITICAL_SECTION g_fls_lock;
static DWORD g_fls_tls[SHIM_FLS_MAX];   /* FLS index -> TLS index */
static PFLS_ALLOC_CB g_fls_cb[SHIM_FLS_MAX];
static INIT_ONCE g_fls_once = INIT_ONCE_STATIC_INIT;

/*
 * One-time initialisation of the table and its lock.
 *
 * This must be a genuine one-shot: onnxruntime allocates FLS slots from several
 * threads while it is building a session, so an initialisation that lets one
 * thread observe "ready" before the lock is actually initialised leads straight
 * to EnterCriticalSection on garbage and an access violation.  The obvious
 * `if (InterlockedCompareExchange(&ready, 1, 0) == 0) { Initialize... }` is
 * exactly that bug, because it publishes "ready" before doing the work.
 *
 * InitOnceExecuteOnce is available from Windows Vista, so it is safe on
 * Windows 7, and it cannot be entered concurrently by two threads.
 */
static BOOL CALLBACK shim_fls_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    DWORD i;
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_fls_lock);
    for (i = 0; i < SHIM_FLS_MAX; ++i) {
        g_fls_tls[i] = TLS_OUT_OF_INDEXES;
        g_fls_cb[i] = NULL;
    }
    return TRUE;
}

static void shim_fls_init(void) {
    InitOnceExecuteOnce(&g_fls_once, shim_fls_init_once, NULL, NULL);
}

/* Caller must hold g_fls_lock. */
static DWORD shim_fls_alloc_locked(PVOID cb) {
    DWORD i, tls;
    for (i = 0; i < SHIM_FLS_MAX; ++i) {
        if (g_fls_tls[i] == TLS_OUT_OF_INDEXES) {
            tls = TlsAlloc();
            if (tls == TLS_OUT_OF_INDEXES) return 0;
            g_fls_tls[i] = tls;
            g_fls_cb[i] = (PFLS_ALLOC_CB)cb;
            return i + 1; /* FLS indices start at 1, 0 means failure */
        }
    }
    return 0;
}

static DWORD WINAPI shim_FlsAlloc(PVOID cb) {
    DWORD r;
    shim_fls_init();
    EnterCriticalSection(&g_fls_lock);
    r = shim_fls_alloc_locked(cb);
    LeaveCriticalSection(&g_fls_lock);
    return r;
}

static BOOL WINAPI shim_FlsFree(DWORD idx) {
    BOOL ok = FALSE;
    if (idx == 0 || idx > SHIM_FLS_MAX) return FALSE;
    shim_fls_init();
    EnterCriticalSection(&g_fls_lock);
    if (g_fls_tls[idx - 1] != TLS_OUT_OF_INDEXES) {
        TlsFree(g_fls_tls[idx - 1]);
        g_fls_tls[idx - 1] = TLS_OUT_OF_INDEXES;
        g_fls_cb[idx - 1] = NULL;
        ok = TRUE;
    }
    LeaveCriticalSection(&g_fls_lock);
    return ok;
}

static PVOID WINAPI shim_FlsGetValue(DWORD idx) {
    DWORD tls;
    if (idx == 0 || idx > SHIM_FLS_MAX) return NULL;
    shim_fls_init();
    EnterCriticalSection(&g_fls_lock);
    tls = g_fls_tls[idx - 1];
    LeaveCriticalSection(&g_fls_lock);
    if (tls == TLS_OUT_OF_INDEXES) return NULL;
    return TlsGetValue(tls);
}

static BOOL WINAPI shim_FlsSetValue(DWORD idx, PVOID v) {
    DWORD tls;
    if (idx == 0 || idx > SHIM_FLS_MAX) return FALSE;
    shim_fls_init();
    EnterCriticalSection(&g_fls_lock);
    tls = g_fls_tls[idx - 1];
    LeaveCriticalSection(&g_fls_lock);
    if (tls == TLS_OUT_OF_INDEXES) return FALSE;
    return TlsSetValue(tls, v);
}

/* ---- condition variables ------------------------------------------------- */
/*
 * Windows 8 added CONDITION_VARIABLE.  Windows 7 has SRW locks, which are
 * enough to build a correct CV: every waiter registers a private auto-reset
 * event, and signalling releases one (or all) of them.
 */

#define SHIM_CV_MAX 64

typedef struct SHIM_CV {
    CRITICAL_SECTION lock;
    DWORD waiting;
    HANDLE sems[SHIM_CV_MAX];
    DWORD nsems;
} SHIM_CV;

static SHIM_CV *shim_cv_new(void) {
    SHIM_CV *cv = (SHIM_CV *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SHIM_CV));
    if (!cv) return NULL;
    InitializeCriticalSection(&cv->lock);
    return cv;
}

static VOID WINAPI shim_InitializeConditionVariable(CONDITION_VARIABLE *v) {
    SHIM_CV *cv = shim_cv_new();
    if (!cv) return;
    memcpy(v, &cv, sizeof(cv)); /* the handle is opaque storage for our pointer */
}

static HANDLE shim_cv_register(SHIM_CV *cv) {
    HANDLE h = NULL;
    EnterCriticalSection(&cv->lock);
    if (cv->nsems < SHIM_CV_MAX) h = cv->sems[cv->nsems++] = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (h) cv->waiting++;
    LeaveCriticalSection(&cv->lock);
    return h;
}

static VOID shim_cv_unregister(SHIM_CV *cv, HANDLE h) {
    DWORD i;
    EnterCriticalSection(&cv->lock);
    for (i = 0; i < cv->nsems; ++i) {
        if (cv->sems[i] == h) {
            cv->sems[i] = cv->sems[cv->nsems - 1];
            cv->sems[--cv->nsems] = NULL;
            CloseHandle(h);
            if (cv->waiting) cv->waiting--;
            break;
        }
    }
    LeaveCriticalSection(&cv->lock);
}

static VOID WINAPI shim_WakeConditionVariable(CONDITION_VARIABLE *v) {
    SHIM_CV *cv = NULL;
    HANDLE target = NULL;
    DWORD i;
    memcpy(&cv, v, sizeof(cv));
    if (!cv) return;
    EnterCriticalSection(&cv->lock);
    for (i = 0; i < cv->nsems; ++i) {
        if (cv->sems[i]) {
            target = cv->sems[i];
            break;
        }
    }
    LeaveCriticalSection(&cv->lock);
    if (target) SetEvent(target);
}

static VOID WINAPI shim_WakeAllConditionVariable(CONDITION_VARIABLE *v) {
    SHIM_CV *cv = NULL;
    HANDLE copy[SHIM_CV_MAX];
    DWORD n = 0, i;
    memcpy(&cv, v, sizeof(cv));
    if (!cv) return;
    EnterCriticalSection(&cv->lock);
    for (i = 0; i < cv->nsems && n < SHIM_CV_MAX; ++i) {
        if (cv->sems[i]) copy[n++] = cv->sems[i];
    }
    LeaveCriticalSection(&cv->lock);
    /* set/reset/set so a re-armed waiter cannot swallow the signal */
    for (i = 0; i < n; ++i) {
        SetEvent(copy[i]);
        ResetEvent(copy[i]);
        SetEvent(copy[i]);
    }
}

static BOOL WINAPI shim_SleepConditionVariableSRW(CONDITION_VARIABLE *v, PSRWLOCK srw,
                                                  DWORD flags, DWORD timeout) {
    SHIM_CV *cv = NULL;
    HANDLE h;
    BOOL shared = (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0;
    BOOL result;

    memcpy(&cv, v, sizeof(cv));
    if (!cv) return FALSE;

    h = shim_cv_register(cv);
    if (!h) return FALSE;

    /* Acquire the caller's SRW lock, then block until signalled. */
    if (shared) {
        AcquireSRWLockShared(srw);
    } else {
        AcquireSRWLockExclusive(srw);
    }

    if (timeout == INFINITE) {
        WaitForSingleObject(h, INFINITE);
        result = TRUE;
    } else {
        result = WaitForSingleObject(h, timeout) == WAIT_OBJECT_0;
    }

    if (shared) {
        ReleaseSRWLockShared(srw);
    } else {
        ReleaseSRWLockExclusive(srw);
    }

    shim_cv_unregister(cv, h);
    return result;
}

/* ---- shcore.dll PathCch* ------------------------------------------------- */
/*
 * Windows 7 ships shcore.dll, but only with the older Path* API, so these two
 * get real implementations here.
 */

static HRESULT WINAPI shim_PathCchRemoveBackslash(PWSTR path, size_t cch) {
    size_t n;
    if (!path || !cch) return E_INVALIDARG;
    n = lstrlenW(path);
    if (n && path[n - 1] == L'\\') {
        /* keep the separator of a drive root ("C:\") or UNC root ("\") */
        if (n == 3 && path[1] == L':') return S_OK;
        if (n == 1) return S_OK;
        path[n - 1] = L'\0';
    }
    return S_OK;
}

static HRESULT WINAPI shim_PathCchRemoveFileSpec(PWSTR path, size_t cch) {
    size_t n, i;
    if (!path || !cch) return E_INVALIDARG;
    n = lstrlenW(path);
    if (!n) return E_INVALIDARG;
    i = n;
    while (i > 0 && (path[i - 1] == L'\\' || path[i - 1] == L'/')) i--;
    if (i == 0) return S_OK;
    while (i > 0 && path[i - 1] != L'\\' && path[i - 1] != L'/') i--;
    if (i == 0) return S_OK; /* root-relative spec such as "file.txt" */
    path[i - 1] = L'\0';    /* drop the separator as well */
    return S_OK;
}

/* ---- dispatch ------------------------------------------------------------ */

/* Defined at the end of this file, with the other dxgi helpers. */
static HRESULT WINAPI shim_CreateDXGIFactory2(REFIID riid, void **ppFactory);

FARPROC win7shim_get_fallback(const char *name) {
    static const struct {
        const char *name;
        FARPROC addr;
    } table[] = {
        {"GetSystemTimePreciseAsFileTime", (FARPROC)shim_GetSystemTimePreciseAsFileTime},
        {"InitializeCriticalSectionEx", (FARPROC)shim_InitializeCriticalSectionEx},
        {"CreateFile2", (FARPROC)shim_CreateFile2},
        {"GetProcessMitigationPolicy", (FARPROC)shim_GetProcessMitigationPolicy},
        {"FlsAlloc", (FARPROC)shim_FlsAlloc},
        {"FlsFree", (FARPROC)shim_FlsFree},
        {"FlsGetValue", (FARPROC)shim_FlsGetValue},
        {"FlsSetValue", (FARPROC)shim_FlsSetValue},
        {"InitializeConditionVariable", (FARPROC)shim_InitializeConditionVariable},
        {"WakeConditionVariable", (FARPROC)shim_WakeConditionVariable},
        {"WakeAllConditionVariable", (FARPROC)shim_WakeAllConditionVariable},
        {"SleepConditionVariableSRW", (FARPROC)shim_SleepConditionVariableSRW},
        {"PathCchRemoveBackslash", (FARPROC)shim_PathCchRemoveBackslash},
        {"PathCchRemoveFileSpec", (FARPROC)shim_PathCchRemoveFileSpec},
        {"CreateDXGIFactory2", (FARPROC)shim_CreateDXGIFactory2},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (lstrcmpA(table[i].name, name) == 0) return table[i].addr;
    }
    return NULL;
}

/* ---- dxgi.dll ------------------------------------------------------------ */
/*
 * CreateDXGIFactory2 arrived with DXGI 1.2 in Windows 8, but Windows 7's
 * dxgi.dll exports CreateDXGIFactory1, which has an identical signature:
 *
 *     HRESULT CreateDXGIFactory1(REFIID riid, void **ppFactory)
 *
 * The only difference between the two is which DXGI version the factory
 * reports.  onnxruntime.dll imports CreateDXGIFactory2 statically, so on
 * Windows 7 the process refuses to start even though the DXGI-based GPU
 * execution provider is never selected here - the failure happens during
 * loading, before any of that matters.  Forwarding to the 1.1 factory is
 * enough to let the module load, and CPU inference is unaffected either way.
 */

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);

static HRESULT WINAPI shim_CreateDXGIFactory2(REFIID riid, void **ppFactory) {
    HMODULE dxgi;
    PFN_CreateDXGIFactory1 factory1;

    /* Validate before forwarding: some implementations dereference the out
     * parameter without checking it, and a NULL there is a hard fault. */
    if (!riid || !ppFactory) return E_POINTER;

    dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) {
        dxgi = LoadLibraryA("dxgi.dll");
        if (!dxgi) return DXGI_ERROR_NOT_FOUND;
    }
    factory1 = (PFN_CreateDXGIFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!factory1) return DXGI_ERROR_NOT_FOUND;

    return factory1(riid, ppFactory);
}
