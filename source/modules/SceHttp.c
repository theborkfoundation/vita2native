/*
 * SceHttp: HTTP/1.1 client for the firmware API.
 *
 * Requests are synchronous: SendRequest performs the whole transaction and
 * buffers the response, then ReadData copies from that buffer. That is enough
 * for homebrew download managers and avoids keeping socket state in every
 * request object.
 *
 * HTTPS uses the host's OpenSSL through dlopen(), so there is no build-time
 * dependency on the target's TLS libraries. If OpenSSL is unavailable, https
 * URLs fail with SCE_HTTP_ERROR_SSL and plain http still works.
 */

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <psp2/net/http.h>
#include <psp2/types.h>

#include "module.h"

#define HTTP_MAX_TEMPLATES   8
#define HTTP_MAX_CONNECTIONS 8
#define HTTP_MAX_REQUESTS    16
#define HTTP_MAX_REDIRECTS   6
#define HTTP_MAX_HEADER_SIZE (64 * 1024)
#define HTTP_URL_MAX         2048

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} HttpBuffer;

typedef struct {
    bool used;
    int id;
    char user_agent[256];
    int http_version;
    int auto_proxy;
    int auto_redirect;
    unsigned int connect_timeout_us;
    unsigned int send_timeout_us;
    unsigned int recv_timeout_us;
    unsigned int resolve_timeout_us;
    unsigned int header_max_size;
} HttpTemplate;

typedef struct {
    bool used;
    int id;
    int template_id;
    char scheme[8];
    char host[256];
    unsigned short port;
} HttpConnection;

typedef struct {
    char name[64];
    char value[256];
} HttpRequestHeader;

typedef struct {
    bool used;
    int id;
    int connection_id;
    int method;
    char url[HTTP_URL_MAX];
    unsigned long long request_content_length;

    HttpRequestHeader headers[16];
    int header_count;

    int status_code;
    char *response_headers;
    unsigned int response_headers_size;
    uint8_t *response_body;
    size_t response_body_size;
    size_t response_body_pos;
    uint64_t response_content_length;
    bool aborted;
} HttpRequest;

static bool g_http_initialized;
static int g_next_id = 1;
static HttpTemplate g_templates[HTTP_MAX_TEMPLATES];
static HttpConnection g_connections[HTTP_MAX_CONNECTIONS];
static HttpRequest g_requests[HTTP_MAX_REQUESTS];

/* --------------------------------------------------------- TLS (dlopen) */

typedef const void *(*tls_client_method_fn)(void);
typedef void *(*ssl_ctx_new_fn)(const void *);
typedef void (*ssl_ctx_free_fn)(void *);
typedef int (*ssl_ctx_set_default_verify_paths_fn)(void *);
typedef void (*ssl_ctx_set_verify_fn)(void *, int, void *);
typedef void *(*ssl_new_fn)(void *);
typedef void (*ssl_free_fn)(void *);
typedef int (*ssl_set_fd_fn)(void *, int);
typedef long (*ssl_ctrl_fn)(void *, int, long, void *);
typedef int (*ssl_connect_fn)(void *);
typedef int (*ssl_read_fn)(void *, void *, int);
typedef int (*ssl_write_fn)(void *, const void *, int);
typedef int (*ssl_shutdown_fn)(void *);
typedef int (*ssl_get_error_fn)(const void *, int);
typedef long (*ssl_get_verify_result_fn)(const void *);
typedef int (*ssl_set1_host_fn)(void *, const char *);

static int g_tls_state; /* 0 = untried, 1 = ready, -1 = unavailable */
static void *g_ssl_handle;
static const void *g_tls_method;
static void *g_ssl_ctx;

static tls_client_method_fn fp_tls_client_method;
static ssl_ctx_new_fn fp_SSL_CTX_new;
static ssl_ctx_free_fn fp_SSL_CTX_free;
static ssl_ctx_set_default_verify_paths_fn fp_SSL_CTX_set_default_verify_paths;
static ssl_ctx_set_verify_fn fp_SSL_CTX_set_verify;
static ssl_new_fn fp_SSL_new;
static ssl_free_fn fp_SSL_free;
static ssl_set_fd_fn fp_SSL_set_fd;
static ssl_ctrl_fn fp_SSL_ctrl;
static ssl_connect_fn fp_SSL_connect;
static ssl_read_fn fp_SSL_read;
static ssl_write_fn fp_SSL_write;
static ssl_shutdown_fn fp_SSL_shutdown;
static ssl_get_error_fn fp_SSL_get_error;
static ssl_get_verify_result_fn fp_SSL_get_verify_result;
static ssl_set1_host_fn fp_SSL_set1_host;

static bool tls_load(void)
{
    if (g_tls_state > 0)
        return true;
    if (g_tls_state < 0)
        return false;

    static const char *const names[] = { "libssl.so.3", "libssl.so.1.1", "libssl.so", NULL };

    for (int i = 0; names[i] && !g_ssl_handle; i++)
        g_ssl_handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!g_ssl_handle)
        goto fail;

