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

static bool is_multicast_address(struct in_addr address)
{
    uint32_t host_order = ntohl(address.s_addr);

    return host_order >= 0xe0000000UL && host_order <= 0xefffffffUL;
}

static int parse_interface_address(const char *interface_address, struct in_addr *address)
{
    if (interface_address == NULL || interface_address[0] == '\0' ||
        strcmp(interface_address, "0.0.0.0") == 0 || strcmp(interface_address, "*") == 0) {
        address->s_addr = htonl(INADDR_ANY);
        return 0;
    }

    if (inet_pton(AF_INET, interface_address, address) != 1) {
        fprintf(stderr, "invalid IPv4 interface address: %s\n", interface_address);
        return -1;
    }

    return 0;
}

int input_udp_open(input_udp_t *input, const char *url)
{
    return input_udp_open_with_interface(input, url, NULL);
}

int input_udp_open_with_interface(input_udp_t *input, const char *url, const char *interface_address)
{
    char host[128];
    uint16_t port;
    struct in_addr input_address;
    struct in_addr multicast_interface;
    struct sockaddr_in addr;
    int reuse = 1;
    bool is_multicast_input;
    int flags;

    memset(input, 0, sizeof(*input));
    input->fd = -1;

    if (parse_udp_url(url, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "invalid UDP input URL: %s\n", url);
        return -1;
    }

    if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
        input_address.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, &input_address) != 1) {
        fprintf(stderr, "invalid IPv4 address in UDP input URL: %s\n", url);
        return -1;
    }

    is_multicast_input = is_multicast_address(input_address);
    multicast_interface.s_addr = htonl(INADDR_ANY);

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

#ifdef SO_REUSEPORT
    if (setsockopt(input->fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt SO_REUSEPORT");
        input_udp_close(input);
        return -1;
    }
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (is_multicast_input || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        addr.sin_addr = input_address;
    }

    if (bind(input->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        input_udp_close(input);
        return -1;
    }

    if (is_multicast_input) {
        if (parse_interface_address(interface_address, &multicast_interface) != 0) {
            input_udp_close(input);
            return -1;
        }

        memset(&input->multicast_request, 0, sizeof(input->multicast_request));
        input->multicast_request.imr_multiaddr = input_address;
        input->multicast_request.imr_interface = multicast_interface;
        if (setsockopt(input->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &input->multicast_request, sizeof(input->multicast_request)) != 0) {
            perror("setsockopt IP_ADD_MEMBERSHIP");
            input_udp_close(input);
            return -1;
        }
        input->joined_multicast = true;
    }

    flags = fcntl(input->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(input->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        perror("fcntl");
        input_udp_close(input);
        return -1;
    }

    snprintf(input->url, sizeof(input->url), "%s", url);
    if (interface_address != NULL) {
        snprintf(input->interface_address, sizeof(input->interface_address), "%s", interface_address);
    }
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
        if (input->joined_multicast) {
            (void)setsockopt(input->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                             &input->multicast_request, sizeof(input->multicast_request));
            input->joined_multicast = false;
        }
        close(input->fd);
        input->fd = -1;
    }
}
