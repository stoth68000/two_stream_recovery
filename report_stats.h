#ifndef REPORT_STATS_H
#define REPORT_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ts_packet.h"

#define REPORT_STATS_STREAMS 2
#define REPORT_STATS_PIDS 8192
#define REPORT_STATS_ROLLING_SECONDS 60

typedef enum stream_health {
    STREAM_HEALTH_HEALTHY = 0,
    STREAM_HEALTH_DEGRADED,
    STREAM_HEALTH_LOSSY,
    STREAM_HEALTH_UNTRUSTED
} stream_health_t;

typedef enum recovery_reject_reason {
    RECOVERY_REJECT_LOW_ALIGNMENT = 0,
    RECOVERY_REJECT_SECONDARY_LATE,
    RECOVERY_REJECT_TEI,
    RECOVERY_REJECT_DISCONTINUITY,
    RECOVERY_REJECT_WRONG_PID,
    RECOVERY_REJECT_WRONG_COUNTER,
    RECOVERY_REJECT_AMBIGUOUS,
    RECOVERY_REJECT_BURST_TOO_LARGE,
    RECOVERY_REJECT_MISSING_CANDIDATE,
    RECOVERY_REJECT_COUNT
} recovery_reject_reason_t;

typedef struct report_stats_sample {
    uint64_t packets_received[REPORT_STATS_STREAMS];
    uint64_t continuity_errors[REPORT_STATS_STREAMS];
    uint64_t recovered_packets;
    uint64_t unrecoverable_loss;
    uint64_t output_packets;
    uint32_t alignment_confidence;
} report_stats_sample_t;

typedef struct report_stats {
    uint64_t input_datagrams[REPORT_STATS_STREAMS];
    uint64_t malformed_datagrams[REPORT_STATS_STREAMS];
    uint64_t partial_datagrams[REPORT_STATS_STREAMS];
    uint64_t resync_events[REPORT_STATS_STREAMS];
    uint64_t output_datagrams;
    uint64_t output_short_flushes;
    uint64_t packets_received[REPORT_STATS_STREAMS];
    uint64_t sync_errors[REPORT_STATS_STREAMS];
    uint64_t transport_errors[REPORT_STATS_STREAMS];
    uint64_t continuity_errors[REPORT_STATS_STREAMS];
    uint64_t duplicate_counters[REPORT_STATS_STREAMS];
    uint64_t null_packets[REPORT_STATS_STREAMS];
    uint64_t pcr_packets[REPORT_STATS_STREAMS];
    uint64_t stream_disagreements;
    uint64_t primary_delay_overflows;
    uint64_t recovered_null_packets;
    uint64_t recovered_content_packets;
    uint64_t recovered_content_bursts;
    uint64_t unrecoverable_null_regions;
    uint64_t secondary_loss_events;
    uint64_t secondary_missing_packets;
    uint64_t secondary_missing_anchors;
    uint32_t pcr_timing_confidence[REPORT_STATS_STREAMS];
    double pcr_bitrate_bps[REPORT_STATS_STREAMS];
    double pcr_delay_ns;
    double observed_secondary_latency_min_ns;
    double observed_secondary_latency_avg_ns;
    double observed_secondary_latency_max_ns;
    double observed_secondary_latency_jitter_ns;
    uint64_t observed_secondary_latency_samples;
    uint64_t secondary_packets_too_late;
    uint64_t primary_delay_insufficient;
    uint64_t recovery_rejects[RECOVERY_REJECT_COUNT];
    uint64_t recovery_exact_cc_gap;
    stream_health_t stream_health[REPORT_STATS_STREAMS];
    int64_t alignment_offset_packets;
    uint32_t alignment_confidence;
    uint64_t per_pid_packets[REPORT_STATS_STREAMS][REPORT_STATS_PIDS];
    uint64_t recovered_packets;
    uint64_t unrecoverable_loss;
    uint64_t output_packets;
    uint64_t last_report_ns;
    report_stats_sample_t rolling_samples[REPORT_STATS_ROLLING_SECONDS];
    size_t rolling_index;
    size_t rolling_count;
    uint8_t last_continuity_counter[REPORT_STATS_STREAMS][REPORT_STATS_PIDS];
    bool has_continuity_counter[REPORT_STATS_STREAMS][REPORT_STATS_PIDS];
} report_stats_t;

uint64_t report_stats_now_ns(void);
void report_stats_init(report_stats_t *stats);
void report_stats_observe_datagram(report_stats_t *stats, int stream_id, size_t bytes);
void report_stats_observe_packet(report_stats_t *stats, int stream_id, const ts_packet_info_t *info);
void report_stats_observe_output_datagram(report_stats_t *stats, bool short_flush);
void report_stats_observe_latency(report_stats_t *stats, uint64_t primary_arrival_ns,
                                  uint64_t secondary_arrival_ns, uint64_t max_latency_ns);
void report_stats_reject_recovery(report_stats_t *stats, recovery_reject_reason_t reason);
void report_stats_maybe_print(report_stats_t *stats, bool force);

#endif