#define LOAD(name, var)                                                                           \
    do {                                                                                           \
        *(void **)(&var) = dlsym(g_ssl_handle, name);                                              \
        if (!var)                                                                                  \
            goto fail;                                                                             \
    } while (0)

    LOAD("TLS_client_method", fp_tls_client_method);
    LOAD("SSL_CTX_new", fp_SSL_CTX_new);
    LOAD("SSL_CTX_free", fp_SSL_CTX_free);
    LOAD("SSL_CTX_set_default_verify_paths", fp_SSL_CTX_set_default_verify_paths);
    LOAD("SSL_CTX_set_verify", fp_SSL_CTX_set_verify);
    LOAD("SSL_new", fp_SSL_new);
    LOAD("SSL_free", fp_SSL_free);
    LOAD("SSL_set_fd", fp_SSL_set_fd);
    LOAD("SSL_ctrl", fp_SSL_ctrl);
    LOAD("SSL_connect", fp_SSL_connect);
    LOAD("SSL_read", fp_SSL_read);
    LOAD("SSL_write", fp_SSL_write);
    LOAD("SSL_shutdown", fp_SSL_shutdown);
    LOAD("SSL_get_error", fp_SSL_get_error);
    LOAD("SSL_get_verify_result", fp_SSL_get_verify_result);
    *(void **)(&fp_SSL_set1_host) = dlsym(g_ssl_handle, "SSL_set1_host");

#undef LOAD

    g_tls_method = fp_tls_client_method();
    if (!g_tls_method)
        goto fail;

    g_ssl_ctx = fp_SSL_CTX_new(g_tls_method);
    if (!g_ssl_ctx)
        goto fail;

    fp_SSL_CTX_set_default_verify_paths(g_ssl_ctx);
    fp_SSL_CTX_set_verify(g_ssl_ctx, 1 /* SSL_VERIFY_PEER */, NULL);

    g_tls_state = 1;
    return true;

fail:
    if (g_ssl_handle) {
        dlclose(g_ssl_handle);
        g_ssl_handle = NULL;
    }
    g_tls_state = -1;
    return false;
}

static void *tls_start(int fd, const char *host)
{
    void *ssl;

    if (!tls_load())
        return NULL;

    ssl = fp_SSL_new(g_ssl_ctx);
    if (!ssl)
        return NULL;

    fp_SSL_set_fd(ssl, fd);
    /* SSL_CTRL_SET_TLSEXT_HOSTNAME (55), TLSEXT_NAMETYPE_host_name (0). */
    fp_SSL_ctrl(ssl, 55, 0, (void *)host);

    if (fp_SSL_set1_host && !fp_SSL_set1_host(ssl, host)) {
        fp_SSL_free(ssl);
        return NULL;
    }

    if (fp_SSL_connect(ssl) != 1) {
        fp_SSL_free(ssl);
        return NULL;
    }

    if (fp_SSL_get_verify_result(ssl) != 0) {
        fp_SSL_free(ssl);
        return NULL;
    }

    return ssl;
}

static void tls_stop(void *ssl)
{
    if (!ssl)
        return;

    fp_SSL_shutdown(ssl);
    fp_SSL_free(ssl);
}

/* ------------------------------------------------------------- utilities */

static bool buffer_reserve(HttpBuffer *buffer, size_t extra)
{
    size_t needed = buffer->length + extra;
    size_t capacity = buffer->capacity ? buffer->capacity : 4096;

    if (needed <= buffer->capacity)
        return true;

    while (capacity < needed)
        capacity *= 2;

    uint8_t *data = realloc(buffer->data, capacity);
    if (!data)
        return false;

    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

static bool buffer_append(HttpBuffer *buffer, const void *data, size_t size)
{
    if (!buffer_reserve(buffer, size))
        return false;

    memcpy(buffer->data + buffer->length, data, size);
    buffer->length += size;
    return true;
}

static HttpTemplate *template_for_id(int id)
{
    for (int i = 0; i < HTTP_MAX_TEMPLATES; i++) {
        if (g_templates[i].used && g_templates[i].id == id)
            return &g_templates[i];
    }
    return NULL;
}

static HttpConnection *connection_for_id(int id)
{
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (g_connections[i].used && g_connections[i].id == id)
            return &g_connections[i];
    }
    return NULL;
}

static HttpRequest *request_for_id(int id)
{
    for (int i = 0; i < HTTP_MAX_REQUESTS; i++) {
        if (g_requests[i].used && g_requests[i].id == id)
            return &g_requests[i];
    }
    return NULL;
}

/* Finds the template that owns any of the three object kinds. */
static HttpTemplate *template_for_any_id(int id)
{
    HttpTemplate *tmpl = template_for_id(id);
    if (tmpl)
        return tmpl;

    HttpConnection *conn = connection_for_id(id);
    if (conn)
        return template_for_id(conn->template_id);

    HttpRequest *request = request_for_id(id);
    if (request) {
        conn = connection_for_id(request->connection_id);
        if (conn)
            return template_for_id(conn->template_id);
    }

    return NULL;
}

