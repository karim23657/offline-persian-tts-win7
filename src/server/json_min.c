#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "json_min.h"

/* Append one UTF-8 encoding of a code point. */
static void put_utf8(char *out, int *n, int cap, unsigned cp) {
    if (*n + 4 >= cap) return;
    if (cp < 0x80) {
        out[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*n)++] = (char)(0xC0 | (cp >> 6));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*n)++] = (char)(0xE0 | (cp >> 12));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*n)++] = (char)(0xF0 | (cp >> 18));
        out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
    out[*n] = 0;
}

const char *json_find(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;

    if (!json) return NULL;
    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 2 + klen;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                return q;
            }
        }
        /* advance past this string, respecting escapes */
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        if (!*p) break;
    }
    return NULL;
}

/* Read a \uXXXX escape (handling a surrogate pair) starting at p.
   Returns the number of bytes consumed from p, or 0 on malformed input. */
static int read_unicode_escape(const char *p, char *out, int *n, int cap) {
    unsigned hi = 0, lo = 0;
    unsigned cp;
    int i, consumed = 4;

    for (i = 0; i < 4; ++i) {
        char c = p[i];
        hi <<= 4;
        if (c >= '0' && c <= '9') hi |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') hi |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hi |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    cp = hi;

    /* Combine a UTF-16 surrogate pair when one follows. */
    if (cp >= 0xD800 && cp <= 0xDBFF && p[4] == '\\' && p[5] == 'u') {
        for (i = 0; i < 4; ++i) {
            char c = p[6 + i];
            lo <<= 4;
            if (c >= '0' && c <= '9') lo |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') lo |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') lo |= (unsigned)(c - 'A' + 10);
            else break;
        }
        if (i == 4 && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            consumed = 10;
        }
    }
    put_utf8(out, n, cap, cp);
    return consumed;
}

int json_get_string(const char *json, const char *key, char *out, int out_size) {
    const char *p = json_find(json, key);
    int n = 0;

    if (!p || *p != '"' || out_size < 2) return 0;
    out[0] = 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            char c = p[1];
            p += 2;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'u') {
                int consumed = read_unicode_escape(p, out, &n, out_size);
                if (consumed <= 0) {      /* malformed: keep the raw escape */
                    if (n + 7 < out_size) n += sprintf(out + n, "\\u");
                    out[n] = 0;
                    break;
                }
                p += consumed;
                continue;
            }
            if (n + 1 < out_size) out[n++] = c;
            out[n] = 0;
            continue;
        }
        if (n + 1 < out_size) out[n++] = *p;
        out[n] = 0;
        p++;
    }
    return (*p == '"');
}

int json_get_double(const char *json, const char *key, double *out) {
    const char *p = json_find(json, key);
    char *end = NULL;
    double v;
    if (!p) return 0;
    v = strtod(p, &end);
    if (end == p) return 0;
    if (out) *out = v;
    return 1;
}

int json_get_int(const char *json, const char *key, int *out) {
    double d;
    if (!json_get_double(json, key, &d)) return 0;
    if (out) *out = (int)(d < 0 ? d - 0.5 : d + 0.5);
    return 1;
}

void json_escape(const char *in, char *out, int out_size) {
    int n = 0;
    if (out_size < 1) return;
    out[0] = 0;
    while (*in && n + 8 < out_size) {
        unsigned char c = (unsigned char)*in++;
        switch (c) {
        case '"':  out[n++] = '\\'; out[n++] = '"';  break;
        case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
        case '\n': out[n++] = '\\'; out[n++] = 'n';  break;
        case '\r': out[n++] = '\\'; out[n++] = 'r';  break;
        case '\t': out[n++] = '\\'; out[n++] = 't';  break;
        default:
            if (c < 0x20) {
                n += sprintf(out + n, "\\u%04x", c);
            } else {
                out[n++] = (char)c;   /* pass UTF-8 bytes through unchanged */
            }
            break;
        }
    }
    out[n] = 0;
}
