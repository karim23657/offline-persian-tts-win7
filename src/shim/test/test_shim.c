/*
 * MinGW's headers already declare every one of these Windows 8 APIs, so there
 * is nothing to declare here.  What matters is test_shim.def: it redirects
 * each of them to w7shim.dll, so under a Windows 7 loader they resolve through
 * the shim exactly as they do for the patched sherpa-onnx binary.
 */
#include <windows.h>
#include <stdio.h>

/* Not declared by MinGW; exported by shcore on Win8 and by our shim on Win7. */
typedef HRESULT(WINAPI *PFN_PathCchRemoveBackslash)(PWSTR, size_t);
typedef VOID(WINAPI *PFN_INIT_CV)(CONDITION_VARIABLE *);
typedef VOID(WINAPI *PFN_WAKE_CV)(CONDITION_VARIABLE *);
typedef BOOL(WINAPI *PFN_SLEEP_CV)(CONDITION_VARIABLE *, PSRWLOCK, DWORD, DWORD);

/* Test hook exported by the shim: name -> its Windows 7 implementation. */
typedef FARPROC(WINAPI *PFN_GET_FALLBACK)(const char *name);
typedef DWORD(WINAPI *PFN_FLS_ALLOC)(PVOID);
typedef PVOID(WINAPI *PFN_FLS_GET)(DWORD);
typedef BOOL(WINAPI *PFN_FLS_SET)(DWORD, PVOID);
typedef BOOL(WINAPI *PFN_FLS_FREE)(DWORD);
typedef HRESULT(WINAPI *PFN_PathCchRemoveFileSpec)(PWSTR, size_t);

static int g_fail = 0;

/*
 * Wakes the condition variable repeatedly from another thread, through the
 * shim's own export.  A single early wake can legitimately be lost (real CVs
 * lose signals sent before the waiter blocks), so we retry for a while to be
 * sure the waiter is definitely released.
 */
static DWORD WINAPI cv_waker_thread(LPVOID param) {
    CONDITION_VARIABLE *cv = (CONDITION_VARIABLE *)param;
    HMODULE shim = LoadLibraryA("w7shim.dll");
    PFN_GET_FALLBACK gf = shim ? (PFN_GET_FALLBACK)GetProcAddress(shim, "win7shim_get_fallback") : NULL;
    PFN_WAKE_CV wake = gf ? (PFN_WAKE_CV)gf("WakeConditionVariable") : NULL;
    int i;
    for (i = 0; i < 100; ++i) {
        Sleep(20);
        if (wake) wake(cv);
    }
    return 0;
}

/* Bundled function pointers so the worker thread needs one argument. */
typedef struct {
    PFN_FLS_ALLOC alloc;
    PFN_FLS_GET get;
    PFN_FLS_SET set;
    PFN_FLS_FREE free_it;
    volatile LONG *failures;
} FlsApi;

/*
 * Worker for the FLS stress test.
 *
 * Every thread races to allocate a slot, which is what exposed the one-time
 * initialisation bug.  Slots are allocated *and freed* again so the 128-entry
 * table is not simply exhausted; running out is expected and not a failure,
 * whereas a fault here is exactly the bug under test.
 */
static DWORD WINAPI fls_worker(LPVOID arg) {
    const FlsApi *api = (const FlsApi *)arg;
    int i;
    for (i = 0; i < 500; ++i) {
        DWORD idx = api->alloc(NULL);
        if (idx == 0) {
            /* table momentarily full - not an error, just back off a moment */
            Sleep(1);
            continue;
        }
        if (!api->set(idx, (PVOID)(ULONG_PTR)(idx + 1)) ||
            api->get(idx) != (PVOID)(ULONG_PTR)(idx + 1)) {
            InterlockedIncrement(api->failures);
        }
        if (!api->free_it(idx)) {
            InterlockedIncrement(api->failures);
        }
    }
    return 0;
}