static bool parse_url(const char *url, char *scheme, size_t scheme_size, char *host,
                      size_t host_size, unsigned short *port, char *path, size_t path_size)
{
    const char *separator;
    const char *authority;
    const char *host_end;
    const char *port_start;
    size_t scheme_length;

    if (!url)
        return false;

    separator = strstr(url, "://");
    if (!separator)
        return false;

    scheme_length = (size_t)(separator - url);
    if (scheme_length == 0 || scheme_length + 1 > scheme_size)
        return false;

    for (size_t i = 0; i < scheme_length; i++) {
        char c = url[i];
        if (!isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
            return false;
    }
    memcpy(scheme, url, scheme_length);
    scheme[scheme_length] = '\0';

    authority = separator + 3;
    host_end = authority;
    while (*host_end && *host_end != '/' && *host_end != ':' && *host_end != '?')
        host_end++;

    if ((size_t)(host_end - authority) == 0 || (size_t)(host_end - authority) + 1 > host_size)
        return false;
    memcpy(host, authority, (size_t)(host_end - authority));
    host[host_end - authority] = '\0';

    *port = strcmp(scheme, "https") == 0 ? 443 : 80;
    port_start = host_end;
    if (*port_start == ':') {
        char *end;
        long value = strtol(port_start + 1, &end, 10);

        if (end == port_start + 1 || value <= 0 || value > 65535)
            return false;
        *port = (unsigned short)value;
        host_end = end;
    }

    if (*host_end == '\0') {
        snprintf(path, path_size, "/");
    } else if (*host_end == '?') {
        snprintf(path, path_size, "/%s", host_end);
    } else {
        snprintf(path, path_size, "%s", host_end);
    }

    return true;
}

static int connect_to_host(const char *host, unsigned short port, const HttpTemplate *tmpl)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port_string[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_string, sizeof(port_string), "%u", port);

    if (getaddrinfo(host, port_string, &hints, &result) != 0)
        return -1;

    for (struct addrinfo *entry = result; entry; entry = entry->ai_next) {
        fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0)
            continue;

        struct timeval timeout;
        timeout.tv_sec = (time_t)((tmpl->connect_timeout_us ? tmpl->connect_timeout_us
                                                           : SCE_HTTP_DEFAULT_CONNECT_TIMEOUT) /
                                 1000000u);
        timeout.tv_usec = (suseconds_t)((tmpl->connect_timeout_us ? tmpl->connect_timeout_us
                                                                 : SCE_HTTP_DEFAULT_CONNECT_TIMEOUT) %
                                        1000000u);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static int connection_read(int fd, void *ssl, void *data, size_t size)
{
    if (ssl) {
        int ret = fp_SSL_read(ssl, data, (int)size);

        if (ret > 0)
            return ret;

        int error = fp_SSL_get_error(ssl, ret);
        if (error == 6 /* SSL_ERROR_ZERO_RETURN */)
            return 0;
        return -1;
    }

    return (int)recv(fd, data, size, 0);
}

static int connection_write(int fd, void *ssl, const void *data, size_t size)
{
    if (ssl)
        return fp_SSL_write(ssl, data, (int)size);

    return (int)send(fd, data, size, 0);
}

static int decode_chunked(const uint8_t *input, size_t input_size, HttpBuffer *output)
{
    size_t position = 0;

    while (position < input_size) {
        size_t line_end = position;
        unsigned long chunk_size = 0;
        char *end;

        while (line_end + 1 < input_size &&
               !(input[line_end] == '\r' && input[line_end + 1] == '\n'))
            line_end++;
        if (line_end + 1 >= input_size)
            return SCE_HTTP_ERROR_CHUNK_ENC;

        chunk_size = strtoul((const char *)input + position, &end, 16);
        (void)end;
        position = line_end + 2;

        if (chunk_size == 0)
            return 0; /* trailers are ignored */

        if (position + chunk_size > input_size)
            return SCE_HTTP_ERROR_CHUNK_ENC;

        if (!buffer_append(output, input + position, chunk_size))
            return SCE_HTTP_ERROR_OUT_OF_MEMORY;

        position += chunk_size;
        if (position + 1 < input_size && input[position] == '\r' && input[position + 1] == '\n')
            position += 2;
    }

    return 0;
}

static const char *find_header(const char *headers, const char *name, size_t *value_length)
{
    size_t name_length = strlen(name);

    for (const char *line = headers; line && *line;) {
        const char *line_end = strstr(line, "\r\n");
        const char *value;

        if (!line_end)
            line_end = line + strlen(line);

        if ((size_t)(line_end - line) > name_length && strncasecmp(line, name, name_length) == 0 &&
            line[name_length] == ':') {
            value = line + name_length + 1;
            while (value < line_end && (*value == ' ' || *value == '\t'))
                value++;
            if (value_length)
                *value_length = (size_t)(line_end - value);
            return value;
        }

        line = *line_end ? line_end + 2 : line_end;
    }

    return NULL;
}

static bool header_value_equals(const char *headers, const char *name, const char *expected)
{
    size_t length;
    const char *value = find_header(headers, name, &length);

    return value && length == strlen(expected) && strncasecmp(value, expected, length) == 0;
}

/* ------------------------------------------------------------- requests */

static int perform_request(HttpRequest *request, const void *post_data, unsigned int post_size,
                           int redirects_left)
{
    HttpConnection *connection = connection_for_id(request->connection_id);
    HttpTemplate *tmpl;
    HttpBuffer response = { 0 };
    HttpBuffer body = { 0 };
    HttpBuffer request_data = { 0 };
    char scheme[8], host[256], path[HTTP_URL_MAX];
    unsigned short port;
    const char *url = request->url;
    void *ssl = NULL;
    int fd = -1;
    int ret = 0;
    size_t header_end;
    size_t content_length_value;
    const char *status_text;
    int status_code = 0;
    static const char *method_names[] = { "GET",  "POST",   "HEAD", "OPTIONS",
                                          "PUT",  "DELETE", "TRACE", "CONNECT" };

    if (!connection)
        return SCE_HTTP_ERROR_INVALID_ID;

    tmpl = template_for_id(connection->template_id);
    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;

    if (!parse_url(url, scheme, sizeof(scheme), host, sizeof(host), &port, path, sizeof(path)))
        return SCE_HTTP_ERROR_INVALID_URL;

    fd = connect_to_host(host, port, tmpl);
    if (fd < 0)
        return SCE_HTTP_ERROR_NETWORK;

    if (strcmp(scheme, "https") == 0) {
        ssl = tls_start(fd, host);
        if (!ssl) {
            close(fd);
            return SCE_HTTP_ERROR_SSL;
        }
    }

    /* Build the request. */
    {
        char header_line[512];
        const char *method = method_names[request->method <= 7 ? request->method : 0];
        int written;

        written = snprintf(header_line, sizeof(header_line), "%s %s HTTP/1.1\r\n", method, path);
        if (written < 0 || !buffer_append(&request_data, header_line, (size_t)written))
            goto out_of_memory;

        written = snprintf(header_line, sizeof(header_line), "Host: %s\r\n", host);
        if (written < 0 || !buffer_append(&request_data, header_line, (size_t)written))
            goto out_of_memory;

        written = snprintf(header_line, sizeof(header_line), "User-Agent: %s\r\n",
                           tmpl->user_agent[0] ? tmpl->user_agent : "vita2native");
        if (written < 0 || !buffer_append(&request_data, header_line, (size_t)written))
            goto out_of_memory;

        if (!buffer_append(&request_data, "Accept: */*\r\n", 13))
            goto out_of_memory;

        if (request->request_content_length || post_size) {
            written = snprintf(header_line, sizeof(header_line), "Content-Length: %llu\r\n",
                               request->request_content_length ? request->request_content_length
                                                               : (unsigned long long)post_size);
            if (written < 0 || !buffer_append(&request_data, header_line, (size_t)written))
                goto out_of_memory;
        }

        for (int i = 0; i < request->header_count; i++) {
            written = snprintf(header_line, sizeof(header_line), "%s: %s\r\n",
                               request->headers[i].name, request->headers[i].value);
            if (written < 0 || !buffer_append(&request_data, header_line, (size_t)written))
                goto out_of_memory;
        }

        if (!buffer_append(&request_data, "Connection: close\r\n\r\n", 21))
            goto out_of_memory;

        if (post_data && post_size && !buffer_append(&request_data, post_data, post_size))
            goto out_of_memory;
    }

    if (connection_write(fd, ssl, request_data.data, request_data.length) !=
        (int)request_data.length) {
        ret = SCE_HTTP_ERROR_NETWORK;
        goto done;
    }

    /* Read everything until EOF. */
    for (;;) {
        uint8_t chunk[8192];
        int read = connection_read(fd, ssl, chunk, sizeof(chunk));

        if (read == 0)
            break;
        if (read < 0) {
            ret = SCE_HTTP_ERROR_NETWORK;
            goto done;
        }
        if (response.length + (size_t)read > HTTP_MAX_HEADER_SIZE + (1u << 30)) {
            ret = SCE_HTTP_ERROR_OUT_OF_SIZE;
            goto done;
        }
        if (!buffer_append(&response, chunk, (size_t)read)) {
            ret = SCE_HTTP_ERROR_OUT_OF_MEMORY;
            goto done;
        }
    }

    /* Split status line, headers and body. */
    header_end = 0;
    for (size_t i = 0; i + 3 < response.length; i++) {
        if (response.data[i] == '\r' && response.data[i + 1] == '\n' &&
            response.data[i + 2] == '\r' && response.data[i + 3] == '\n') {
            header_end = i + 4;
            break;
        }
    }
    if (!header_end) {
        ret = SCE_HTTP_ERROR_BAD_RESPONSE;
        goto done;
    }

    status_text = strstr((const char *)response.data, "\r\n");
    if (!status_text || sscanf((const char *)response.data, "HTTP/%*d.%*d %d", &status_code) != 1) {
        ret = SCE_HTTP_ERROR_BAD_RESPONSE;
        goto done;
    }

    {
        const char *headers_start = strstr((const char *)response.data, "\r\n") + 2;
        size_t headers_length = header_end - (size_t)(headers_start - (const char *)response.data);
        char *headers = malloc(headers_length + 1);

        if (!headers)
            goto out_of_memory;
        memcpy(headers, headers_start, headers_length);
        headers[headers_length] = '\0';

        /* Redirects. */
        if (status_code >= 300 && status_code < 400 && tmpl->auto_redirect &&
            redirects_left > 0) {
            size_t location_length;
            const char *location = find_header(headers, "Location", &location_length);

            if (location && location_length < HTTP_URL_MAX) {
                char next_url[HTTP_URL_MAX];
                int written;

                if (location[0] == '/') {
                    written = snprintf(next_url, sizeof(next_url), "%s://%s:%u%.*s", scheme, host,
                                       port, (int)location_length, location);
                } else {
                    written = snprintf(next_url, sizeof(next_url), "%.*s", (int)location_length,
                                       location);
                }

                free(headers);
                if (written < 0 || (size_t)written >= sizeof(next_url)) {
                    ret = SCE_HTTP_ERROR_INVALID_URL;
                    goto done;
                }

                snprintf(request->url, sizeof(request->url), "%s", next_url);
                free(request->response_headers);
                request->response_headers = NULL;
                request->response_headers_size = 0;
                free(request->response_body);
                request->response_body = NULL;
                request->response_body_size = 0;
                request->response_body_pos = 0;

                if (ssl)
                    tls_stop(ssl);
                close(fd);
                ssl = NULL;
                fd = -1;

                free(request_data.data);
                free(response.data);
                return perform_request(request, post_data, post_size, redirects_left - 1);
            }
        }

        free(request->response_headers);
        request->response_headers = headers;
        request->response_headers_size = (unsigned int)headers_length;
        request->status_code = status_code;

        /* Body: dechunk or copy the remainder. */
        if (header_value_equals(headers, "Transfer-Encoding", "chunked")) {
            ret = decode_chunked(response.data + header_end, response.length - header_end, &body);
            if (ret < 0) {
                free(headers);
                request->response_headers = NULL;
                goto done;
            }
            request->response_content_length = body.length;
        } else {
            size_t body_length = response.length - header_end;
            const char *length_header = find_header(headers, "Content-Length", &content_length_value);

            if (length_header) {
                char value[32];
                size_t copy = content_length_value < sizeof(value) - 1 ? content_length_value
                                                                       : sizeof(value) - 1;
                unsigned long long declared;

                memcpy(value, length_header, copy);
                value[copy] = '\0';
                declared = strtoull(value, NULL, 10);
                if (declared < body_length)
                    body_length = (size_t)declared;
                request->response_content_length = declared;
            } else {
                request->response_content_length = body_length;
            }

            if (!buffer_append(&body, response.data + header_end, body_length))
                goto out_of_memory;
        }
    }

    free(request->response_body);
    request->response_body = body.data;
    request->response_body_size = body.length;
    request->response_body_pos = 0;
    body.data = NULL;
    ret = 0;
    goto done;

out_of_memory:
    ret = SCE_HTTP_ERROR_OUT_OF_MEMORY;

done:
    free(request_data.data);
    free(body.data);
    free(response.data);
    if (ssl)
        tls_stop(ssl);
    if (fd >= 0)
        close(fd);
    return ret;
}

/* -------------------------------------------------------------- exports */

EXPORT(SceHttp, 0x214926D9, int, sceHttpInit, unsigned int pool_size)
{
    (void)pool_size;

    if (g_http_initialized)
        return SCE_HTTP_ERROR_ALREADY_INITED;

    memset(g_templates, 0, sizeof(g_templates));
    memset(g_connections, 0, sizeof(g_connections));
    memset(g_requests, 0, sizeof(g_requests));
    g_http_initialized = true;
    return 0;
}

EXPORT(SceHttp, 0xC9076666, int, sceHttpTerm, void)
{
    if (!g_http_initialized)
        return SCE_HTTP_ERROR_BEFORE_INIT;

    for (int i = 0; i < HTTP_MAX_REQUESTS; i++) {
        if (!g_requests[i].used)
            continue;
        free(g_requests[i].response_headers);
        free(g_requests[i].response_body);
    }

    memset(g_templates, 0, sizeof(g_templates));
    memset(g_connections, 0, sizeof(g_connections));
    memset(g_requests, 0, sizeof(g_requests));
    g_http_initialized = false;
    return 0;
}

EXPORT(SceHttp, 0x62241DAB, int, sceHttpCreateTemplate, const char *user_agent, int http_version,
       int auto_proxy)
{
    if (!g_http_initialized)
        return SCE_HTTP_ERROR_BEFORE_INIT;

    for (int i = 0; i < HTTP_MAX_TEMPLATES; i++) {
        HttpTemplate *tmpl = &g_templates[i];

        if (tmpl->used)
            continue;

        memset(tmpl, 0, sizeof(*tmpl));
        tmpl->used = true;
        tmpl->id = g_next_id++;
        if (user_agent)
            snprintf(tmpl->user_agent, sizeof(tmpl->user_agent), "%s", user_agent);
        tmpl->http_version = http_version;
        tmpl->auto_proxy = auto_proxy;
        tmpl->auto_redirect = 1;
        tmpl->connect_timeout_us = SCE_HTTP_DEFAULT_CONNECT_TIMEOUT;
        tmpl->send_timeout_us = SCE_HTTP_DEFAULT_SEND_TIMEOUT;
        tmpl->recv_timeout_us = SCE_HTTP_DEFAULT_RECV_TIMEOUT;
        tmpl->resolve_timeout_us = SCE_HTTP_DEFAULT_CONNECT_TIMEOUT;
        tmpl->header_max_size = HTTP_MAX_HEADER_SIZE;
        return tmpl->id;
    }

    return SCE_HTTP_ERROR_OUT_OF_MEMORY;
}

EXPORT(SceHttp, 0xEC85ECFB, int, sceHttpDeleteTemplate, int template_id)
{
    HttpTemplate *tmpl = template_for_id(template_id);

    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;

    memset(tmpl, 0, sizeof(*tmpl));
    return 0;
}

static int create_connection(int template_id, const char *scheme, const char *host,
                             unsigned short port)
{
    HttpTemplate *tmpl = template_for_id(template_id);

    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!scheme || !host)
        return SCE_HTTP_ERROR_INVALID_VALUE;
    if (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0)
        return SCE_HTTP_ERROR_UNKNOWN_SCHEME;

    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        HttpConnection *connection = &g_connections[i];

        if (connection->used)
            continue;

        memset(connection, 0, sizeof(*connection));
        connection->used = true;
        connection->id = g_next_id++;
        connection->template_id = template_id;
        snprintf(connection->scheme, sizeof(connection->scheme), "%s", scheme);
        snprintf(connection->host, sizeof(connection->host), "%s", host);
        connection->port = port;
        return connection->id;
    }

    return SCE_HTTP_ERROR_OUT_OF_MEMORY;
}

