#ifndef OUTPUT_UDP_H
#define OUTPUT_UDP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "packet_sink.h"

#define OUTPUT_TS_PACKETS_PER_DATAGRAM 7
#define OUTPUT_UDP_PAYLOAD_BYTES (TS_PACKET_SIZE * OUTPUT_TS_PACKETS_PER_DATAGRAM)

typedef struct output_udp {
    int fd;
    struct sockaddr_storage *addr_storage;
    socklen_t addr_len;
    uint8_t pending[OUTPUT_UDP_PAYLOAD_BYTES];
    size_t pending_packets;
} output_udp_t;

int output_udp_parse_url(const char *url, char *host, size_t host_size, uint16_t *port);
int output_udp_open(output_udp_t *output, const char *url);
int output_udp_send_ts_packet(output_udp_t *output, const uint8_t packet[TS_PACKET_SIZE]);
int output_udp_flush(output_udp_t *output);
packet_sink_t output_udp_as_packet_sink(output_udp_t *output);
void output_udp_close(output_udp_t *output);

#endif
