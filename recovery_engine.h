#ifndef RECOVERY_ENGINE_H
#define RECOVERY_ENGINE_H

#include "output_udp.h"
#include "report_stats.h"
#include "ts_packet.h"

#include <stddef.h>
#include <stdint.h>

typedef struct recovery_engine {
    output_udp_t *output;
    report_stats_t *stats;
} recovery_engine_t;

void recovery_engine_init(recovery_engine_t *engine, output_udp_t *output, report_stats_t *stats);
int recovery_engine_push_packet(recovery_engine_t *engine, int stream_id, const uint8_t packet[TS_PACKET_SIZE]);
int recovery_engine_flush(recovery_engine_t *engine);

#endif