EXPORT(SceHttp, 0xC616C200, int, sceHttpCreateConnectionWithURL, int template_id, const char *url,
       int enable_keepalive)
{
    char scheme[8], host[256], path[HTTP_URL_MAX];
    unsigned short port;

    (void)enable_keepalive;

    if (getenv("VITA2NATIVE_INPUT_DEBUG")) {
        fprintf(stderr, "vita2native: HTTP connection for %s\n", url ? url : "(null)");
        fflush(stderr);
    }

    if (!parse_url(url, scheme, sizeof(scheme), host, sizeof(host), &port, path, sizeof(path)))
        return SCE_HTTP_ERROR_INVALID_URL;

    return create_connection(template_id, scheme, host, port);
}

EXPORT(SceHttp, 0xAEB3307E, int, sceHttpCreateConnection, int template_id, const char *server_name,
       const char *scheme, unsigned short port, int enable_keepalive)
{
    (void)enable_keepalive;
    return create_connection(template_id, scheme, server_name, port);
}

EXPORT(SceHttp, 0xF0F65C15, int, sceHttpDeleteConnection, int connection_id)
{
    HttpConnection *connection = connection_for_id(connection_id);

    if (!connection)
        return SCE_HTTP_ERROR_INVALID_ID;

    memset(connection, 0, sizeof(*connection));
    return 0;
}

