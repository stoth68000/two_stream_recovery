#include "output_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int output_udp_parse_url(const char *url, char *host, size_t host_size, uint16_t *port)
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

int output_udp_open(output_udp_t *output, const char *url)
{
    char host[128];
    uint16_t port;
    struct sockaddr_in *addr;

    memset(output, 0, sizeof(*output));
    output->fd = -1;
    output->addr_storage = calloc(1, sizeof(*output->addr_storage));
    if (output->addr_storage == NULL) {
        return -1;
    }

    if (output_udp_parse_url(url, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "invalid UDP output URL: %s\n", url);
        output_udp_close(output);
        return -1;
    }

    output->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (output->fd < 0) {
        perror("socket");
        output_udp_close(output);
        return -1;
    }

    addr = (struct sockaddr_in *)output->addr_storage;
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address in UDP output URL: %s\n", url);
        output_udp_close(output);
        return -1;
    }

    output->addr_len = sizeof(*addr);
    return 0;
}

int output_udp_flush(output_udp_t *output)
{
    size_t bytes = output->pending_packets * 188;
    ssize_t sent;

    if (output->pending_packets == 0) {
        return 0;
    }

    sent = sendto(output->fd, output->pending, bytes, 0,
                  (struct sockaddr *)output->addr_storage, output->addr_len);
    if (sent < 0 || (size_t)sent != bytes) {
        perror("sendto");
        return -1;
    }

    output->pending_packets = 0;
    return 0;
}

int output_udp_send_ts_packet(output_udp_t *output, const uint8_t packet[188])
{
    memcpy(output->pending + (output->pending_packets * 188), packet, 188);
    output->pending_packets++;

    if (output->pending_packets == OUTPUT_TS_PACKETS_PER_DATAGRAM) {
        return output_udp_flush(output);
    }

    return 0;
}

static int output_udp_sink_send_ts_packet(void *ctx, const uint8_t packet[TS_PACKET_SIZE])
{
    return output_udp_send_ts_packet((output_udp_t *)ctx, packet);
}

static int output_udp_sink_flush(void *ctx)
{
    return output_udp_flush((output_udp_t *)ctx);
}

packet_sink_t output_udp_as_packet_sink(output_udp_t *output)
{
    packet_sink_t sink;

    sink.ctx = output;
    sink.send_ts_packet = output_udp_sink_send_ts_packet;
    sink.flush = output_udp_sink_flush;
    return sink;
}

void output_udp_close(output_udp_t *output)
{
    if (output->fd >= 0) {
        close(output->fd);
        output->fd = -1;
    }
    free(output->addr_storage);
    output->addr_storage = NULL;
}
