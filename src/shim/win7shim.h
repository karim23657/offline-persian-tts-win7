#ifndef WIN7SHIM_H
#define WIN7SHIM_H

#include <windows.h>

/*
 * Primitives grabbed directly from the real KERNEL32.dll before any slot is
 * resolved: the shim's own code needs them and they must not be trampolined.
 */
struct win7shim_sys {
    HMODULE(WINAPI *gmha)(LPCSTR);
    FARPROC(WINAPI *gpa)(HMODULE, LPCSTR);
    int(WINAPI *lstrcmp)(LPCSTR, LPCSTR);
};
extern struct win7shim_sys win7shim_sys;

/* One redirected import. */
struct win7shim_entry {
    const char *primary;    /* DLL the import is redirected from          */
    const char *modules[3]; /* extra modules to probe, NULL terminated    */
    const char *names[4];   /* canonical name first, then aliases         */
};

/* Generated table (win7shim_table.c); index matches win7shim_slots. */
extern const struct win7shim_entry win7shim_table[];
extern const int win7shim_table_size;

/* Filled in by DllMain: the address every thunk jumps to. */
extern FARPROC win7shim_slots[];

/* Windows 7 implementations, see win7shim_fallback.c. */
extern FARPROC win7shim_get_fallback(const char *name);

#endif /* WIN7SHIM_H */