static int create_request(int connection_id, int method, const char *url,
                          unsigned long long content_length)
{
    HttpConnection *connection = connection_for_id(connection_id);

    if (!connection)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (method < 0 || method > 7)
        return SCE_HTTP_ERROR_UNKNOWN_METHOD;

    for (int i = 0; i < HTTP_MAX_REQUESTS; i++) {
        HttpRequest *request = &g_requests[i];

        if (request->used)
            continue;

        memset(request, 0, sizeof(*request));
        request->used = true;
        request->id = g_next_id++;
        request->connection_id = connection_id;
        request->method = method;
        request->request_content_length = content_length;
        request->response_content_length = (uint64_t)-1;
        if (url)
            snprintf(request->url, sizeof(request->url), "%s", url);
        return request->id;
    }

    return SCE_HTTP_ERROR_OUT_OF_MEMORY;
}

EXPORT(SceHttp, 0xBD5DA1D0, int, sceHttpCreateRequestWithURL, int connection_id, int method,
       const char *url, unsigned long long content_length)
{
    if (!url)
        return SCE_HTTP_ERROR_INVALID_URL;

    return create_request(connection_id, method, url, content_length);
}

EXPORT(SceHttp, 0xB0284270, int, sceHttpCreateRequest, int connection_id, int method,
       const char *path, unsigned long long content_length)
{
    HttpConnection *connection = connection_for_id(connection_id);
    char url[HTTP_URL_MAX];
    int written;

    if (!connection)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!path)
        return SCE_HTTP_ERROR_INVALID_URL;

    written = snprintf(url, sizeof(url), "%s://%s:%u%s", connection->scheme, connection->host,
                       connection->port, path);
    if (written < 0 || (size_t)written >= sizeof(url))
        return SCE_HTTP_ERROR_INVALID_URL;

    return create_request(connection_id, method, url, content_length);
}

