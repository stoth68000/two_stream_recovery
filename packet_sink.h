#ifndef PACKET_SINK_H
#define PACKET_SINK_H

#include "ts_packet.h"

#include <stddef.h>

typedef struct packet_sink {
    void *ctx;
    int (*send_ts_packet)(void *ctx, const uint8_t packet[TS_PACKET_SIZE]);
    int (*flush)(void *ctx);
} packet_sink_t;

static inline int packet_sink_send_ts_packet(packet_sink_t *sink, const uint8_t packet[TS_PACKET_SIZE])
{
    return sink->send_ts_packet(sink->ctx, packet);
}

static inline int packet_sink_flush(packet_sink_t *sink)
{
    if (sink->flush == NULL) {
        return 0;
    }

    return sink->flush(sink->ctx);
}

#endif
