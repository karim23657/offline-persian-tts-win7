#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_min.h"

#define MAX_BODY      (8 * 1024 * 1024)
#define MAX_HEADER_BYTES 16384

const char *http_status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
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
    strncpy(out->peer, inet_ntoa(peer.sin_addr), sizeof(out->peer) - 1);
    out->peer[sizeof(out->peer) - 1] = 0;
    return 1;
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

    memset(req, 0, sizeof(*req));
    req->content_length = -1;

    for (;;) {
        int n;
        if (used + 1 >= sizeof(buf)) return -1;    /* header block too large */
        n = recv(conn->sock, buf + used, (int)(sizeof(buf) - used - 1), 0);
        if (n == 0) return (used == 0) ? 0 : -1;
        if (n < 0) return -1;
        used += (size_t)n;
        buf[used] = 0;

        {
            char *crlf = strstr(buf, "\r\n\r\n");
            char *lf = strstr(buf, "\n\n");
            if (crlf && (!lf || crlf <= lf)) hdr_end = (size_t)(crlf - buf) + 4;
            else if (lf) hdr_end = (size_t)(lf - buf) + 2;
            if (hdr_end) break;
        }
    }

    /* Split the header block into individual lines. */
    headers = (char *)malloc(hdr_end + 1);
    if (!headers) return -1;
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
            return -1;
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
            }
            p = eol;
        }
    }
    free(headers);

    if (req->content_length > MAX_BODY) return -1;

    if (req->content_length > 0) {
        size_t have = used - hdr_end;
        size_t need;
        if (have > (size_t)req->content_length) have = (size_t)req->content_length;
        req->body = (char *)malloc((size_t)req->content_length + 1);
        if (!req->body) return -1;
        body_start = buf + hdr_end;
        memcpy(req->body, body_start, have);
        need = (size_t)req->content_length - have;
        while (need > 0) {
            int n = recv(conn->sock, req->body + have, (int)need, 0);
            if (n <= 0) {
                free(req->body);
                req->body = NULL;
                return -1;
            }
            have += (size_t)n;
            need -= (size_t)n;
        }
        req->body[have] = 0;
        req->body_len = have;
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
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Cache-Control: no-store\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     status, http_status_text(status),
                     content_type ? content_type : "text/plain; charset=utf-8",
                     extra ? extra : "");
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