EXPORT(SceHttp, 0x3D3D29AD, int, sceHttpDeleteRequest, int request_id)
{
    HttpRequest *request = request_for_id(request_id);

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;

    free(request->response_headers);
    free(request->response_body);
    memset(request, 0, sizeof(*request));
    return 0;
}

EXPORT(SceHttp, 0x9CA58B99, int, sceHttpSendRequest, int request_id, const void *post_data,
       unsigned int post_size)
{
    HttpRequest *request = request_for_id(request_id);
    int result;

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;

    request->aborted = false;
    result = perform_request(request, post_data, post_size, HTTP_MAX_REDIRECTS);

    if (getenv("VITA2NATIVE_INPUT_DEBUG")) {
        fprintf(stderr, "vita2native: HTTP send %s -> %d\n", request->url, result);
        fflush(stderr);
    }

    return result;
}

EXPORT(SceHttp, 0x7EDE3979, int, sceHttpReadData, int request_id, void *data,
       unsigned int size)
{
    HttpRequest *request = request_for_id(request_id);
    size_t available;

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!data && size)
        return SCE_HTTP_ERROR_INVALID_VALUE;

    available = request->response_body_size - request->response_body_pos;
    if (size > available)
        size = (unsigned int)available;

    if (size) {
        memcpy(data, request->response_body + request->response_body_pos, size);
        request->response_body_pos += size;
    }

    return (int)size;
}

EXPORT(SceHttp, 0x27071691, int, sceHttpGetStatusCode, int request_id, int *status_code)
{
    HttpRequest *request = request_for_id(request_id);

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!status_code)
        return SCE_HTTP_ERROR_INVALID_VALUE;

    *status_code = request->status_code;
    return 0;
}

EXPORT(SceHttp, 0xF580D304, int, sceHttpGetResponseContentLength, int request_id,
       unsigned long long *content_length)
{
    HttpRequest *request = request_for_id(request_id);

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!content_length)
        return SCE_HTTP_ERROR_INVALID_VALUE;

    *content_length = request->response_content_length;
    return 0;
}

EXPORT(SceHttp, 0x11F6C27F, int, sceHttpGetAllResponseHeaders, int request_id, char **header,
       unsigned int *header_size)
{
    HttpRequest *request = request_for_id(request_id);

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!header || !header_size)
        return SCE_HTTP_ERROR_INVALID_VALUE;

    *header = request->response_headers;
    *header_size = request->response_headers_size;
    return 0;
}

EXPORT(SceHttp, 0x03A6C89E, int, sceHttpParseResponseHeader, const char *header,
       unsigned int header_length, const char *field_str, const char **field_value,
       unsigned int *value_length)
{
    const char *line;
    const char *end;
    size_t field_length;

    if (!header || !field_str || !field_value || !value_length)
        return SCE_HTTP_ERROR_PARSE_HTTP_INVALID_VALUE;

    field_length = strlen(field_str);
    line = header;
    end = header + header_length;

    while (line < end) {
        const char *line_end = line;

        while (line_end + 1 < end && !(line_end[0] == '\r' && line_end[1] == '\n'))
            line_end++;

        if ((size_t)(line_end - line) > field_length &&
            strncasecmp(line, field_str, field_length) == 0 && line[field_length] == ':') {
            const char *value = line + field_length + 1;

            while (value < line_end && (*value == ' ' || *value == '\t'))
                value++;
            *field_value = value;
            *value_length = (unsigned int)(line_end - value);
            return 0;
        }

        line = line_end + 2;
    }

    return SCE_HTTP_ERROR_PARSE_HTTP_NOT_FOUND;
}

