#include "input_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_udp_url(const char *url, char *host, size_t host_size, uint16_t *port)
{
    const char *prefix = "udp://";
    const char *address = url;
    const char *colon;
    unsigned long parsed_port;
    char *end = NULL;

    if (strncmp(url, prefix, strlen(prefix)) == 0) {
        address = url + strlen(prefix);
    }

    colon = strrchr(address, ':');
    if (colon == NULL || colon == address || colon[1] == '\0') {
        return -1;
    }

    if ((size_t)(colon - address) >= host_size) {
        return -1;
    }

    memcpy(host, address, (size_t)(colon - address));
    host[colon - address] = '\0';

    errno = 0;
    parsed_port = strtoul(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || parsed_port > 65535UL) {
        return -1;
    }

    *port = (uint16_t)parsed_port;
    return 0;
}

int input_udp_open(input_udp_t *input, const char *url)
{
    char host[128];
    uint16_t port;
    struct sockaddr_in addr;
    int reuse = 1;
    int flags;

    memset(input, 0, sizeof(*input));
    input->fd = -1;

    if (parse_udp_url(url, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "invalid UDP input URL: %s\n", url);
        return -1;
    }

    input->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (input->fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(input->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        input_udp_close(input);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address in UDP input URL: %s\n", url);
        input_udp_close(input);
        return -1;
    }

    if (bind(input->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        input_udp_close(input);
        return -1;
    }

    flags = fcntl(input->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(input->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        perror("fcntl");
        input_udp_close(input);
        return -1;
    }

    snprintf(input->url, sizeof(input->url), "%s", url);
    return 0;
}

ssize_t input_udp_receive(input_udp_t *input, uint8_t *buffer, size_t buffer_size)
{
    ssize_t received = recv(input->fd, buffer, buffer_size, 0);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    return received;
}

void input_udp_close(input_udp_t *input)
{
    if (input->fd >= 0) {
        close(input->fd);
        input->fd = -1;
    }
}
