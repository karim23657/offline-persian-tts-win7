#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_min.h"

/* Log every read_request outcome when WIN7TTS_DEBUG is set (keep-alive
   debugging: which path ended a connection). */
#define DBG_RETURN(v) do { \
    if (getenv("WIN7TTS_DEBUG")) { \
        fprintf(stderr, "  http_read_request -> %d (line %d)\n", (int)(v), __LINE__); \
        fflush(stderr); \
    } \
    return (v); \
} while (0)

#define MAX_BODY      (8 * 1024 * 1024)
#define MAX_HEADER_BYTES 16384

const char *http_status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

static int send_all(SOCKET s, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, (int)(len - sent), 0);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

int http_listen(const char *host, int port, int *out_port) {
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in addr;
    int opt = 1;
    int len = sizeof(addr);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 0;
    }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed\n");
        return 0;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind %s:%d failed (error %d)\n", host, port, WSAGetLastError());
        closesocket(s);
        return 0;
    }
    if (listen(s, 16) == SOCKET_ERROR) {
        fprintf(stderr, "listen failed\n");
        closesocket(s);
        return 0;
    }
    if (getsockname(s, (struct sockaddr *)&addr, &len) == 0) {
        if (out_port) *out_port = ntohs(addr.sin_port);
    }
    return (int)s;
}

int http_accept(int listener, HttpConn *out) {
    struct sockaddr_in peer;
    int len = sizeof(peer);
    SOCKET c = accept((SOCKET)listener, (struct sockaddr *)&peer, &len);
    if (c == INVALID_SOCKET) return 0;
    out->sock = c;
    out->keep = 0;
    out->pend = NULL;
    out->pend_len = 0;
    out->pend_cap = 0;
    strncpy(out->peer, inet_ntoa(peer.sin_addr), sizeof(out->peer) - 1);
    out->peer[sizeof(out->peer) - 1] = 0;
    return 1;
}

/* Keep the bytes that arrived past the current request (HTTP pipelining). */
static void save_pend(HttpConn *conn, const char *data, size_t len) {
    char *p;
    if (!len || !data) return;
    if (len > conn->pend_cap) {
        p = (char *)realloc(conn->pend, len);
        if (!p) return;      /* only a keep-alive nicety is lost on OOM */
        conn->pend = p;
        conn->pend_cap = len;
    }
    memcpy(conn->pend, data, len);
    conn->pend_len = len;
}

/*
 * Read one request.
 *
 * Deliberately simple: read until the header terminator, parse the request
 * line and headers, then read exactly Content-Length bytes of body.  Only
 * "\r\n\r\n" and "\n\n" are accepted as terminators - a search for "\n\n"
 * alone silently fails on normal CRLF clients and closes the connection with
 * no reply at all, which is exactly what happened before this was rewritten.
 */