EXPORT(SceHttp, 0x179C56DB, int, sceHttpParseStatusLine, const char *status_line,
       unsigned int line_length, int *http_major, int *http_minor, int *response_code,
       const char **reason_phrase, unsigned int *phrase_length)
{
    int major = 0, minor = 0, code = 0;
    const char *reason;
    const char *line_end;
    char *parse_end;

    if (!status_line || line_length == 0)
        return SCE_HTTP_ERROR_PARSE_HTTP_INVALID_VALUE;

    line_end = status_line + line_length;
    reason = status_line;

    while (reason < line_end && (*reason == ' ' || *reason == '\t'))
        reason++;
    if (strncasecmp(reason, "HTTP/", 5) != 0)
        return SCE_HTTP_ERROR_PARSE_HTTP_INVALID_RESPONSE;

    reason += 5;
    major = (int)strtol(reason, &parse_end, 10);
    if (parse_end == reason || *parse_end != '.')
        return SCE_HTTP_ERROR_PARSE_HTTP_INVALID_RESPONSE;

    reason = parse_end + 1;
    minor = (int)strtol(reason, &parse_end, 10);
    if (parse_end == reason)
        return SCE_HTTP_ERROR_PARSE_HTTP_INVALID_RESPONSE;

    reason = parse_end;
    while (reason < line_end && (*reason == ' ' || *reason == '\t'))
        reason++;
    code = (int)strtol(reason, &parse_end, 10);
    if (parse_end == reason)
        return SCE_HTTP_ERROR_PARSE_HTTP_INVALID_RESPONSE;

    reason = parse_end;
    while (reason < line_end && (*reason == ' ' || *reason == '\t'))
        reason++;

    if (http_major)
        *http_major = major;
    if (http_minor)
        *http_minor = minor;
    if (response_code)
        *response_code = code;
    if (reason_phrase)
        *reason_phrase = reason;
    if (phrase_length) {
        const char *end = reason;

        while (end < line_end && *end != '\r' && *end != '\n')
            end++;
        *phrase_length = (unsigned int)(end - reason);
    }

    return 0;
}

/* --------------------------------------------------------------- options */

EXPORT(SceHttp, 0x237CA86E, int, sceHttpSetConnectTimeOut, int id, unsigned int usec)
{
    HttpTemplate *tmpl = template_for_any_id(id);
    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;
    tmpl->connect_timeout_us = usec;
    return 0;
}

EXPORT(SceHttp, 0x8AE3F008, int, sceHttpSetSendTimeOut, int id, unsigned int usec)
{
    HttpTemplate *tmpl = template_for_any_id(id);
    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;
    tmpl->send_timeout_us = usec;
    return 0;
}

EXPORT(SceHttp, 0x94BF196E, int, sceHttpSetRecvTimeOut, int id, unsigned int usec)
{
    HttpTemplate *tmpl = template_for_any_id(id);
    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;
    tmpl->recv_timeout_us = usec;
    return 0;
}

EXPORT(SceHttp, 0x8455B5B3, int, sceHttpSetResolveTimeOut, int id, unsigned int usec)
{
    HttpTemplate *tmpl = template_for_any_id(id);

    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;

    tmpl->resolve_timeout_us = usec;
    return 0;
}

EXPORT(SceHttp, 0x9AB56EA7, int, sceHttpSetResolveRetry, int id, int retry)
{
    (void)retry;
    return template_for_any_id(id) ? 0 : SCE_HTTP_ERROR_INVALID_ID;
}

EXPORT(SceHttp, 0x34891C3F, int, sceHttpSetAutoRedirect, int id, int enable)
{
    HttpTemplate *tmpl = template_for_any_id(id);
    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;
    tmpl->auto_redirect = enable;
    return 0;
}

EXPORT(SceHttp, 0x6EAD73EB, int, sceHttpGetAutoRedirect, int id, int *enable)
{
    HttpTemplate *tmpl = template_for_any_id(id);

    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!enable)
        return SCE_HTTP_ERROR_INVALID_VALUE;

    *enable = tmpl->auto_redirect;
    return 0;
}

EXPORT(SceHttp, 0xFD76E1FB, int, sceHttpSetResponseHeaderMaxSize, int id, unsigned int size)
{
    HttpTemplate *tmpl = template_for_any_id(id);
    if (!tmpl)
        return SCE_HTTP_ERROR_INVALID_ID;
    tmpl->header_max_size = size;
    return 0;
}

EXPORT(SceHttp, 0x37C30C90, int, sceHttpSetRequestContentLength, int id,
       unsigned long long content_length)
{
    HttpRequest *request = request_for_id(id);
    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;
    request->request_content_length = content_length;
    return 0;
}

EXPORT(SceHttp, 0x7B51B122, int, sceHttpAddRequestHeader, int id, const char *name,
       const char *value, unsigned int mode)
{
    HttpRequest *request = request_for_id(id);

    (void)mode;

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!name || !value)
        return SCE_HTTP_ERROR_INVALID_VALUE;
    if (request->header_count >= 16)
        return SCE_HTTP_ERROR_OUT_OF_SIZE;

    snprintf(request->headers[request->header_count].name,
             sizeof(request->headers[request->header_count].name), "%s", name);
    snprintf(request->headers[request->header_count].value,
             sizeof(request->headers[request->header_count].value), "%s", value);
    request->header_count++;
    return 0;
}

