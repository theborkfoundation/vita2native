/*
 * Exercises SceHttp against a tiny local HTTP/1.1 server: normal responses,
 * redirects, chunked bodies and status/header parsing.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <psp2/types.h>
#include <psp2/net/http.h>

int sceHttpInit(unsigned int poolSize);
int sceHttpTerm(void);
int sceHttpCreateTemplate(const char *userAgent, int httpVer, int autoProxyConf);
int sceHttpDeleteTemplate(int tmplId);
int sceHttpCreateConnectionWithURL(int tmplId, const char *url, int enableKeepalive);
int sceHttpDeleteConnection(int connId);
int sceHttpCreateRequestWithURL(int connId, int method, const char *url,
                                unsigned long long contentLength);
int sceHttpDeleteRequest(int reqId);
int sceHttpSendRequest(int reqId, const void *postData, unsigned int size);
int sceHttpReadData(int reqId, void *data, unsigned int size);
int sceHttpGetStatusCode(int reqId, int *statusCode);
int sceHttpGetResponseContentLength(int reqId, unsigned long long *contentLength);
int sceHttpGetAllResponseHeaders(int reqId, char **header, unsigned int *headerSize);
int sceHttpParseResponseHeader(const char *header, unsigned int headerLen, const char *fieldStr,
                               const char **fieldValue, unsigned int *valueLen);

typedef struct {
    int listen_fd;
    int port;
    int stop;
} TestServer;

static TestServer g_server;

static void respond(int client, const char *response, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t ret = send(client, response + sent, length - sent, 0);

        if (ret <= 0)
            return;
        sent += (size_t)ret;
    }
}

static void *server_thread(void *arg)
{
    TestServer *server = arg;

    while (!server->stop) {
        int client = accept(server->listen_fd, NULL, NULL);
        char request[2048];
        ssize_t got;

        if (client < 0)
            continue;

        got = recv(client, request, sizeof(request) - 1, 0);
        if (got <= 0) {
            close(client);
            continue;
        }
        request[got] = '\0';

        if (strstr(request, "GET /hello ")) {
            static const char body[] = "hello world";
            char response[256];
            int length = snprintf(response, sizeof(response),
                                  "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                                  "X-Test: yes\r\nConnection: close\r\n\r\n%s",
                                  sizeof(body) - 1, body);

            respond(client, response, (size_t)length);
        } else if (strstr(request, "GET /redirect ")) {
            static const char response[] = "HTTP/1.1 302 Found\r\nLocation: /hello\r\n"
                                           "Content-Length: 0\r\nConnection: close\r\n\r\n";

            respond(client, response, sizeof(response) - 1);
        } else if (strstr(request, "GET /chunked ")) {
            static const char response[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                                           "X-Test: yes\r\nConnection: close\r\n\r\n"
                                           "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";

            respond(client, response, sizeof(response) - 1);
        } else {
            static const char body[] = "not found";
            char response[256];
            int length = snprintf(response, sizeof(response),
                                  "HTTP/1.1 404 Not Found\r\nContent-Length: %zu\r\n"
                                  "Connection: close\r\n\r\n%s",
                                  sizeof(body) - 1, body);

            respond(client, response, (size_t)length);
        }

        close(client);
    }

    return NULL;
}

static int start_server(TestServer *server)
{
    struct sockaddr_in addr;
    socklen_t addr_length = sizeof(addr);
    pthread_t thread;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0)
        return -1;

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server->listen_fd, 8) != 0 ||
        getsockname(server->listen_fd, (struct sockaddr *)&addr, &addr_length) != 0) {
        close(server->listen_fd);
        return -1;
    }

    server->port = ntohs(addr.sin_port);
    server->stop = 0;

    if (pthread_create(&thread, NULL, server_thread, server) != 0) {
        close(server->listen_fd);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

static void stop_server(TestServer *server)
{
    server->stop = 1;
    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
}

#define CHECK(cond, message)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("http_smoke: %s (line %d)\n", message, __LINE__);                              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int fetch(const char *url, int expected_status, const char *expected_body)
{
    unsigned long long content_length = 0;
    unsigned int header_size = 0;
    char *headers = NULL;
    const char *value = NULL;
    unsigned int value_length = 0;
    char body[128] = { 0 };
    char full_url[512];
    int template_id, connection_id, request_id;
    int status = 0;
    int total = 0;

    snprintf(full_url, sizeof(full_url), "http://127.0.0.1:%d%s", g_server.port, url);

    template_id = sceHttpCreateTemplate("vita2native-test", 1, 0);
    CHECK(template_id > 0, "CreateTemplate failed");

    connection_id = sceHttpCreateConnectionWithURL(template_id, full_url, 1);
    CHECK(connection_id > 0, "CreateConnectionWithURL failed");

    request_id = sceHttpCreateRequestWithURL(connection_id, SCE_HTTP_METHOD_GET, full_url, 0);
    CHECK(request_id > 0, "CreateRequestWithURL failed");

    CHECK(sceHttpSendRequest(request_id, NULL, 0) == 0, "SendRequest failed");
    CHECK(sceHttpGetStatusCode(request_id, &status) == 0, "GetStatusCode failed");
    CHECK(status == expected_status, "unexpected HTTP status");

    CHECK(sceHttpGetAllResponseHeaders(request_id, &headers, &header_size) == 0,
          "GetAllResponseHeaders failed");
    CHECK(headers && header_size > 0, "the response headers are empty");

    if (expected_status == 200) {
        CHECK(sceHttpParseResponseHeader(headers, header_size, "X-Test", &value, &value_length) == 0,
              "ParseResponseHeader did not find X-Test");
        CHECK(value_length == 3 && strncmp(value, "yes", 3) == 0, "X-Test has the wrong value");

        CHECK(sceHttpGetResponseContentLength(request_id, &content_length) == 0,
              "GetResponseContentLength failed");
        CHECK(content_length == strlen(expected_body), "the content length is wrong");
    }

    for (;;) {
        int read = sceHttpReadData(request_id, body + total, (unsigned int)(sizeof(body) - 1 - total));

        CHECK(read >= 0, "ReadData failed");
        if (read == 0)
            break;
        total += read;
    }

    if (expected_body)
        CHECK(total == (int)strlen(expected_body) &&
                  memcmp(body, expected_body, (size_t)total) == 0,
              "the response body is wrong");

    CHECK(sceHttpDeleteRequest(request_id) == 0, "DeleteRequest failed");
    CHECK(sceHttpDeleteConnection(connection_id) == 0, "DeleteConnection failed");
    CHECK(sceHttpDeleteTemplate(template_id) == 0, "DeleteTemplate failed");

    return 0;
}

static int probe_external(const char *url)
{
    int template_id = sceHttpCreateTemplate("vita2native-probe", 1, 0);
    int connection_id, request_id, status = 0, total = 0;
    unsigned long long length = 0;
    char buffer[4096];
    int ret;

    if (template_id <= 0)
        return template_id;

    connection_id = sceHttpCreateConnectionWithURL(template_id, url, 1);
    if (connection_id <= 0)
        return connection_id;

    request_id = sceHttpCreateRequestWithURL(connection_id, SCE_HTTP_METHOD_GET, url, 0);
    if (request_id <= 0)
        return request_id;

    ret = sceHttpSendRequest(request_id, NULL, 0);
    if (ret == 0) {
        sceHttpGetStatusCode(request_id, &status);
        sceHttpGetResponseContentLength(request_id, &length);
        for (;;) {
            int read = sceHttpReadData(request_id, buffer, sizeof(buffer));

            if (read <= 0)
                break;
            total += read;
        }
    }

    sceHttpDeleteRequest(request_id);
    sceHttpDeleteConnection(connection_id);
    sceHttpDeleteTemplate(template_id);

    if (ret != 0) {
        printf("http_smoke: %s failed: 0x%x\n", url, (unsigned int)ret);
        return ret;
    }

    printf("http_smoke: %s -> status %d, %d bytes (content-length %llu)\n", url, status, total,
           (unsigned long long)length);
    return 0;
}

int main(int argc, char **argv)
{
    if (start_server(&g_server) != 0) {
        puts("http_smoke: could not start the local server");
        return 1;
    }

    CHECK(sceHttpInit(0) == 0, "Init failed");
    CHECK(fetch("/hello", 200, "hello world") == 0, "plain request failed");
    CHECK(fetch("/redirect", 200, "hello world") == 0, "redirect was not followed");
    CHECK(fetch("/chunked", 200, "hello world") == 0, "chunked body was not decoded");
    CHECK(fetch("/missing", 404, "not found") == 0, "404 response failed");

    if (argc > 1)
        probe_external(argv[1]);

    CHECK(sceHttpTerm() == 0, "Term failed");

    stop_server(&g_server);
    puts("http_smoke: ok");
    return 0;
}
