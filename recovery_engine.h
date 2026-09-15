#ifndef RECOVERY_ENGINE_H
#define RECOVERY_ENGINE_H

#include "packet_sink.h"
#include "report_stats.h"
#include "ts_packet.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

#define RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS 65536
#define RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS 2048
#define RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD 8
#define RECOVERY_ENGINE_DEFAULT_PRIMARY_DELAY_NS 2500000000ULL
#define RECOVERY_ENGINE_DEFAULT_MAX_SECONDARY_LATENCY_NS 5000000000ULL
#define RECOVERY_ENGINE_DEFAULT_ALIGNMENT_WINDOW_NS 6000000000ULL
#define RECOVERY_ENGINE_DEFAULT_HISTORY_MS 10000ULL
#define RECOVERY_ENGINE_DEFAULT_MIN_ALIGNMENT_CONFIDENCE 40U
#define RECOVERY_ENGINE_DEFAULT_PRIMARY_OUTAGE_NS 50000000ULL
#define RECOVERY_ENGINE_DEFAULT_PRIMARY_RETURN_NS 120000000000ULL
#define RECOVERY_ENGINE_MAX_PCR_PIDS 32
#define RECOVERY_ENGINE_DEFAULT_MAX_CONTENT_BURST_PACKETS 15U
#define RECOVERY_ENGINE_STREAM_GAP_MIN_NS 10000000ULL
#define RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS 1000000ULL
#define RECOVERY_ENGINE_MAX_STREAM_GAP_RECOVERY_PACKETS 4096U
#define RECOVERY_ENGINE_MIN_NULL_GAP_RECOVERY_PACKETS 7U
#define RECOVERY_ENGINE_PID_GAP_MIN_NS 250000000ULL
#define RECOVERY_ENGINE_MAX_PID_GAP_RECOVERY_PACKETS 131072U

typedef struct recovery_engine_config {
    uint64_t primary_delay_ns;
    uint64_t max_secondary_latency_ns;
    uint64_t alignment_window_ns;
    uint64_t history_ms;
    uint64_t primary_outage_ns;
    uint64_t primary_return_ns;
    uint32_t min_alignment_confidence;
    uint32_t max_content_burst_packets;
} recovery_engine_config_t;

typedef struct alignment_state {
    bool has_alignment;
    int64_t offset_packets;
    uint32_t confidence;
    uint32_t consecutive_matches;
    uint32_t consecutive_misses;
} alignment_state_t;

typedef struct primary_anchor {
    bool valid;
    uint64_t primary_index;
    uint64_t secondary_index;
} primary_anchor_t;

typedef struct primary_gap_state {
    bool valid;
    uint64_t arrival_time_ns;
    uint64_t last_recovered_secondary_arrival_ns;
    uint64_t recovered_stream_packets;
    uint64_t secondary_recovery_index;
    uint64_t continuity_errors;
    uint64_t duplicate_counters;
} primary_gap_state_t;

typedef struct output_pid_state {
    bool valid;
    uint8_t continuity_counter;
    uint64_t last_arrival_time_ns;
    uint64_t last_recovered_secondary_arrival_ns;
    uint64_t last_primary_continuity_errors;
} output_pid_state_t;

typedef struct input_pid_state {
    bool valid;
    uint8_t continuity_counter;
} input_pid_state_t;

typedef struct packet_record {
    int source_stream_id;
    uint64_t stream_index;
    struct timeval arrival_time;
    uint64_t arrival_time_ns;
    uint16_t pid;
    uint8_t continuity_counter;
    bool has_payload;
    bool has_pcr;
    bool is_null;
    bool transport_error;
    bool discontinuity_indicator;
    uint64_t stream_continuity_errors;
    uint64_t stream_duplicate_counters;
    uint64_t pcr_value;
    uint64_t hash;
    uint8_t packet[TS_PACKET_SIZE];
} packet_record_t;

typedef struct packet_history {
    packet_record_t *records;
    size_t capacity;
    size_t start;
    size_t count;
} packet_history_t;

typedef struct primary_delay_queue {
    packet_record_t *records;
    size_t capacity;
    size_t start;
    size_t count;
    uint64_t delay_ns;
} primary_delay_queue_t;

typedef struct pcr_pid_model {
    bool active;
    uint16_t pid;
    uint64_t last_pcr;
    uint64_t last_stream_index;
    uint64_t last_arrival_time_ns;
    double bitrate_bps;
    double packets_per_second;
    double jitter_ns;
    uint32_t confidence;
} pcr_pid_model_t;

typedef struct pcr_timing_model {
    pcr_pid_model_t pid_models[RECOVERY_ENGINE_MAX_PCR_PIDS];
    double estimated_delay_ns;
    uint32_t confidence;
} pcr_timing_model_t;

typedef struct recovery_engine {
    packet_sink_t *sink;
    report_stats_t *stats;
    recovery_engine_config_t config;
    packet_history_t history[2];
    primary_delay_queue_t primary_queue;
    primary_delay_queue_t secondary_queue;
    pcr_timing_model_t pcr_model[2];
    uint64_t next_stream_index[2];
    uint64_t last_input_arrival_ns[2];
    uint64_t last_output_stream_index[2];
    bool has_output_stream_index[2];
    int active_output_stream_id;
    uint64_t primary_return_start_ns;
    bool primary_switchback_guard;
    uint32_t primary_switchback_guard_informative;
    alignment_state_t alignment;
    primary_anchor_t last_primary_anchor;
    primary_gap_state_t primary_gap;
    input_pid_state_t input_pid_state[REPORT_STATS_STREAMS][REPORT_STATS_PIDS];
    output_pid_state_t output_pid_state[REPORT_STATS_PIDS];
} recovery_engine_t;

recovery_engine_config_t recovery_engine_default_config(void);
int recovery_engine_init(recovery_engine_t *engine, packet_sink_t *sink, report_stats_t *stats);
int recovery_engine_init_with_config(recovery_engine_t *engine, packet_sink_t *sink, report_stats_t *stats,
                                     const recovery_engine_config_t *config);
int recovery_engine_push_packet(recovery_engine_t *engine, int stream_id, const uint8_t packet[TS_PACKET_SIZE],
                                const ts_packet_info_t *info);
int recovery_engine_drain(recovery_engine_t *engine, bool force);
int recovery_engine_flush(recovery_engine_t *engine);
void recovery_engine_free(recovery_engine_t *engine);

#endif