static void check(const char *what, int ok) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main(void) {
    FILETIME ft1, ft2;
    LoadLibraryA("w7shim.dll");
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv2;
    SRWLOCK srw;
    HANDLE h;
    DWORD fls;
    wchar_t buf[64];

    printf("w7shim self-test\n");

    /* Timing must be monotonic and non-zero. */
    GetSystemTimePreciseAsFileTime(&ft1);
    Sleep(30);
    GetSystemTimePreciseAsFileTime(&ft2);
    check("GetSystemTimePreciseAsFileTime advances", ((DWORDLONG)ft2.dwHighDateTime<<32|ft2.dwLowDateTime) > ((DWORDLONG)ft1.dwHighDateTime<<32|ft1.dwLowDateTime));

    check("InitializeCriticalSectionEx", InitializeCriticalSectionEx(&cs, 0, 4000));
    EnterCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);

    /*
     * MinGW declares CreateFile2 with the wrong (5 argument) prototype, so it
     * is called through the real Win8 signature resolved at load time.  The
     * shim implements exactly this ABI.
     *
     * Note: Wine exports a CreateFile2 with a *different* 5 argument ABI, so
     * when the host already provides one the shim forwards to it and the call
     * is skipped here.  On real Windows 7 no such export exists, the shim's
     * own implementation is used, and this check runs for real.
     */
    {
        typedef HANDLE(WINAPI *PFN_CREATEFILE2_WIN8)(LPCWSTR, DWORD, DWORD,
                                                     LPSECURITY_ATTRIBUTES, DWORD,
                                                     DWORD, HANDLE, PHANDLE);
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        PFN_CREATEFILE2_WIN8 hostCreateFile2 = NULL;
        if (k32) {
            hostCreateFile2 = (PFN_CREATEFILE2_WIN8)GetProcAddress(k32, "CreateFile2");
        }
        if (hostCreateFile2) {
            printf("  [SKIP] CreateFile2 body: host provides its own non-Win8 ABI\n");
        } else {
            HMODULE shim = LoadLibraryA("w7shim.dll");
            PFN_CREATEFILE2_WIN8 createFile2 = shim
                ? (PFN_CREATEFILE2_WIN8)GetProcAddress(shim, "CreateFile2") : NULL;
            check("CreateFile2 resolved through the shim", createFile2 != NULL);
            if (createFile2) {
                h = createFile2(L"shim_test_tmp.txt", GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL, NULL);
                check("CreateFile2 returns a handle", h != INVALID_HANDLE_VALUE);
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
        }
        DeleteFileW(L"shim_test_tmp.txt");
    }

    {
        HMODULE shim = LoadLibraryA("w7shim.dll");
        PFN_GET_FALLBACK gf = shim
            ? (PFN_GET_FALLBACK)GetProcAddress(shim, "win7shim_get_fallback") : NULL;
        PFN_FLS_ALLOC flsAlloc = gf ? (PFN_FLS_ALLOC)gf("FlsAlloc") : NULL;
        PFN_FLS_GET flsGet = gf ? (PFN_FLS_GET)gf("FlsGetValue") : NULL;
        PFN_FLS_SET flsSet = gf ? (PFN_FLS_SET)gf("FlsSetValue") : NULL;
        PFN_FLS_FREE flsFree = gf ? (PFN_FLS_FREE)gf("FlsFree") : NULL;
        check("shim exports the FLS fallbacks",
              flsAlloc && flsGet && flsSet && flsFree);
        if (flsAlloc) {
            fls = flsAlloc(NULL);
            check("FlsAlloc returns a slot", fls != 0);
            check("FlsSetValue/FlsGetValue round-trip",
                  flsSet(fls, (PVOID)0x1234) && flsGet(fls) == (PVOID)0x1234);
            check("FlsFree", flsFree(fls));
        }
    }

    /*
     * Condition variables are exercised by calling the shim's exports directly
     * rather than through the import table.  Under an emulator (Wine) the host
     * may already export the Win8 CV APIs, in which case the shim forwards to
     * those and our own fallback would never run - resolving the export by name
     * is what actually tests the Windows 7 implementation.
     */
    {
        HMODULE shim = LoadLibraryA("w7shim.dll");
        PFN_GET_FALLBACK getFallback = shim
            ? (PFN_GET_FALLBACK)GetProcAddress(shim, "win7shim_get_fallback") : NULL;
        PFN_INIT_CV initCv = getFallback ? (PFN_INIT_CV)getFallback("InitializeConditionVariable") : NULL;
        PFN_WAKE_CV wakeCv = getFallback ? (PFN_WAKE_CV)getFallback("WakeConditionVariable") : NULL;
        PFN_SLEEP_CV sleepCv = getFallback ? (PFN_SLEEP_CV)getFallback("SleepConditionVariableSRW") : NULL;

        check("shim exports InitializeConditionVariable", initCv != NULL);
        check("shim exports WakeConditionVariable", wakeCv != NULL);
        check("shim exports SleepConditionVariableSRW", sleepCv != NULL);

        if (initCv && wakeCv && sleepCv) {
            CONDITION_VARIABLE cv3;
            SRWLOCK srw3;
            DWORD start, elapsed;

            initCv(&cv3);
            InitializeSRWLock(&srw3);
            CreateThread(NULL, 0, cv_waker_thread, &cv3, 0, NULL);

            /*
             * The SRW lock must NOT be held here: SleepConditionVariableSRW
             * acquires it itself, and taking it recursively would deadlock.
             */
            start = GetTickCount();
            /* Generous: the waker retries, so this fails only if genuinely broken. */
            check("shim SleepConditionVariableSRW returns after wake",
                  sleepCv(&cv3, &srw3, 0, 10000));
            elapsed = GetTickCount() - start;
            printf("         (released after %lu ms)\n", (unsigned long)elapsed);

            /* And it must honour the timeout when nobody signals. */
            initCv(&cv2);
            InitializeSRWLock(&srw);
            start = GetTickCount();
            check("shim SleepConditionVariableSRW times out when unsignalled",
                  !sleepCv(&cv2, &srw, 0, 300));
            elapsed = GetTickCount() - start;
            check("timeout was honoured (>= 250 ms)", elapsed >= 250);
        }
    }

    {
        HMODULE m = GetModuleHandleA("w7shim.dll");
        PFN_PathCchRemoveBackslash rmBack = m ? (PFN_PathCchRemoveBackslash)GetProcAddress(m, "PathCchRemoveBackslash") : NULL;
        PFN_PathCchRemoveFileSpec rmSpec = m ? (PFN_PathCchRemoveFileSpec)GetProcAddress(m, "PathCchRemoveFileSpec") : NULL;
        check("PathCchRemoveBackslash resolved", rmBack != NULL);
        check("PathCchRemoveFileSpec resolved", rmSpec != NULL);
        if (rmBack) {
            wcscpy(buf, L"C:\\foo\\bar\\");
            check("PathCchRemoveBackslash strips the trailing separator",
                  SUCCEEDED(rmBack(buf, 64)) && buf[wcslen(buf) - 1] != L'\\');
        }
        if (rmSpec) {
            wcscpy(buf, L"C:\\foo\\bar\\baz.txt");
            check("PathCchRemoveFileSpec drops the last component",
                  SUCCEEDED(rmSpec(buf, 64)) && wcscmp(buf, L"C:\\foo\\bar") == 0);
        }
    }

    {
        DWORD pol[8];
        check("GetProcessMitigationPolicy",
              GetProcessMitigationPolicy(GetCurrentProcess(), 0, pol, sizeof(pol)));
    }

    /* Plain Win7 APIs must still route through the shim unharmed. */
    check("GetTickCount64 via shim", GetTickCount64() > 0);

    /*
     * Hammer the FLS implementation from many threads at once.  This is the
     * regression test for a real crash on Windows 7: the table used to be
     * initialised lazily, and the "ready" flag was published before the lock
     * itself was initialised, so a second thread could call
     * EnterCriticalSection on an uninitialised CRITICAL_SECTION and fault with
     * 0xC0000005 while onnxruntime was building a session.
     */
    {
        HMODULE shim = LoadLibraryA("w7shim.dll");
        PFN_GET_FALLBACK gfs = shim
            ? (PFN_GET_FALLBACK)GetProcAddress(shim, "win7shim_get_fallback") : NULL;
        PFN_FLS_ALLOC alloc = gfs ? (PFN_FLS_ALLOC)gfs("FlsAlloc") : NULL;
        PFN_FLS_GET get = gfs ? (PFN_FLS_GET)gfs("FlsGetValue") : NULL;
        PFN_FLS_SET set = gfs ? (PFN_FLS_SET)gfs("FlsSetValue") : NULL;
        PFN_FLS_FREE freeIt = gfs ? (PFN_FLS_FREE)gfs("FlsFree") : NULL;
        check("shim exports the FLS fallbacks", alloc && get && set && freeIt);

        if (alloc && get && set && freeIt) {
            FlsApi api;
            HANDLE threads[16];
            volatile LONG failures = 0;
            int t;

            api.alloc = alloc;
            api.get = get;
            api.set = set;
            api.free_it = freeIt;
            api.failures = &failures;

            /* Start them all at once so they collide on the first allocation. */
            for (t = 0; t < 16; ++t) {
                threads[t] = CreateThread(NULL, 0, fls_worker, &api, 0, NULL);
            }
            for (t = 0; t < 16; ++t) {
                if (threads[t]) {
                    WaitForSingleObject(threads[t], 30000);
                    CloseHandle(threads[t]);
                }
            }
            check("16 threads hammering FLS survived", failures == 0);
        }
    }

    /*
     * CreateDXGIFactory2 is the dxgi.dll import that stops onnxruntime.dll
     * from loading on Windows 7.  Resolve the Windows 7 implementation
     * directly and call it, so Wine's own (working) CreateDXGIFactory2 cannot
     * mask a broken fallback.
     */
    {
        HMODULE shim = LoadLibraryA("w7shim.dll");
        PFN_GET_FALLBACK gf2 = shim
            ? (PFN_GET_FALLBACK)GetProcAddress(shim, "win7shim_get_fallback") : NULL;
        typedef HRESULT(WINAPI *PFN_DXGI_FACTORY)(const void *, void **);
        PFN_DXGI_FACTORY makeFactory = gf2
            ? (PFN_DXGI_FACTORY)gf2("CreateDXGIFactory2") : NULL;
        check("shim exports the CreateDXGIFactory2 fallback", makeFactory != NULL);
        if (makeFactory) {
            GUID zero;
            void *out = NULL;
            HRESULT hr;

            /*
             * Test the argument checking rather than the underlying factory.
             * A zero GUID is not a real interface, and asking Wine's
             * dxgi.dll for an unknown IID faults inside Wine itself, which
             * says nothing about the Windows 7 implementation we care about.
             * The NULL out-pointer path is entirely ours and is deterministic.
             */
            ZeroMemory(&zero, sizeof(zero));
            hr = makeFactory(&zero, NULL);
            check("rejects a NULL out-pointer with E_POINTER", hr == (HRESULT)0x80004003L);
            hr = makeFactory(NULL, &out);
            check("rejects a NULL IID with E_POINTER", hr == (HRESULT)0x80004003L);
        }
    }

    printf("%s\n", g_fail ? "RESULT: FAILURES" : "RESULT: ALL PASS");
    return g_fail ? 1 : 0;
}