EXPORT(SceHttp, 0x5EB5F548, int, sceHttpRemoveRequestHeader, int id, const char *name)
{
    HttpRequest *request = request_for_id(id);

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;
    if (!name)
        return SCE_HTTP_ERROR_INVALID_VALUE;

    for (int i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, name) == 0) {
            memmove(&request->headers[i], &request->headers[i + 1],
                    sizeof(request->headers[0]) * (size_t)(request->header_count - i - 1));
            request->header_count--;
            return 0;
        }
    }

    return SCE_HTTP_ERROR_NOT_FOUND;
}

EXPORT(SceHttp, 0xEA61662F, int, sceHttpAbortRequest, int id)
{
    HttpRequest *request = request_for_id(id);

    if (!request)
        return SCE_HTTP_ERROR_INVALID_ID;

    request->aborted = true;
    return 0;
}

EXPORT(SceHttp, 0x3D44A6E5, int, sceHttpAbortRequestForce, int id)
{
    return sceHttpAbortRequest(id);
}

EXPORT(SceHttp, 0x42E7DFF1, int, sceHttpAbortWaitRequest, int id)
{
    return sceHttpAbortRequest(id);
}

EXPORT(SceHttp, 0xC6D60403, int, sceHttpsDisableOption, unsigned int option)
{
    (void)option;
    return 0;
}

EXPORT(SceHttp, 0x2E77B201, int, sceHttpsDisableOption2, unsigned int option)
{
    (void)option;
    return 0;
}

EXPORT(SceHttp, 0x00659635, int, sceHttpsDisableOptionPrivate, unsigned int option)
{
    (void)option;
    return 0;
}

EXPORT(SceHttp, 0x9FBE2869, int, sceHttpsEnableOption, unsigned int option)
{
    (void)option;
    return 0;
}

EXPORT(SceHttp, 0x1E93E8EF, int, sceHttpsEnableOption2, unsigned int option)
{
    (void)option;
    return 0;
}

EXPORT(SceHttp, 0x72CB0741, int, sceHttpsEnableOptionPrivate, unsigned int option)
{
    (void)option;
    return 0;
}

EXPORT(SceHttp, 0x56C95D94, int, sceHttpsFreeCaList, SceHttpsCaList *list)
{
    (void)list;
    return 0;
}

EXPORT(SceHttp, 0xF71AA58D, int, sceHttpsGetCaList, SceHttpsCaList *list)
{
    (void)list;
    return 0;
}

EXPORT(SceHttp, 0x2B79BDE0, int, sceHttpsGetSslError, int id, int *error, unsigned int *detail)
{
    (void)id;
    if (error)
        *error = 0;
    if (detail)
        *detail = 0;
    return 0;
}

EXPORT(SceHttp, 0xAE8D7C33, int, sceHttpsLoadCert, int ca_cert_num,
       const SceHttpsData **ca_list, const SceHttpsData *cert, const SceHttpsData *private_key)
{
    (void)ca_cert_num;
    (void)ca_list;
    (void)cert;
    (void)private_key;
    return 0;
}

EXPORT(SceHttp, 0x8577833F, int, sceHttpsUnloadCert, void)
{
    return 0;
}

EXPORT(SceHttp, 0xA0926037, int, sceHttpsSetSslCallback, int id, SceHttpsCallback callback,
       void *user_arg)
{
    (void)id;
    (void)callback;
    (void)user_arg;
    return 0;
}

EXPORT(SceHttp, 0x1DA2A673, int, sceHttpUriEscape, char *out, unsigned int *require,
       unsigned int prepare, const char *input)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t needed = 0;
    const unsigned char *src = (const unsigned char *)input;

    if (!input)
        return SCE_HTTP_ERROR_INVALID_VALUE;

    for (; *src; src++) {
        if (isalnum(*src) || *src == '-' || *src == '_' || *src == '.' || *src == '~')
            needed++;
        else
            needed += 3;
    }

    if (require)
        *require = (unsigned int)needed + 1;
    if (!out || prepare < needed + 1)
        return SCE_HTTP_ERROR_OUT_OF_SIZE;

    src = (const unsigned char *)input;
    while (*src) {
        if (isalnum(*src) || *src == '-' || *src == '_' || *src == '.' || *src == '~') {
            *out++ = (char)*src;
        } else {
            *out++ = '%';
            *out++ = hex[*src >> 4];
            *out++ = hex[*src & 0xf];
        }
        src++;
    }
    *out = '\0';

    return 0;
}

EXPORT(SceHttp, 0x1274D318, int, sceHttpUriUnescape, char *out, unsigned int *require,
       unsigned int prepare, const char *input)
{
    size_t needed = 0;

    if (!input)
        return SCE_HTTP_ERROR_INVALID_VALUE;

    for (const char *p = input; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]))
            p += 2;
        needed++;
    }

    if (require)
        *require = (unsigned int)needed + 1;
    if (!out || prepare < needed + 1)
        return SCE_HTTP_ERROR_OUT_OF_SIZE;

    while (*input) {
        if (*input == '%' && isxdigit((unsigned char)input[1]) &&
            isxdigit((unsigned char)input[2])) {
            char digits[3] = { input[1], input[2], 0 };

            *out++ = (char)strtol(digits, NULL, 16);
            input += 3;
        } else {
            *out++ = *input++;
        }
    }
    *out = '\0';

    return 0;
}

/* Unimplemented cookie/auth/epoll/URI helpers. */
static int http_unimplemented(void)
{
    return SCE_HTTP_ERROR_INVALID_VALUE;
}

#include "SceHttp_exports.inc"

DECLARE_LIBRARY(SceHttp, 0xE8F15CDE);
