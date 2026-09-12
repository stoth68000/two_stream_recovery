#ifndef RECOVERY_ENGINE_H
#define RECOVERY_ENGINE_H

#include "output_udp.h"
#include "report_stats.h"
#include "ts_packet.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS 65536

typedef struct packet_record {
    uint64_t stream_index;
    uint64_t arrival_time_ns;
    uint16_t pid;
    uint8_t continuity_counter;
    bool has_payload;
    bool has_pcr;
    bool is_null;
    bool transport_error;
    uint64_t hash;
    uint8_t packet[TS_PACKET_SIZE];
} packet_record_t;

typedef struct packet_history {
    packet_record_t *records;
    size_t capacity;
    size_t start;
    size_t count;
} packet_history_t;

typedef struct recovery_engine {
    output_udp_t *output;
    report_stats_t *stats;
    packet_history_t history[2];
    uint64_t next_stream_index[2];
} recovery_engine_t;

int recovery_engine_init(recovery_engine_t *engine, output_udp_t *output, report_stats_t *stats);
int recovery_engine_push_packet(recovery_engine_t *engine, int stream_id, const uint8_t packet[TS_PACKET_SIZE],
                                const ts_packet_info_t *info);
int recovery_engine_flush(recovery_engine_t *engine);
void recovery_engine_free(recovery_engine_t *engine);

#endif
