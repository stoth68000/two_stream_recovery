#ifndef REPORT_STATS_H
#define REPORT_STATS_H

#include <stdbool.h>
#include <stdint.h>

#include "ts_packet.h"

#define REPORT_STATS_STREAMS 2
#define REPORT_STATS_PIDS 8192

typedef struct report_stats {
    uint64_t packets_received[REPORT_STATS_STREAMS];
    uint64_t sync_errors[REPORT_STATS_STREAMS];
    uint64_t transport_errors[REPORT_STATS_STREAMS];
    uint64_t continuity_errors[REPORT_STATS_STREAMS];
    uint64_t duplicate_counters[REPORT_STATS_STREAMS];
    uint64_t null_packets[REPORT_STATS_STREAMS];
    uint64_t pcr_packets[REPORT_STATS_STREAMS];
    uint64_t stream_disagreements;
    uint64_t primary_delay_overflows;
    uint32_t pcr_timing_confidence[REPORT_STATS_STREAMS];
    double pcr_bitrate_bps[REPORT_STATS_STREAMS];
    double pcr_delay_ns;
    int64_t alignment_offset_packets;
    uint32_t alignment_confidence;
    uint64_t per_pid_packets[REPORT_STATS_STREAMS][REPORT_STATS_PIDS];
    uint64_t recovered_packets;
    uint64_t unrecoverable_loss;
    uint64_t output_packets;
    uint64_t last_report_ns;
    uint8_t last_continuity_counter[REPORT_STATS_STREAMS][REPORT_STATS_PIDS];
    bool has_continuity_counter[REPORT_STATS_STREAMS][REPORT_STATS_PIDS];
} report_stats_t;

uint64_t report_stats_now_ns(void);
void report_stats_init(report_stats_t *stats);
void report_stats_observe_packet(report_stats_t *stats, int stream_id, const ts_packet_info_t *info);
void report_stats_maybe_print(report_stats_t *stats, bool force);

#endif
