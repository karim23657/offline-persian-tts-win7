/*
 * dict.c - the word -> replacement dictionary shared by GUI, server and CLI.
 *
 * Deliberately small: a flat array, whole-word matching, no regular
 * expressions.  The file is read line by line with CreateFileW so the path
 * may be any Unicode directory, while the *content* stays UTF-8 because that
 * is what the engine consumes.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dict.h"

static DictEntry g_entries[DICT_MAX_ENTRIES];
static int g_count = 0;

void DictClear(void) {
    g_count = 0;
}

int DictCount(void) {
    return g_count;
}

const DictEntry *DictGet(int index) {
    if (index < 0 || index >= g_count) return NULL;
    return &g_entries[index];
}

static int ascii_is_word(unsigned char c) {
    return c >= 0x80 || (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static void trim(char *s) {
    size_t n;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        memmove(s, s + 1, strlen(s));
    }
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

int DictSet(const char *word, const char *repl) {
    int i;
    if (!word || !repl || !*word) return 0;
    if (strlen(word) >= DICT_MAX_WORD || strlen(repl) >= DICT_MAX_REPL) return 0;
    for (i = 0; i < g_count; ++i) {
        if (!strcmp(g_entries[i].word, word)) {
            strcpy(g_entries[i].repl, repl);
            return 1;
        }
    }
    if (g_count >= DICT_MAX_ENTRIES) return 0;
    strcpy(g_entries[g_count].word, word);
    strcpy(g_entries[g_count].repl, repl);
    g_count++;
    return 1;
}

int DictRemove(const char *word) {
    int i;
    if (!word) return 0;
    for (i = 0; i < g_count; ++i) {
        if (!strcmp(g_entries[i].word, word)) {
            memmove(&g_entries[i], &g_entries[i + 1],
                    (size_t)(g_count - i - 1) * sizeof(DictEntry));
            g_count--;
            return 1;
        }
    }
    return 0;
}

/* Parse "word = replacement" from one line (UTF-8). */
static void parse_line(char *line) {
    char *eq;
    char word[DICT_MAX_WORD], repl[DICT_MAX_REPL];

    trim(line);
    if (!*line || *line == '#') return;
    eq = strchr(line, '=');
    if (!eq) return;
    *eq = 0;
    snprintf(word, sizeof(word), "%s", line);
    snprintf(repl, sizeof(repl), "%s", eq + 1);
    trim(word);
    trim(repl);
    if (*word && *repl) DictSet(word, repl);
}

int DictLoad(const wchar_t *path) {
    HANDLE h;
    LARGE_INTEGER sz;
    char *buf;
    DWORD got = 0;
    char *line, *save = NULL;

    DictClear();
    if (!path || !*path) return 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;   /* no file yet: not an error */
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return -1;
    }
    buf = (char *)malloc((size_t)sz.QuadPart + 1);
    if (!buf) {
        CloseHandle(h);
        return -1;
    }
    if (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL)) {
        free(buf);
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    buf[got] = '\0';

    for (line = strtok_r(buf, "\r\n", &save); line;
         line = strtok_r(NULL, "\r\n", &save)) {
        parse_line(line);
    }
    free(buf);
    return 0;
}

int DictLoadA(const char *path) {
    wchar_t wpath[1024];
    if (!path) return 0;
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 1024) <= 0) return -1;
    return DictLoad(wpath);
}

int DictSave(const wchar_t *path) {
    HANDLE h;
    char line[DICT_MAX_WORD + DICT_MAX_REPL + 8];
    DWORD wrote;
    int i, ok = 1;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    for (i = 0; i < g_count && ok; ++i) {
        int n = snprintf(line, sizeof(line), "%s = %s\r\n",
                         g_entries[i].word, g_entries[i].repl);
        ok = WriteFile(h, line, (DWORD)n, &wrote, NULL) && wrote == (DWORD)n;
    }
    CloseHandle(h);
    return ok ? 0 : -1;
}

int DictApply(char *text, size_t cap) {
    size_t i = 0;
    int hits = 0;
    int pass;

    if (!text || cap == 0) return 0;

    /* A few passes so a chained entry (a -> b, b -> c) settles; the cap stops
       any pathological cycle.  Substitutions never re-scan their own output
       within a pass (i skips past the replacement). */
    for (pass = 0; pass < 4; ++pass) {
        int any = 0;
        i = 0;
        while (text[i]) {
            int best = -1;
            size_t best_len = 0;
            int at_start = (i == 0) || !ascii_is_word((unsigned char)text[i - 1]);
            if (!at_start) {
                i++;
                continue;
            }
            {
                int e;
                for (e = 0; e < g_count; ++e) {
                    size_t wl = strlen(g_entries[e].word);
                    if (wl == 0) continue;
                    /* _strnicmp folds ASCII case only, which is what we want. */
                    if (_strnicmp(text + i, g_entries[e].word, (int)wl) != 0) {
                        continue;
                    }
                    /* whole word: next byte must not be another word byte */
                    if (text[i + wl] &&
                        ascii_is_word((unsigned char)text[i + wl])) {
                        continue;
                    }
                    if (wl > best_len) {
                        best = e;
                        best_len = wl;
                    }
                }
            }
            if (best < 0) {
                i++;                       /* step one *byte* (UTF-8 safe) */
                continue;
            }
            {
                size_t rl = strlen(g_entries[best].repl);
                size_t rest = strlen(text + i + best_len);
                if (i + rl + rest + 1 > cap) {
                    i += best_len;         /* does not fit: leave it alone */
                    continue;
                }
                memmove(text + i + rl, text + i + best_len, rest + 1);
                memcpy(text + i, g_entries[best].repl, rl);
                i += rl;
                hits++;
                any = 1;
            }
        }
        if (!any) break;
    }
    return hits;
}