int http_read_request(HttpConn *conn, HttpRequest *req) {
    char buf[MAX_HEADER_BYTES];
    size_t used = 0;
    size_t hdr_end = 0;      /* offset just past the terminator */
    char *headers;
    char *body_start;

    buf[0] = 0;              /* safe to search even before the first recv */

    memset(req, 0, sizeof(*req));
    req->content_length = -1;

    /*
     * Search-then-receive: bytes held over from the previous request
     * (pipelining) are already in `buf`, so a second request that arrived in
     * the same TCP segment must be found before asking the socket for more.
     * `pend` is always carved out of this same buffer, so it always fits.
     */
    if (conn->pend_len) {
        size_t n = conn->pend_len;
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        memcpy(buf, conn->pend, n);
        used = n;
        buf[used] = 0;
        free(conn->pend);
        conn->pend = NULL;
        conn->pend_len = conn->pend_cap = 0;
    }

    for (;;) {
        int n;
        {
            char *crlf = strstr(buf, "\r\n\r\n");
            char *lf = strstr(buf, "\n\n");
            if (crlf && (!lf || crlf <= lf)) hdr_end = (size_t)(crlf - buf) + 4;
            else if (lf) hdr_end = (size_t)(lf - buf) + 2;
            if (hdr_end) break;
        }
        if (used + 1 >= sizeof(buf)) DBG_RETURN(-1);    /* header block too large */
        n = recv(conn->sock, buf + used, (int)(sizeof(buf) - used - 1), 0);
        /* Clean close, or the idle gap between keep-alive requests running
           into the read timeout - both are normal ends of a connection. */
        if (n == 0) DBG_RETURN((used == 0) ? 0 : -1);
        if (n < 0) DBG_RETURN((used == 0) ? 0 : -1);
        used += (size_t)n;
        buf[used] = 0;
    }

    /* Split the header block into individual lines. */
    headers = (char *)malloc(hdr_end + 1);
    if (!headers) DBG_RETURN(-1);
    memcpy(headers, buf, hdr_end);
    headers[hdr_end] = 0;

    /*
     * Request line: parse from a copy, so the original block keeps its CRLFs
     * for the header walk below.  Writing a NUL over the first CRLF earlier
     * made the header loop stop immediately, Content-Length stayed -1 and the
     * body was never read.
     */
    {
        char line[HTTP_MAX_PATH + 32];
        char *eol = strpbrk(headers, "\r\n");
        size_t len;
        if (eol) *eol = 0;
        snprintf(line, sizeof(line), "%s", headers);
        if (eol) *eol = '\r';   /* restore for the header walk */
        if (sscanf(line, "%15s %2047s", req->method, req->path) < 2) {
            free(headers);
            DBG_RETURN(-1);
        }
        {
            char ver[16] = "";
            if (sscanf(line, "%*15s %*2047s %15s", ver) == 1) {
                if (!strcmp(ver, "HTTP/1.1") || !strncmp(ver, "HTTP/2", 6) ||
                    !strncmp(ver, "HTTP/3", 6)) {
                    req->http11 = 1;
                }
            }
        }
        len = strlen(req->path);
        if (len && req->path[len - 1] == '?') {
            req->path[len - 1] = 0;
        } else {
            char *q = strchr(req->path, '?');
            if (q) {
                *q = 0;
                snprintf(req->query, sizeof(req->query), "%s", q + 1);
            }
        }
    }

    /* Headers: copy each line out before comparing, so nothing is clobbered. */
    {
        char line[512];
        char *p = strpbrk(headers, "\r\n");
        while (p && *p) {
            char *eol;
            size_t len;
            p += (*p == '\r' && p[1] == '\n') ? 2 : 1;
            if (!*p) break;
            eol = strpbrk(p, "\r\n");
            len = eol ? (size_t)(eol - p) : strlen(p);
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
            line[len] = 0;
            if (_strnicmp(line, "Content-Length:", 15) == 0) {
                req->content_length = atoi(line + 15);
            } else if (_strnicmp(line, "Connection:", 11) == 0) {
                char *v = line + 11;
                while (*v == ' ' || *v == '\t') v++;
                if (_strnicmp(v, "close", 5) == 0) req->conn_close = 1;
                else if (_strnicmp(v, "keep-alive", 10) == 0) req->conn_keep10 = 1;
            }
            p = eol;
        }
    }
    free(headers);

    /* Answerable errors get their own codes so the loop can reply properly
       instead of dropping a well-formed-but-awkward request on the floor. */
    if (req->content_length > MAX_BODY) DBG_RETURN(3);
    if (req->content_length < 0 &&
        (!strcmp(req->method, "POST") || !strcmp(req->method, "PUT"))) {
        DBG_RETURN(2);   /* body length unknown: the stream cannot stay aligned */
    }

    if (req->content_length > 0) {
        size_t avail = used - hdr_end;   /* body bytes already in hand */
        size_t have = avail;
        size_t need;
        if (have > (size_t)req->content_length) have = (size_t)req->content_length;
        req->body = (char *)malloc((size_t)req->content_length + 1);
        if (!req->body) DBG_RETURN(-1);
        body_start = buf + hdr_end;
        memcpy(req->body, body_start, have);
        need = (size_t)req->content_length - have;
        while (need > 0) {
            int n = recv(conn->sock, req->body + have, (int)need, 0);
            if (n <= 0) {
                free(req->body);
                req->body = NULL;
                DBG_RETURN(-1);
            }
            have += (size_t)n;
            need -= (size_t)n;
        }
        req->body[have] = 0;
        req->body_len = have;
        /* Whatever arrived past the body is the next pipelined request. */
        if (avail > (size_t)req->content_length) {
            save_pend(conn, buf + hdr_end + req->content_length,
                      avail - (size_t)req->content_length);
        }
    } else if (used > hdr_end) {
        save_pend(conn, buf + hdr_end, used - hdr_end);
    }
    return 1;
}

void http_free_request(HttpRequest *req) {
    if (req->body) {
        free(req->body);
        req->body = NULL;
    }
}

static int send_response_head(HttpConn *conn, int status, const char *content_type,
                              const char *extra) {
    char head[640];
    /* The connection header follows the *request's* decision, so a keep-alive
       exchange advertises it (with the idle bound the server actually uses). */
    const char *conn_hdr = conn->keep
        ? "Connection: keep-alive\r\nKeep-Alive: timeout=15, max=200\r\n"
        : "Connection: close\r\n";
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Cache-Control: no-store\r\n"
                     "%s"
                     "%s"
                     "\r\n",
                     status, http_status_text(status),
                     content_type ? content_type : "text/plain; charset=utf-8",
                     extra ? extra : "", conn_hdr);
    return send_all(conn->sock, head, (size_t)n);
}

int http_send_full(HttpConn *conn, int status, const char *content_type,
                   const char *body, size_t body_len) {
    char extra[64];
    snprintf(extra, sizeof(extra), "Content-Length: %lu\r\n", (unsigned long)body_len);
    if (!send_response_head(conn, status, content_type, extra)) return 0;
    if (body_len && !send_all(conn->sock, body, body_len)) return 0;
    return 1;
}

int http_send_status(HttpConn *conn, int status, const char *content_type, const char *text) {
    return http_send_full(conn, status, content_type, text, strlen(text));
}

int http_begin_chunked(HttpConn *conn, int status, const char *content_type) {
    return send_response_head(conn, status, content_type, "Transfer-Encoding: chunked\r\n");
}

int http_write_chunk(HttpConn *conn, const void *data, size_t len) {
    char head[32];
    if (len == 0) return 1;
    snprintf(head, sizeof(head), "%lx\r\n", (unsigned long)len);
    if (!send_all(conn->sock, head, strlen(head))) return 0;
    if (!send_all(conn->sock, data, len)) return 0;
    return send_all(conn->sock, "\r\n", 2);
}

int http_end_chunked(HttpConn *conn) {
    return send_all(conn->sock, "0\r\n\r\n", 5);
}
