#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_min.h"

#define MAX_BODY (8 * 1024 * 1024)

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

int http_read_request(HttpConn *conn, HttpRequest *req) {
    char head[8192];
    size_t used = 0;
    int chunked = 0;

    memset(req, 0, sizeof(*req));
    req->body = NULL;
    req->body_len = 0;
    req->content_length = -1;

    /* Read until the blank line ending the header block. */
    for (;;) {
        int n;
        if (used + 1 >= sizeof(head)) return -1;   /* header block too large */
        n = recv(conn->sock, head + used, (int)(sizeof(head) - used - 1), 0);
        if (n == 0) return (used == 0) ? 0 : -1;
        if (n < 0) return -1;
        used += (size_t)n;
        head[used] = 0;
        if (strstr(head, "\r\n\r\n") || strstr(head, "\n\n")) break;
        if (n == 0) break;
    }

    {
        char *line_end = strstr(head, "\r\n");
        char *body_start;
        int scanned = 0;
        char *p;

        if (line_end) { *line_end = 0; body_start = strstr(line_end + 2, "\r\n\r\n"); }
        else { line_end = strchr(head, '\n'); if (line_end) *line_end = 0; body_start = strstr(head, "\n\n"); }
        if (!line_end) return -1;
        body_start = strstr(head, "\n\n");
        if (!body_start) return -1;

        /* Request line: METHOD SP PATH SP VERSION */
        if (sscanf(head, "%15s %2047s", req->method, req->path) < 2) return -1;
        {
            char *q = strchr(req->path, '?');
            if (q) {
                *q = 0;
                strncpy(req->query, q + 1, sizeof(req->query) - 1);
                req->query[sizeof(req->query) - 1] = 0;
            }
        }

        /* Headers. */
        p = line_end + 1;
        while (p && *p) {
            char *eol = strpbrk(p, "\r\n");
            if (eol) *eol = 0;
            if (_strnicmp(p, "Content-Length:", 15) == 0) {
                req->content_length = atoi(p + 15);
            } else if (_strnicmp(p, "Transfer-Encoding:", 18) == 0) {
                if (strstr(p + 18, "chunked")) chunked = 1;
            }
            if (!eol) break;
            p = eol + ((eol[0] == '\r' && eol[1] == '\n') ? 2 : 1);
            scanned = (p < body_start);
            (void)scanned;
            if (p >= body_start) break;
        }
        (void)chunked;

        if (req->content_length > MAX_BODY) return -1;
        if (req->content_length > 0) {
            size_t have = used - (size_t)((body_start + 2) - head);
            size_t need;
            if (have > (size_t)req->content_length) have = (size_t)req->content_length;
            req->body = (char *)malloc((size_t)req->content_length + 1);
            if (!req->body) return -1;
            memcpy(req->body, body_start + 2, have);
            need = (size_t)req->content_length - have;
            while (need > 0) {
                int n = recv(conn->sock, req->body + have, (int)need, 0);
                if (n <= 0) { free(req->body); req->body = NULL; return -1; }
                have += (size_t)n;
                need -= (size_t)n;
            }
            req->body[have] = 0;
            req->body_len = have;
        }
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
