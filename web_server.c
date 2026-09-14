#include "web_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define HTTP_REQUEST_SIZE 2048
#define HTTP_RESPONSE_SIZE 16384
#define HTTP_FILE_SIZE 65536

static const char *content_type_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(dot, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(dot, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    return "application/octet-stream";
}

static void send_response(int client_fd, int status, const char *reason,
                          const char *content_type, const char *body, size_t body_size)
{
    char header[512];
    int header_size;

    header_size = snprintf(header, sizeof(header),
                           "HTTP/1.1 %d %s\r\n"
                           "Connection: close\r\n"
                           "Cache-Control: no-store\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "\r\n",
                           status, reason, content_type, body_size);
    if (header_size > 0) {
        (void)send(client_fd, header, (size_t)header_size, 0);
    }
    if (body_size > 0) {
        (void)send(client_fd, body, body_size, 0);
    }
}

static void send_error(int client_fd, int status, const char *reason)
{
    send_response(client_fd, status, reason, "text/plain; charset=utf-8",
                  reason, strlen(reason));
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool wait_for_client_request(int fd)
{
    fd_set read_fds;
    struct timeval timeout;
    int ready;

    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;

    ready = select(fd + 1, &read_fds, NULL, NULL, &timeout);
    return ready > 0 && FD_ISSET(fd, &read_fds);
}

int web_server_open(web_server_t *server, uint16_t port, const char *webroot)
{
    struct sockaddr_in addr;
    int reuse = 1;

    memset(server, 0, sizeof(*server));
    server->fd = -1;
    snprintf(server->webroot, sizeof(server->webroot), "%s", webroot);

    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->fd < 0) {
        perror("socket http");
        return -1;
    }
    if (setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt http");
        web_server_close(server);
        return -1;
    }
    if (set_nonblocking(server->fd) != 0) {
        perror("fcntl http");
        web_server_close(server);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(server->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind http");
        web_server_close(server);
        return -1;
    }
    if (listen(server->fd, 16) != 0) {
        perror("listen http");
        web_server_close(server);
        return -1;
    }

    printf("HTTP stats UI listening on http://127.0.0.1:%u/\n", port);
    fflush(stdout);
    return 0;
}

static void handle_api_stats(int client_fd, report_stats_t *stats)
{
    char body[HTTP_RESPONSE_SIZE];
    int body_size = report_stats_format_json(stats, body, sizeof(body));

    if (body_size < 0) {
        send_error(client_fd, 500, "stats json too large");
        return;
    }
    send_response(client_fd, 200, "OK", "application/json; charset=utf-8",
                  body, (size_t)body_size);
}

static void handle_static_file(web_server_t *server, int client_fd, const char *url_path)
{
    char path[512];
    char body[HTTP_FILE_SIZE];
    FILE *file;
    size_t bytes;
    struct stat st;

    if (strstr(url_path, "..") != NULL) {
        send_error(client_fd, 400, "bad request");
        return;
    }
    if (strcmp(url_path, "/") == 0) {
        url_path = "/index.html";
    }

    if (snprintf(path, sizeof(path), "%s%s", server->webroot, url_path) >= (int)sizeof(path)) {
        send_error(client_fd, 414, "uri too long");
        return;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_error(client_fd, 404, "not found");
        return;
    }
    if (st.st_size < 0 || st.st_size > HTTP_FILE_SIZE) {
        send_error(client_fd, 413, "file too large");
        return;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        send_error(client_fd, 404, "not found");
        return;
    }
    bytes = fread(body, 1, sizeof(body), file);
    fclose(file);

    send_response(client_fd, 200, "OK", content_type_for_path(path), body, bytes);
}

static void handle_client(web_server_t *server, int client_fd, report_stats_t *stats)
{
    char request[HTTP_REQUEST_SIZE];
    char method[8];
    char path[256];
    ssize_t received;

    received = recv(client_fd, request, sizeof(request) - 1U, 0);
    if (received <= 0) {
        return;
    }
    request[received] = '\0';

    if (sscanf(request, "%7s %255s", method, path) != 2) {
        send_error(client_fd, 400, "bad request");
        return;
    }
    if (strcmp(method, "GET") != 0) {
        send_error(client_fd, 405, "method not allowed");
        return;
    }
    if (strcmp(path, "/api/stats") == 0) {
        handle_api_stats(client_fd, stats);
        return;
    }
    handle_static_file(server, client_fd, path);
}

void web_server_handle_ready(web_server_t *server, report_stats_t *stats)
{
    for (;;) {
        int client_fd = accept(server->fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept http");
                if (errno == ENOTSOCK || errno == EBADF) {
                    web_server_close(server);
                }
            }
            return;
        }
        if (!wait_for_client_request(client_fd)) {
            close(client_fd);
            continue;
        }
        handle_client(server, client_fd, stats);
        close(client_fd);
    }
}

void web_server_close(web_server_t *server)
{
    if (server->fd >= 0) {
        close(server->fd);
        server->fd = -1;
    }
}
