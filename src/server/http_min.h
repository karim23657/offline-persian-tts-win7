/*
 * Small blocking HTTP/1.1 server for the loopback TTS API.
 *
 * Scope is deliberately narrow: serve a handful of routes to 127.0.0.1, parse
 * a request line plus headers and a Content-Length body, and stream a response
 * back.  No keep-alive, no TLS, no CGI - none of that belongs on a loopback
 * helper that binds one port.
 *
 * It binds 127.0.0.1 by default.  Binding 0.0.0.0 is available via --host but
 * prints a warning: this endpoint has no authentication and can make the
 * machine generate speech, so it must not be exposed to a network by accident.
 */
#ifndef HTTP_MIN_H
#define HTTP_MIN_H

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_MAX_METHOD 16
#define HTTP_MAX_PATH   2048

typedef struct {
    char method[HTTP_MAX_METHOD];
    char path[HTTP_MAX_PATH];
    char query[1024];
    char *body;          /* malloc'd; NUL terminated, NULL when empty */
    size_t body_len;
    int content_length;
} HttpRequest;

typedef struct {
    SOCKET sock;
    char peer[64];
} HttpConn;

/* Start listening. Returns the bound port, or 0 on failure (message printed). */
int http_listen(const char *host, int port, int *out_port);

/* Accept one connection. Returns 0 when the listener is closed. */
int http_accept(int listener, HttpConn *out);

/* Read a full request. Returns 1 on success, 0 on clean close, -1 on error. */
int http_read_request(HttpConn *conn, HttpRequest *req);

void http_free_request(HttpRequest *req);

/* Send a complete response with a body (sets Content-Length). */
int http_send_full(HttpConn *conn, int status, const char *content_type,
                   const char *body, size_t body_len);

/* Send a complete response with no body. */
int http_send_status(HttpConn *conn, int status, const char *content_type,
                     const char *text);

/* Begin a chunked response, so audio can be written as it is produced. */
int http_begin_chunked(HttpConn *conn, int status, const char *content_type);
int http_write_chunk(HttpConn *conn, const void *data, size_t len);
int http_end_chunked(HttpConn *conn);

/* The short phrase for a status code, e.g. "OK". */
const char *http_status_text(int status);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_MIN_H */
