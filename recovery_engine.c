#include "recovery_engine.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OUTPUT_TS_PACKETS_PER_DATAGRAM 7U
#define STAGE1_BOUNDARY_SEARCH_EXTRA 8U
#define OVERSIZE_AFTER_ANCHOR_SCAN_PACKETS 255ULL
#define WIDE_ALIGNMENT_PROBE_INTERVAL 512ULL
#define RECOVERY_DECISION_NONE 0
#define RECOVERY_DECISION_INSERTED 1
#define RECOVERY_DECISION_REJECTED 2
#define MAX_SECONDARY_AFTER_ANCHOR_CANDIDATES 64U

typedef struct secondary_after_anchor_candidates {
    packet_record_t *records[MAX_SECONDARY_AFTER_ANCHOR_CANDIDATES];
    size_t count;
    bool truncated;
} secondary_after_anchor_candidates_t;

typedef struct recovery_candidate_validation {
    bool valid;
    recovery_reject_reason_t reject_reason;
    const char *reason;
    uint64_t missing_packets;
    bool has_missing_count;
} recovery_candidate_validation_t;

static uint64_t hash_packet(const uint8_t packet[TS_PACKET_SIZE])
{
    uint64_t hash = 1469598103934665603ULL;
    size_t i;

    for (i = 0; i < TS_PACKET_SIZE; i++) {
        hash ^= packet[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void format_packet_timestamp(const packet_record_t *record, char *buffer, size_t buffer_size)
{
    time_t seconds = record->arrival_time.tv_sec;
    struct tm tm_now;

    localtime_r(&seconds, &tm_now);
    strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%S%z", &tm_now);
}

static const char *yes_no(bool value)
{
    return value ? "yes" : "no";
}

static void recovery_decision_log(const recovery_engine_t *engine,
                                  int stream_id,
                                  const packet_record_t *primary,
                                  uint8_t previous_cc,
                                  bool has_previous_cc,
                                  uint64_t missing_count,
                                  bool has_missing_count,
                                  bool candidate_available,
                                  bool before_anchor,
                                  bool after_anchor,
                                  const char *decision,
                                  const char *reason)
{
    char timestamp[64];

    format_packet_timestamp(primary, timestamp, sizeof(timestamp));
    printf("recovery_decision ts=%s.%06ld path=%s hash=%016" PRIx64
           " idx=%" PRIu64 " pid=0x%04x cc=",
           timestamp, (long)primary->arrival_time.tv_usec,
           stream_id == 0 ? "primary" : "secondary", primary->hash,
           primary->stream_index, primary->pid);
    if (has_previous_cc) {
        printf("%u->%u", previous_cc, primary->continuity_counter);
    } else {
        printf("unknown->%u", primary->continuity_counter);
    }
    if (has_missing_count) {
        printf(" missing=%" PRIu64, missing_count);
    } else {
        printf(" missing=unknown");
    }
    printf(" align=%u candidate=%s candidates=%zu truncated=%s before=%s after=%s decision=%s",
           engine->alignment.confidence, yes_no(candidate_available),
           engine->decision_candidate_count,
           yes_no(engine->decision_candidates_truncated),
           yes_no(before_anchor), yes_no(after_anchor), decision);
    if (reason != NULL && reason[0] != '\0') {
        printf(" reason=%s", reason);
    }
    printf("\n");
    fflush(stdout);
}

static void observe_unrecoverable_primary_gap(recovery_engine_t *engine,
                                              const packet_record_t *primary,
                                              uint64_t missing_count,
                                              bool has_missing_count,
                                              const char *reason)
{
    if (!has_missing_count || missing_count == 0 ||
        primary->is_null || primary->transport_error ||
        primary->discontinuity_indicator) {
        return;
    }
    if (reason != NULL && strncmp(reason, "null_", 5) == 0) {
        return;
    }
    engine->stats->unrecoverable_loss += missing_count;
}

static void recovery_reject_log(recovery_engine_t *engine,
                                const packet_record_t *primary,
                                uint8_t previous_cc,
                                bool has_previous_cc,
                                uint64_t missing_count,
                                bool has_missing_count,
                                bool candidate_available,
                                bool before_anchor,
                                bool after_anchor,
                                recovery_reject_reason_t reject_reason,
                                const char *reason)
{
    recovery_decision_log(engine, 0, primary, previous_cc, has_previous_cc,
                          missing_count, has_missing_count, candidate_available,
                          before_anchor, after_anchor, "reject", reason);
    report_stats_reject_recovery(engine->stats, reject_reason);
    observe_unrecoverable_primary_gap(engine, primary, missing_count,
                                      has_missing_count, reason);
}

static int packet_history_init(packet_history_t *history, size_t capacity)
{
    history->records = calloc(capacity, sizeof(*history->records));
    if (history->records == NULL) {
        return -1;
    }
    history->capacity = capacity;
    history->start = 0;
    history->count = 0;
    return 0;
}

static void packet_history_free(packet_history_t *history)
{
    free(history->records);
    memset(history, 0, sizeof(*history));
}

static packet_record_t *packet_history_push(packet_history_t *history)
{
    size_t index;

    if (history->count < history->capacity) {
        index = (history->start + history->count) % history->capacity;
        history->count++;
    } else {
        index = history->start;
        history->start = (history->start + 1U) % history->capacity;
    }

    return &history->records[index];
}

static void packet_history_prune_older_than(packet_history_t *history, uint64_t cutoff_ns)
{
    while (history->count > 0) {
        packet_record_t *oldest = &history->records[history->start];

        if (oldest->arrival_time_ns >= cutoff_ns) {
            break;
        }
        history->start = (history->start + 1U) % history->capacity;
        history->count--;
    }
}

static packet_record_t *packet_history_find_index(packet_history_t *history, uint64_t stream_index)
{
    uint64_t oldest_stream_index;
    uint64_t offset;
    size_t index;

    if (history->count == 0) {
        return NULL;
    }

    oldest_stream_index = history->records[history->start].stream_index;
    if (stream_index < oldest_stream_index) {
        return NULL;
    }
    offset = stream_index - oldest_stream_index;
    if (offset >= history->count) {
        return NULL;
    }

    index = (history->start + (size_t)offset) % history->capacity;
    if (history->records[index].stream_index != stream_index) {
        return NULL;
    }
    return &history->records[index];
}

static int delay_queue_init(primary_delay_queue_t *queue, size_t capacity, uint64_t delay_ns)
{
    queue->records = calloc(capacity, sizeof(*queue->records));
    if (queue->records == NULL) {
        return -1;
    }
    queue->capacity = capacity;
    queue->start = 0;
    queue->count = 0;
    queue->delay_ns = delay_ns;
    return 0;
}

static void delay_queue_free(primary_delay_queue_t *queue)
{
    free(queue->records);
    memset(queue, 0, sizeof(*queue));
}

static packet_record_t *delay_queue_front(primary_delay_queue_t *queue)
{
    if (queue->count == 0) {
        return NULL;
    }
    return &queue->records[queue->start];
}

static void delay_queue_pop(primary_delay_queue_t *queue)
{
    if (queue->count == 0) {
        return;
    }
    queue->start = (queue->start + 1U) % queue->capacity;
    queue->count--;
}

static int delay_queue_push(primary_delay_queue_t *queue, const packet_record_t *record)
{
    size_t index;

    if (queue->count >= queue->capacity) {
        return -1;
    }

    index = (queue->start + queue->count) % queue->capacity;
    queue->records[index] = *record;
    queue->count++;
    return 0;
}

static bool record_is_informative(const packet_record_t *record)
{
    return !record->is_null && !record->transport_error && !record->discontinuity_indicator;
}

static bool records_match(const packet_record_t *a, const packet_record_t *b)
{
    return record_is_informative(a) && record_is_informative(b) &&
           a->hash == b->hash && a->pid == b->pid &&
           a->continuity_counter == b->continuity_counter;
}

static bool records_exact_non_error_match(const packet_record_t *a, const packet_record_t *b)
{
    return !a->transport_error && !b->transport_error &&
           !a->discontinuity_indicator && !b->discontinuity_indicator &&
           a->hash == b->hash && a->pid == b->pid &&
           a->continuity_counter == b->continuity_counter;
}

static bool records_are_non_null_exact_anchor(const packet_record_t *a, const packet_record_t *b)
{
    return a != NULL && b != NULL && !a->is_null && !b->is_null &&
           records_exact_non_error_match(a, b);
}

static packet_record_t *find_aligned_secondary_record(recovery_engine_t *engine,
                                                      const packet_record_t *primary)
{
    int64_t expected_index;

    if (!engine->alignment.has_alignment) {
        return NULL;
    }
    expected_index = (int64_t)primary->stream_index + engine->alignment.offset_packets;
    if (expected_index < 0) {
        return NULL;
    }
    return packet_history_find_index(&engine->history[1], (uint64_t)expected_index);
}

static bool record_fits_output_states(const output_pid_state_t states[REPORT_STATS_PIDS],
                                      const packet_record_t *record)
{
    const output_pid_state_t *state;
    uint8_t expected;

    if (record->is_null) {
        return true;
    }
    if (record->transport_error || record->discontinuity_indicator) {
        return false;
    }

    state = &states[record->pid];
    if (!state->valid) {
        return true;
    }
    if (!record->has_payload) {
        return record->continuity_counter == state->continuity_counter;
    }

    expected = (uint8_t)((state->continuity_counter + 1U) & 0x0fU);
    return record->continuity_counter == expected;
}

static bool record_fits_next_output(recovery_engine_t *engine, const packet_record_t *record)
{
    return record_fits_output_states(engine->output_pid_state, record);
}

static bool previous_output_cc_for_record(const recovery_engine_t *engine,
                                          const packet_record_t *record,
                                          uint8_t *previous_cc)
{
    const output_pid_state_t *state;

    if (record->is_null || record->pid >= REPORT_STATS_PIDS) {
        return false;
    }
    state = &engine->output_pid_state[record->pid];
    if (!state->valid) {
        return false;
    }
    *previous_cc = state->continuity_counter;
    return true;
}

static bool infer_missing_from_cc(uint8_t previous_cc, const packet_record_t *record,
                                  uint64_t *missing_count)
{
    uint8_t delta;

    if (!record->has_payload || record->is_null) {
        return false;
    }

    delta = (uint8_t)((record->continuity_counter + 16U - previous_cc) & 0x0fU);
    if (delta == 0) {
        return false;
    }
    *missing_count = (uint64_t)(delta - 1U);
    return true;
}

static void observe_input_path_gap(recovery_engine_t *engine, int stream_id,
                                   const packet_record_t *record)
{
    input_pid_state_t *state;
    uint64_t missing_count = 0;
    bool has_missing_count = false;

    if (record->is_null || record->transport_error || record->discontinuity_indicator ||
        record->pid >= REPORT_STATS_PIDS) {
        return;
    }

    state = &engine->input_pid_state[stream_id][record->pid];
    if (state->valid) {
        bool gap = false;

        if (record->has_payload) {
            uint8_t expected = (uint8_t)((state->continuity_counter + 1U) & 0x0fU);

            gap = record->continuity_counter != state->continuity_counter &&
                  record->continuity_counter != expected;
            has_missing_count = infer_missing_from_cc(state->continuity_counter, record,
                                                      &missing_count);
        } else {
            gap = record->continuity_counter != state->continuity_counter;
        }

        if (gap && stream_id == 1) {
            engine->stats->secondary_loss_events++;
            engine->stats->secondary_missing_packets += has_missing_count ? missing_count : 1U;
            recovery_decision_log(engine, stream_id, record, state->continuity_counter,
                                  true, missing_count, has_missing_count, false, false, false,
                                  "reject", "secondary_input_gap");
        }
    }

    state->valid = true;
    state->continuity_counter = record->continuity_counter;
}

static void observe_output_states(output_pid_state_t states[REPORT_STATS_PIDS],
                                  const packet_record_t *record)
{
    output_pid_state_t *state;

    if (record->is_null || record->transport_error || record->discontinuity_indicator) {
        return;
    }

    state = &states[record->pid];
    state->valid = true;
    state->continuity_counter = record->continuity_counter;
    state->last_arrival_time_ns = record->arrival_time_ns;
    if (record->source_stream_id == 0) {
        state->last_primary_continuity_errors = record->stream_continuity_errors;
    }
}

static void observe_output_record(recovery_engine_t *engine, const packet_record_t *record)
{
    output_pid_state_t *state;
    uint8_t expected;

    if (record->is_null) {
        return;
    }

    state = &engine->output_pid_state[record->pid];
    if (state->valid && !record->transport_error && !record->discontinuity_indicator) {
        if (record->has_payload) {
            expected = (uint8_t)((state->continuity_counter + 1U) & 0x0fU);
            if (record->continuity_counter == state->continuity_counter) {
                engine->stats->output_duplicate_counters++;
            } else if (record->continuity_counter != expected) {
                engine->stats->output_continuity_errors++;
            }
        } else if (record->continuity_counter != state->continuity_counter) {
            engine->stats->output_continuity_errors++;
        }
    }

    state->valid = true;
    state->continuity_counter = record->continuity_counter;
    state->last_arrival_time_ns = record->arrival_time_ns;
    if (record->source_stream_id == 0) {
        state->last_primary_continuity_errors = record->stream_continuity_errors;
    }
}

static int output_record(recovery_engine_t *engine, const packet_record_t *record)
{
    if (packet_sink_send_ts_packet(engine->sink, record->packet) != 0) {
        return -1;
    }

    engine->stats->output_packets++;
    if (record->source_stream_id >= 0 && record->source_stream_id < 2) {
        engine->has_output_stream_index[record->source_stream_id] = true;
        engine->last_output_stream_index[record->source_stream_id] = record->stream_index;
    }
    if (engine->stats->output_packets % OUTPUT_TS_PACKETS_PER_DATAGRAM == 0) {
        report_stats_observe_output_datagram(engine->stats, false);
    }
    observe_output_record(engine, record);
    return 0;
}

static void set_active_output_stream(recovery_engine_t *engine, int stream_id)
{
    if (stream_id < 0 || stream_id > 1 ||
        engine->active_output_stream_id == stream_id) {
        return;
    }

    engine->active_output_stream_id = stream_id;
    engine->stats->active_output_stream_id = stream_id;
    engine->stats->source_switches[stream_id]++;
}

static size_t history_capacity_from_config(const recovery_engine_config_t *config)
{
    uint64_t capacity = RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS;

    if (config->history_ms > 0) {
        capacity = config->history_ms * 14000ULL;
        capacity = (capacity + 999ULL) / 1000ULL;
    }
    if (capacity < 8192ULL) {
        capacity = 8192ULL;
    }
    if (capacity > 1048576ULL) {
        capacity = 1048576ULL;
    }
    return (size_t)capacity;
}

static uint64_t alignment_search_radius_packets(const recovery_engine_t *engine)
{
    uint64_t radius = (engine->config.alignment_window_ns * 14ULL + 999999ULL) / 1000000ULL;

    if (radius < RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS) {
        radius = RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS;
    }
    if (radius > engine->history[0].capacity) {
        radius = engine->history[0].capacity;
    }
    return radius;
}

static void update_alignment_from_match(recovery_engine_t *engine, const packet_record_t *primary,
                                        const packet_record_t *secondary)
{
    engine->alignment.has_alignment = true;
    engine->alignment.offset_packets = (int64_t)secondary->stream_index - (int64_t)primary->stream_index;
    engine->alignment.confidence = 100;
    engine->alignment.consecutive_matches++;
    engine->alignment.consecutive_misses = 0;
    engine->stats->alignment_offset_packets = engine->alignment.offset_packets;
    engine->stats->alignment_confidence = engine->alignment.confidence;
    report_stats_observe_latency(engine->stats, primary->arrival_time_ns,
                                 secondary->arrival_time_ns,
                                 engine->config.max_secondary_latency_ns);
}

static pcr_pid_model_t *pcr_pid_model_for(recovery_engine_t *engine, int stream_id, uint16_t pid)
{
    pcr_pid_model_t *free_model = NULL;
    size_t i;

    for (i = 0; i < RECOVERY_ENGINE_MAX_PCR_PIDS; i++) {
        pcr_pid_model_t *model = &engine->pcr_model[stream_id].pid_models[i];

        if (model->active && model->pid == pid) {
            return model;
        }
        if (!model->active && free_model == NULL) {
            free_model = model;
        }
    }

    if (free_model != NULL) {
        memset(free_model, 0, sizeof(*free_model));
        free_model->active = true;
        free_model->pid = pid;
    }
    return free_model;
}

static const packet_record_t *find_matching_pcr_record(const packet_history_t *history,
                                                       const packet_record_t *record)
{
    const packet_record_t *best = NULL;
    size_t i;

    for (i = 0; i < history->count; i++) {
        size_t index = (history->start + i) % history->capacity;
        const packet_record_t *candidate = &history->records[index];

        if (!candidate->has_pcr || candidate->pid != record->pid ||
            candidate->pcr_value != record->pcr_value ||
            candidate->transport_error || candidate->discontinuity_indicator) {
            continue;
        }
        if (best == NULL || candidate->arrival_time_ns > best->arrival_time_ns) {
            best = candidate;
        }
    }
    return best;
}

static void update_pcr_timing(recovery_engine_t *engine, const packet_record_t *record)
{
    pcr_pid_model_t *model;
    const packet_record_t *match;
    int stream_id = record->source_stream_id;
    int other_stream_id = stream_id == 0 ? 1 : 0;

    if (!record->has_pcr || record->transport_error || record->discontinuity_indicator) {
        return;
    }

    model = pcr_pid_model_for(engine, stream_id, record->pid);
    if (model != NULL) {
        if (model->last_arrival_time_ns != 0 && record->pcr_value > model->last_pcr &&
            record->stream_index > model->last_stream_index) {
            double pcr_seconds = (double)(record->pcr_value - model->last_pcr) / 90000.0;
            double packet_bits = (double)(record->stream_index - model->last_stream_index) *
                                 (double)TS_PACKET_SIZE * 8.0;

            if (pcr_seconds > 0.0) {
                double bitrate_bps = packet_bits / pcr_seconds;
                model->bitrate_bps = model->bitrate_bps == 0.0
                                         ? bitrate_bps
                                         : (model->bitrate_bps * 0.875) + (bitrate_bps * 0.125);
                engine->stats->pcr_bitrate_bps[stream_id] = model->bitrate_bps;
                if (model->confidence < 100U) {
                    model->confidence += model->confidence > 95U ? 100U - model->confidence : 5U;
                }
                engine->pcr_model[stream_id].confidence = model->confidence;
                engine->stats->pcr_timing_confidence[stream_id] = model->confidence;
            }
        }
        model->last_pcr = record->pcr_value;
        model->last_stream_index = record->stream_index;
        model->last_arrival_time_ns = record->arrival_time_ns;
    }

    match = find_matching_pcr_record(&engine->history[other_stream_id], record);
    if (match != NULL) {
        double sample_ns;

        if (stream_id == 0) {
            sample_ns = (double)((int64_t)match->arrival_time_ns -
                                 (int64_t)record->arrival_time_ns);
        } else {
            sample_ns = (double)((int64_t)record->arrival_time_ns -
                                 (int64_t)match->arrival_time_ns);
        }
        engine->pcr_model[stream_id].estimated_delay_ns = sample_ns;
        engine->pcr_model[other_stream_id].estimated_delay_ns = sample_ns;
        report_stats_observe_pcr_delay(engine->stats, sample_ns);
    }
}

static bool has_confirmed_backward_run(packet_history_t *self,
                                       packet_history_t *other,
                                       const packet_record_t *record,
                                       const packet_record_t *match,
                                       uint32_t required_matches)
{
    uint32_t count = 1;

    while (count < required_matches) {
        packet_record_t *previous_self;
        packet_record_t *previous_other;

        if (record->stream_index < count || match->stream_index < count) {
            return false;
        }
        previous_self = packet_history_find_index(self, record->stream_index - count);
        previous_other = packet_history_find_index(other, match->stream_index - count);
        if (previous_self == NULL || previous_other == NULL ||
            !records_match(previous_self, previous_other)) {
            return false;
        }
        count++;
    }

    return true;
}

static packet_record_t *find_confirmed_matching_record_near(packet_history_t *self,
                                                            packet_history_t *other,
                                                            const packet_record_t *record,
                                                            int64_t expected_index,
                                                            uint64_t radius,
                                                            uint32_t required_matches)
{
    int64_t delta;

    for (delta = -(int64_t)radius; delta <= (int64_t)radius; delta++) {
        int64_t index = expected_index + delta;
        packet_record_t *candidate;

        if (index < 0) {
            continue;
        }
        candidate = packet_history_find_index(other, (uint64_t)index);
        if (candidate != NULL && records_match(record, candidate) &&
            has_confirmed_backward_run(self, other, record, candidate, required_matches)) {
            return candidate;
        }
    }
    return NULL;
}

static void add_secondary_after_anchor_candidate(secondary_after_anchor_candidates_t *candidates,
                                                 packet_record_t *candidate)
{
    size_t i;

    if (candidate == NULL) {
        return;
    }
    for (i = 0; i < candidates->count; i++) {
        if (candidates->records[i] == candidate) {
            return;
        }
    }
    if (candidates->count >= MAX_SECONDARY_AFTER_ANCHOR_CANDIDATES) {
        candidates->truncated = true;
        return;
    }
    candidates->records[candidates->count++] = candidate;
}

static void collect_secondary_after_anchor_candidates_in_range(recovery_engine_t *engine,
                                                               const packet_record_t *primary,
                                                               uint64_t first_index,
                                                               uint64_t last_index,
                                                               secondary_after_anchor_candidates_t *candidates)
{
    uint64_t index;

    for (index = first_index; index <= last_index; index++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1], index);

        if (candidate != NULL && records_exact_non_error_match(primary, candidate)) {
            add_secondary_after_anchor_candidate(candidates, candidate);
        }
        if (index == UINT64_MAX) {
            break;
        }
    }
}

static void update_alignment(recovery_engine_t *engine, int stream_id, const packet_record_t *record)
{
    packet_history_t *self = &engine->history[stream_id];
    packet_history_t *other = &engine->history[stream_id == 0 ? 1 : 0];
    packet_record_t *match = NULL;
    int64_t expected_index;

    if (!record_is_informative(record)) {
        return;
    }

    if (engine->alignment.has_alignment) {
        expected_index = stream_id == 0
                             ? (int64_t)record->stream_index + engine->alignment.offset_packets
                             : (int64_t)record->stream_index - engine->alignment.offset_packets;
        match = find_confirmed_matching_record_near(self, other, record, expected_index,
                                                    STAGE1_BOUNDARY_SEARCH_EXTRA,
                                                    RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD);
    }
    if (match == NULL) {
        match = find_confirmed_matching_record_near(self, other, record, (int64_t)record->stream_index,
                                                    STAGE1_BOUNDARY_SEARCH_EXTRA,
                                                    RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD);
    }
    if (match == NULL) {
        if ((record->stream_index % WIDE_ALIGNMENT_PROBE_INTERVAL) != 0ULL) {
            engine->alignment.consecutive_misses++;
            return;
        }
        match = find_confirmed_matching_record_near(self, other, record, (int64_t)record->stream_index,
                                                    alignment_search_radius_packets(engine),
                                                    RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD);
    }
    if (match == NULL) {
        engine->alignment.consecutive_misses++;
        if (engine->alignment.consecutive_misses >= 64U && engine->alignment.confidence > 0U) {
            engine->alignment.confidence--;
            engine->alignment.consecutive_misses = 0;
            engine->stats->alignment_confidence = engine->alignment.confidence;
        }
        return;
    }

    if (stream_id == 0) {
        update_alignment_from_match(engine, record, match);
    } else {
        update_alignment_from_match(engine, match, record);
    }
}

static bool primary_is_silent_beyond_outage(const recovery_engine_t *engine, uint64_t now_ns)
{
    if (engine->config.primary_outage_ns == 0 ||
        engine->last_input_arrival_ns[1] == 0) {
        return false;
    }
    if (engine->last_input_arrival_ns[0] == 0) {
        return false;
    }
    if (engine->last_input_arrival_ns[0] > now_ns) {
        return false;
    }
    return now_ns - engine->last_input_arrival_ns[0] >= engine->config.primary_outage_ns;
}

static void discard_secondary_records_through(recovery_engine_t *engine, uint64_t stream_index)
{
    while (engine->secondary_queue.count > 0) {
        packet_record_t *secondary = delay_queue_front(&engine->secondary_queue);

        if (secondary == NULL || secondary->stream_index > stream_index) {
            break;
        }
        delay_queue_pop(&engine->secondary_queue);
    }
}

static void discard_secondary_records_already_output_by_primary(recovery_engine_t *engine)
{
    uint64_t cutoff = 0;
    bool has_cutoff = false;

    if (engine->last_primary_anchor.valid) {
        cutoff = engine->last_primary_anchor.secondary_index;
        has_cutoff = true;
    }
    if (engine->alignment.has_alignment && engine->has_output_stream_index[0]) {
        int64_t secondary_index = (int64_t)engine->last_output_stream_index[0] +
                                  engine->alignment.offset_packets;

        if (secondary_index >= 0 &&
            (!has_cutoff || (uint64_t)secondary_index > cutoff)) {
            cutoff = (uint64_t)secondary_index;
            has_cutoff = true;
        }
    }
    if (!has_cutoff) {
        return;
    }
    discard_secondary_records_through(engine, cutoff);
}

static bool primary_record_already_output_by_secondary(const recovery_engine_t *engine,
                                                       const packet_record_t *primary)
{
    int64_t secondary_index;

    if (!engine->alignment.has_alignment || !engine->has_output_stream_index[1]) {
        return false;
    }
    secondary_index = (int64_t)primary->stream_index + engine->alignment.offset_packets;
    return secondary_index >= 0 &&
           (uint64_t)secondary_index <= engine->last_output_stream_index[1];
}

static void discard_primary_records_already_output_by_secondary(recovery_engine_t *engine)
{
    while (engine->primary_queue.count > 0) {
        packet_record_t *primary = delay_queue_front(&engine->primary_queue);

        if (primary == NULL ||
            !primary_record_already_output_by_secondary(engine, primary)) {
            break;
        }
        delay_queue_pop(&engine->primary_queue);
    }
}

static int drain_secondary_on_primary_outage(recovery_engine_t *engine, bool force, uint64_t now_ns)
{
    if (engine->active_output_stream_id != 1 &&
        !primary_is_silent_beyond_outage(engine, now_ns)) {
        return 0;
    }

    discard_secondary_records_already_output_by_primary(engine);

    while (engine->secondary_queue.count > 0) {
        packet_record_t *secondary = delay_queue_front(&engine->secondary_queue);

        if (secondary == NULL) {
            break;
        }
        now_ns = report_stats_now_ns();
        if (!force && secondary->arrival_time_ns <= now_ns &&
            now_ns - secondary->arrival_time_ns < engine->secondary_queue.delay_ns) {
            break;
        }
        set_active_output_stream(engine, 1);
        if (output_record(engine, secondary) != 0) {
            return -1;
        }
        delay_queue_pop(&engine->secondary_queue);
        if (!force) {
            break;
        }
    }

    return 0;
}

static bool primary_return_holdoff_elapsed(recovery_engine_t *engine, uint64_t now_ns)
{
    if (engine->active_output_stream_id != 1) {
        engine->primary_return_start_ns = 0;
        return true;
    }
    if (primary_is_silent_beyond_outage(engine, now_ns)) {
        engine->primary_return_start_ns = 0;
        return false;
    }
    if (engine->last_input_arrival_ns[0] == 0) {
        engine->primary_return_start_ns = 0;
        return false;
    }
    if (engine->primary_return_start_ns == 0) {
        engine->primary_return_start_ns = now_ns;
    }
    return now_ns - engine->primary_return_start_ns >= engine->config.primary_return_ns;
}

static void find_secondary_boundary_matches(recovery_engine_t *engine,
                                            const packet_record_t *primary,
                                            secondary_after_anchor_candidates_t *candidates)
{
    uint64_t first_index;
    uint64_t last_index;
    int64_t expected_index;

    memset(candidates, 0, sizeof(*candidates));

    if (primary->is_null || primary->transport_error || primary->discontinuity_indicator) {
        return;
    }
    if (engine->alignment.has_alignment) {
        expected_index = (int64_t)primary->stream_index + engine->alignment.offset_packets;
        if (expected_index >= 0) {
            packet_record_t *exact = packet_history_find_index(&engine->history[1],
                                                               (uint64_t)expected_index);

            if (exact != NULL && records_exact_non_error_match(primary, exact)) {
                add_secondary_after_anchor_candidate(candidates, exact);
            }
        }
        if (expected_index + (int64_t)STAGE1_BOUNDARY_SEARCH_EXTRA >= 0) {
            uint64_t first_exact = expected_index > (int64_t)STAGE1_BOUNDARY_SEARCH_EXTRA
                                       ? (uint64_t)(expected_index - (int64_t)STAGE1_BOUNDARY_SEARCH_EXTRA)
                                       : 0ULL;
            uint64_t last_exact = (uint64_t)(expected_index + (int64_t)STAGE1_BOUNDARY_SEARCH_EXTRA);

            collect_secondary_after_anchor_candidates_in_range(engine, primary, first_exact,
                                                               last_exact, candidates);
        }
    }
    if (!engine->last_primary_anchor.valid ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return;
    }

    first_index = engine->last_primary_anchor.secondary_index + 1ULL;
    {
        uint64_t oversize_scan = (uint64_t)engine->config.max_content_burst_packets + 1ULL;

        if (oversize_scan < OVERSIZE_AFTER_ANCHOR_SCAN_PACKETS) {
            oversize_scan = OVERSIZE_AFTER_ANCHOR_SCAN_PACKETS;
        }
        last_index = first_index +
                     (primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL) +
                     oversize_scan;
    }
    collect_secondary_after_anchor_candidates_in_range(engine, primary, first_index,
                                                       last_index, candidates);
}

static recovery_candidate_validation_t validate_exact_content_candidate(recovery_engine_t *engine,
                                                                        const packet_record_t *primary,
                                                                        const packet_record_t *secondary_match)
{
    recovery_candidate_validation_t result = {
        false, RECOVERY_REJECT_MISSING_CANDIDATE, "not_content_gap", 0, false
    };
    output_pid_state_t states[REPORT_STATS_PIDS];
    uint64_t primary_between;
    uint64_t secondary_between;
    uint64_t missing_packets;
    uint64_t cc_missing_count = 0;
    uint64_t candidate_first_index;
    uint64_t candidate_count;
    bool has_previous_cc;
    bool has_cc_missing_count = false;
    uint8_t previous_cc = 0;
    packet_record_t *candidates[256] = {0};

    if (primary->is_null || primary->transport_error || primary->discontinuity_indicator ||
        !engine->last_primary_anchor.valid || secondary_match == NULL) {
        return result;
    }
    if (secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return result;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (secondary_between < primary_between) {
        return result;
    }
    missing_packets = secondary_between - primary_between;
    if (record_fits_next_output(engine, primary)) {
        return result;
    }
    has_previous_cc = previous_output_cc_for_record(engine, primary, &previous_cc);
    if (has_previous_cc) {
        has_cc_missing_count = infer_missing_from_cc(previous_cc, primary, &cc_missing_count);
    }
    if (!has_cc_missing_count && has_previous_cc && primary->has_payload &&
        missing_packets > 0 && missing_packets <= 15U &&
        primary->continuity_counter ==
            (uint8_t)((previous_cc + missing_packets + 1ULL) & 0x0fU)) {
        cc_missing_count = missing_packets;
        has_cc_missing_count = true;
    }
    result.missing_packets = cc_missing_count;
    result.has_missing_count = has_cc_missing_count;
    if (!has_cc_missing_count || cc_missing_count == 0) {
        return result;
    }
    if (cc_missing_count > engine->config.max_content_burst_packets) {
        result.reject_reason = RECOVERY_REJECT_BURST_TOO_LARGE;
        result.reason = "burst_too_large";
        return result;
    }
    if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
        result.reject_reason = RECOVERY_REJECT_LOW_ALIGNMENT;
        result.reason = "low_alignment";
        return result;
    }
    if (!records_are_non_null_exact_anchor(packet_history_find_index(&engine->history[0],
                                                                     engine->last_primary_anchor.primary_index),
                                           packet_history_find_index(&engine->history[1],
                                                                     engine->last_primary_anchor.secondary_index))) {
        result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
        result.reason = "before_anchor";
        return result;
    }
    if (primary->is_null || secondary_match->is_null ||
        !records_exact_non_error_match(primary, secondary_match)) {
        result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
        result.reason = "after_anchor";
        return result;
    }

    candidate_first_index = engine->last_primary_anchor.secondary_index + 1ULL;
    if (primary_between > 0 && secondary_between > primary_between) {
        bool prefix_matches = true;
        uint64_t i;

        for (i = 0; i < primary_between; i++) {
            packet_record_t *primary_prefix = packet_history_find_index(&engine->history[0],
                                                                        engine->last_primary_anchor.primary_index + 1ULL + i);
            packet_record_t *secondary_prefix = packet_history_find_index(&engine->history[1],
                                                                          engine->last_primary_anchor.secondary_index + 1ULL + i);

            if (primary_prefix == NULL || secondary_prefix == NULL ||
                !records_exact_non_error_match(primary_prefix, secondary_prefix)) {
                prefix_matches = false;
                break;
            }
        }
        if (prefix_matches) {
            candidate_first_index += primary_between;
        }
    }

    for (candidate_count = 0; candidate_count < cc_missing_count; candidate_count++) {
        uint64_t index;
        packet_record_t *candidate = NULL;
        uint8_t expected_missing_cc = (uint8_t)((previous_cc + 1U + candidate_count) & 0x0fU);

        for (index = candidate_first_index; index < secondary_match->stream_index; index++) {
            packet_record_t *possible = packet_history_find_index(&engine->history[1], index);

            if (possible == NULL) {
                continue;
            }
            if (!possible->is_null && possible->has_payload && possible->pid == primary->pid &&
                possible->continuity_counter == expected_missing_cc) {
                if (candidate != NULL) {
                    result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
                    result.reason = "multiple_candidates";
                    return result;
                }
                candidate = possible;
            }
        }
        if (candidate == NULL && missing_packets == 1) {
            uint64_t fallback_index = primary_between == 0
                                          ? engine->last_primary_anchor.secondary_index + 1ULL
                                          : secondary_match->stream_index - 1ULL;

            candidate = packet_history_find_index(&engine->history[1], fallback_index);
        }
        if (candidate == NULL) {
            result.reject_reason = RECOVERY_REJECT_MISSING_CANDIDATE;
            result.reason = "missing_candidate";
            return result;
        }
        if (candidate->is_null) {
            result.reject_reason = RECOVERY_REJECT_WRONG_PID;
            result.reason = "null_candidate";
            return result;
        }
        if (candidate->transport_error) {
            result.reject_reason = RECOVERY_REJECT_TEI;
            result.reason = "secondary_tei";
            return result;
        }
        if (candidate->discontinuity_indicator) {
            result.reject_reason = RECOVERY_REJECT_DISCONTINUITY;
            result.reason = "secondary_discontinuity";
            return result;
        }
        if (candidate->pid != primary->pid) {
            result.reject_reason = RECOVERY_REJECT_WRONG_PID;
            result.reason = "wrong_pid";
            return result;
        }
        if (!candidate->has_payload || candidate->continuity_counter != expected_missing_cc) {
            result.reject_reason = RECOVERY_REJECT_WRONG_COUNTER;
            result.reason = "wrong_counter";
            return result;
        }
        candidates[candidate_count] = candidate;
    }

    memcpy(states, engine->output_pid_state, sizeof(states));
    for (candidate_count = 0; candidate_count < cc_missing_count; candidate_count++) {
        if (!record_fits_output_states(states, candidates[candidate_count])) {
            result.reject_reason = RECOVERY_REJECT_WRONG_COUNTER;
            result.reason = "secondary_wrong_counter";
            return result;
        }
        observe_output_states(states, candidates[candidate_count]);
    }
    if (!record_fits_output_states(states, primary)) {
        result.reject_reason = RECOVERY_REJECT_WRONG_COUNTER;
        result.reason = "after_anchor_wrong_counter";
        return result;
    }

    result.valid = true;
    result.reason = NULL;
    return result;
}

static int try_recover_exact_content_gap(recovery_engine_t *engine,
                                         const packet_record_t *primary,
                                         const packet_record_t *secondary_match)
{
    uint64_t primary_between;
    uint64_t secondary_between;
    uint64_t missing_packets;
    uint64_t cc_missing_count = 0;
    bool has_previous_cc;
    bool has_cc_missing_count = false;
    uint8_t previous_cc = 0;
    uint8_t expected_missing_cc = 0;
    packet_record_t *before_primary;
    packet_record_t *before_secondary;
    packet_record_t *candidates[256] = {0};
    uint64_t candidate_first_index;
    uint64_t candidate_count;
    output_pid_state_t states[REPORT_STATS_PIDS];
    const char *decision;

    if (primary->is_null || primary->transport_error || primary->discontinuity_indicator ||
        !engine->last_primary_anchor.valid || secondary_match == NULL) {
        return 0;
    }
    if (secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return 0;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (secondary_between < primary_between) {
        return 0;
    }
    missing_packets = secondary_between - primary_between;
    if (record_fits_next_output(engine, primary)) {
        return 0;
    }
    has_previous_cc = previous_output_cc_for_record(engine, primary, &previous_cc);
    if (has_previous_cc) {
        has_cc_missing_count = infer_missing_from_cc(previous_cc, primary, &cc_missing_count);
    }
    if (!has_cc_missing_count && has_previous_cc && primary->has_payload &&
        missing_packets > 0 && missing_packets <= 15U &&
        primary->continuity_counter ==
            (uint8_t)((previous_cc + missing_packets + 1ULL) & 0x0fU)) {
        cc_missing_count = missing_packets;
        has_cc_missing_count = true;
    }
    if (!has_cc_missing_count || cc_missing_count == 0) {
        return RECOVERY_DECISION_NONE;
    }
    if (cc_missing_count > engine->config.max_content_burst_packets) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            cc_missing_count, true, true, true, true,
                            RECOVERY_REJECT_BURST_TOO_LARGE, "burst_too_large");
        return RECOVERY_DECISION_REJECTED;
    }
    if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            cc_missing_count, true, true, true, true,
                            RECOVERY_REJECT_LOW_ALIGNMENT, "low_alignment");
        return RECOVERY_DECISION_REJECTED;
    }

    before_primary = packet_history_find_index(&engine->history[0],
                                               engine->last_primary_anchor.primary_index);
    before_secondary = packet_history_find_index(&engine->history[1],
                                                 engine->last_primary_anchor.secondary_index);
    if (before_primary == NULL || before_secondary == NULL ||
        before_primary->is_null || before_secondary->is_null ||
        !records_exact_non_error_match(before_primary, before_secondary)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            cc_missing_count, true, true, false, true,
                            RECOVERY_REJECT_AMBIGUOUS, "before_anchor");
        return RECOVERY_DECISION_REJECTED;
    }
    if (primary->is_null || secondary_match->is_null ||
        !records_exact_non_error_match(primary, secondary_match)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            cc_missing_count, true, true, true, false,
                            RECOVERY_REJECT_AMBIGUOUS, "after_anchor");
        return RECOVERY_DECISION_REJECTED;
    }

    candidate_first_index = engine->last_primary_anchor.secondary_index + 1ULL;
    if (primary_between > 0 && secondary_between > primary_between) {
        bool prefix_matches = true;
        uint64_t i;

        for (i = 0; i < primary_between; i++) {
            packet_record_t *primary_prefix = packet_history_find_index(&engine->history[0],
                                                                        engine->last_primary_anchor.primary_index + 1ULL + i);
            packet_record_t *secondary_prefix = packet_history_find_index(&engine->history[1],
                                                                          engine->last_primary_anchor.secondary_index + 1ULL + i);

            if (primary_prefix == NULL || secondary_prefix == NULL ||
                !records_exact_non_error_match(primary_prefix, secondary_prefix)) {
                prefix_matches = false;
                break;
            }
        }
        if (prefix_matches) {
            candidate_first_index += primary_between;
        }
    }

    for (candidate_count = 0; candidate_count < cc_missing_count; candidate_count++) {
        uint64_t index;
        packet_record_t *candidate = NULL;

        expected_missing_cc = (uint8_t)((previous_cc + 1U + candidate_count) & 0x0fU);
        for (index = candidate_first_index;
             index < secondary_match->stream_index; index++) {
            packet_record_t *possible = packet_history_find_index(&engine->history[1], index);

            if (possible == NULL) {
                continue;
            }
            if (!possible->is_null && possible->has_payload && possible->pid == primary->pid &&
                possible->continuity_counter == expected_missing_cc) {
                if (candidate != NULL) {
                    recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                        cc_missing_count, true, true, true, true,
                                        RECOVERY_REJECT_AMBIGUOUS, "multiple_candidates");
                    return RECOVERY_DECISION_REJECTED;
                }
                candidate = possible;
            }
        }
        if (candidate == NULL && missing_packets == 1) {
            uint64_t fallback_index = primary_between == 0
                                          ? engine->last_primary_anchor.secondary_index + 1ULL
                                          : secondary_match->stream_index - 1ULL;

            candidate = packet_history_find_index(&engine->history[1], fallback_index);
        }
        if (candidate == NULL) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                cc_missing_count, true, false, true, true,
                                RECOVERY_REJECT_MISSING_CANDIDATE, "missing_candidate");
            return RECOVERY_DECISION_REJECTED;
        }
        if (candidate->is_null) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                cc_missing_count, true, true, true, true,
                                RECOVERY_REJECT_WRONG_PID, "null_candidate");
            return RECOVERY_DECISION_REJECTED;
        }
        if (candidate->transport_error) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                cc_missing_count, true, true, true, true,
                                RECOVERY_REJECT_TEI, "secondary_tei");
            return RECOVERY_DECISION_REJECTED;
        }
        if (candidate->discontinuity_indicator) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                cc_missing_count, true, true, true, true,
                                RECOVERY_REJECT_DISCONTINUITY, "secondary_discontinuity");
            return RECOVERY_DECISION_REJECTED;
        }
        if (candidate->pid != primary->pid) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                cc_missing_count, true, true, true, true,
                                RECOVERY_REJECT_WRONG_PID, "wrong_pid");
            return RECOVERY_DECISION_REJECTED;
        }
        if (!candidate->has_payload || candidate->continuity_counter != expected_missing_cc) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                cc_missing_count, true, true, true, true,
                                RECOVERY_REJECT_WRONG_COUNTER, "wrong_counter");
            return RECOVERY_DECISION_REJECTED;
        }
        candidates[candidate_count] = candidate;
    }

    memcpy(states, engine->output_pid_state, sizeof(states));
    for (candidate_count = 0; candidate_count < cc_missing_count; candidate_count++) {
        if (!record_fits_output_states(states, candidates[candidate_count])) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                cc_missing_count, true, true, true, true,
                                RECOVERY_REJECT_WRONG_COUNTER, "secondary_wrong_counter");
            return RECOVERY_DECISION_REJECTED;
        }
        observe_output_states(states, candidates[candidate_count]);
    }
    if (!record_fits_output_states(states, primary)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            cc_missing_count, true, true, true, true,
                            RECOVERY_REJECT_WRONG_COUNTER, "after_anchor_wrong_counter");
        return RECOVERY_DECISION_REJECTED;
    }

    decision = cc_missing_count > 1 ? "recover_content_burst" : "recover_content";
    recovery_decision_log(engine, 0, primary, previous_cc, has_previous_cc,
                          cc_missing_count, true, true, true, true,
                          decision, NULL);
    for (candidate_count = 0; candidate_count < cc_missing_count; candidate_count++) {
        if (output_record(engine, candidates[candidate_count]) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, false);
    }
    if (cc_missing_count > 1) {
        engine->stats->recovered_content_bursts++;
    }
    return 1;
}

static int try_recover_primary_tei_packet(recovery_engine_t *engine,
                                          const packet_record_t *primary)
{
    packet_record_t *candidate;
    packet_record_t *before_primary;
    packet_record_t *before_secondary;
    packet_record_t *after_primary;
    packet_record_t *after_secondary;
    uint8_t previous_cc = 0;
    bool has_previous_cc = previous_output_cc_for_record(engine, primary, &previous_cc);

    if (!primary->transport_error) {
        return 0;
    }
    if (primary->is_null || !primary->has_payload || primary->discontinuity_indicator) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, false, engine->last_primary_anchor.valid, false,
                            primary->discontinuity_indicator ? RECOVERY_REJECT_DISCONTINUITY
                                                             : RECOVERY_REJECT_WRONG_PID,
                            primary->discontinuity_indicator ? "primary_discontinuity"
                                                             : "primary_not_content");
        return RECOVERY_DECISION_REJECTED;
    }
    if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, false, engine->last_primary_anchor.valid, false,
                            RECOVERY_REJECT_LOW_ALIGNMENT, "low_alignment");
        return RECOVERY_DECISION_REJECTED;
    }

    before_primary = packet_history_find_index(&engine->history[0],
                                               engine->last_primary_anchor.primary_index);
    before_secondary = packet_history_find_index(&engine->history[1],
                                                 engine->last_primary_anchor.secondary_index);
    if (!records_are_non_null_exact_anchor(before_primary, before_secondary)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, false, false, false,
                            RECOVERY_REJECT_AMBIGUOUS, "before_anchor");
        return RECOVERY_DECISION_REJECTED;
    }

    candidate = find_aligned_secondary_record(engine, primary);
    if (candidate == NULL) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, false, true, false,
                            RECOVERY_REJECT_MISSING_CANDIDATE, "missing_candidate");
        return RECOVERY_DECISION_REJECTED;
    }
    if (candidate->transport_error) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, true, true, false,
                            RECOVERY_REJECT_TEI, "secondary_tei");
        return RECOVERY_DECISION_REJECTED;
    }
    if (candidate->discontinuity_indicator) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, true, true, false,
                            RECOVERY_REJECT_DISCONTINUITY, "secondary_discontinuity");
        return RECOVERY_DECISION_REJECTED;
    }
    if (candidate->is_null || !candidate->has_payload || candidate->pid != primary->pid) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, true, true, false,
                            RECOVERY_REJECT_WRONG_PID, "wrong_pid");
        return RECOVERY_DECISION_REJECTED;
    }
    if (candidate->continuity_counter != primary->continuity_counter) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, true, true, false,
                            RECOVERY_REJECT_WRONG_COUNTER, "wrong_counter");
        return RECOVERY_DECISION_REJECTED;
    }

    after_primary = packet_history_find_index(&engine->history[0], primary->stream_index + 1ULL);
    after_secondary = candidate->stream_index == UINT64_MAX
                          ? NULL
                          : packet_history_find_index(&engine->history[1],
                                                      candidate->stream_index + 1ULL);
    if (!records_are_non_null_exact_anchor(after_primary, after_secondary)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, true, true, false,
                            RECOVERY_REJECT_AMBIGUOUS, "no_after_anchor");
        return RECOVERY_DECISION_REJECTED;
    }
    if (!record_fits_next_output(engine, candidate)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, true, true, true, true,
                            RECOVERY_REJECT_WRONG_COUNTER, "secondary_wrong_counter");
        return RECOVERY_DECISION_REJECTED;
    }

    recovery_decision_log(engine, 0, primary, previous_cc, has_previous_cc,
                          0, true, true, true, true, "recover_primary_tei", NULL);
    if (output_record(engine, candidate) != 0) {
        return -1;
    }
    report_stats_observe_recovery(engine->stats, false);
    return 1;
}

static recovery_candidate_validation_t validate_null_position_candidate(recovery_engine_t *engine,
                                                                        const packet_record_t *primary,
                                                                        const packet_record_t *secondary_match)
{
    recovery_candidate_validation_t result = {
        false, RECOVERY_REJECT_MISSING_CANDIDATE, "not_null_position_gap", 0, false
    };
    uint64_t primary_between;
    uint64_t secondary_between;
    uint64_t missing_packets;
    uint64_t first_candidate_index;
    uint64_t i;

    if (primary->is_null || primary->transport_error || primary->discontinuity_indicator ||
        !engine->last_primary_anchor.valid || secondary_match == NULL) {
        return result;
    }
    if (secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return result;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (secondary_between <= primary_between) {
        return result;
    }
    missing_packets = secondary_between - primary_between;
    result.missing_packets = missing_packets;
    result.has_missing_count = true;
    if (missing_packets == 0) {
        result.reason = "not_null_position_gap";
        return result;
    }
    if (missing_packets > engine->config.max_content_burst_packets) {
        uint64_t scan_limit = (uint64_t)engine->config.max_content_burst_packets + 1ULL;

        first_candidate_index = engine->last_primary_anchor.secondary_index + primary_between + 1ULL;
        for (i = 0; i < scan_limit; i++) {
            packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                                   first_candidate_index + i);

            if (candidate == NULL) {
                result.reject_reason = RECOVERY_REJECT_MISSING_CANDIDATE;
                result.reason = "missing_candidate";
                return result;
            }
            if (!candidate->is_null) {
                result.reason = "not_null_position_gap";
                return result;
            }
        }
        result.reject_reason = RECOVERY_REJECT_BURST_TOO_LARGE;
        result.reason = "null_burst_too_large";
        return result;
    }
    if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
        result.reject_reason = RECOVERY_REJECT_LOW_ALIGNMENT;
        result.reason = "low_alignment";
        return result;
    }
    if (!records_are_non_null_exact_anchor(packet_history_find_index(&engine->history[0],
                                                                     engine->last_primary_anchor.primary_index),
                                           packet_history_find_index(&engine->history[1],
                                                                     engine->last_primary_anchor.secondary_index))) {
        result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
        result.reason = "before_anchor";
        return result;
    }
    if (primary->is_null || secondary_match->is_null ||
        !records_exact_non_error_match(primary, secondary_match)) {
        result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
        result.reason = "after_anchor";
        return result;
    }

    for (i = 0; i < primary_between; i++) {
        packet_record_t *primary_prefix = packet_history_find_index(&engine->history[0],
                                                                    engine->last_primary_anchor.primary_index + 1ULL + i);
        packet_record_t *secondary_prefix = packet_history_find_index(&engine->history[1],
                                                                      engine->last_primary_anchor.secondary_index + 1ULL + i);

        if (primary_prefix == NULL || secondary_prefix == NULL ||
            !records_exact_non_error_match(primary_prefix, secondary_prefix)) {
            result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
            result.reason = "stale_anchor_prefix";
            return result;
        }
    }

    first_candidate_index = engine->last_primary_anchor.secondary_index + primary_between + 1ULL;
    for (i = 0; i < missing_packets; i++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                               first_candidate_index + i);

        if (candidate == NULL) {
            result.reject_reason = RECOVERY_REJECT_MISSING_CANDIDATE;
            result.reason = "missing_candidate";
            return result;
        }
        if (!candidate->is_null) {
            result.reason = "not_null_position_gap";
            return result;
        }
        if (candidate->transport_error) {
            result.reject_reason = RECOVERY_REJECT_TEI;
            result.reason = "secondary_tei";
            return result;
        }
        if (candidate->discontinuity_indicator) {
            result.reject_reason = RECOVERY_REJECT_DISCONTINUITY;
            result.reason = "secondary_discontinuity";
            return result;
        }
    }

    result.valid = true;
    result.reject_reason = RECOVERY_REJECT_MISSING_CANDIDATE;
    result.reason = NULL;
    return result;
}

static recovery_candidate_validation_t validate_mixed_position_candidate(recovery_engine_t *engine,
                                                                         const packet_record_t *primary,
                                                                         const packet_record_t *secondary_match)
{
    recovery_candidate_validation_t result = {
        false, RECOVERY_REJECT_MISSING_CANDIDATE, "not_mixed_position_gap", 0, false
    };
    output_pid_state_t states[REPORT_STATS_PIDS];
    uint64_t primary_between;
    uint64_t secondary_between;
    uint64_t missing_packets;
    uint64_t first_candidate_index;
    bool saw_content = false;
    uint8_t previous_cc = 0;
    bool has_previous_cc = previous_output_cc_for_record(engine, primary, &previous_cc);
    uint64_t i;

    if (primary->is_null || primary->transport_error || primary->discontinuity_indicator ||
        !engine->last_primary_anchor.valid || secondary_match == NULL) {
        return result;
    }
    if (secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return result;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (secondary_between <= primary_between) {
        return result;
    }
    missing_packets = secondary_between - primary_between;
    result.missing_packets = missing_packets;
    result.has_missing_count = true;
    if (missing_packets == 0) {
        return result;
    }
    if (missing_packets > engine->config.max_content_burst_packets) {
        result.reject_reason = RECOVERY_REJECT_BURST_TOO_LARGE;
        result.reason = "mixed_burst_too_large";
        return result;
    }
    if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
        result.reject_reason = RECOVERY_REJECT_LOW_ALIGNMENT;
        result.reason = "low_alignment";
        return result;
    }
    if (!records_are_non_null_exact_anchor(packet_history_find_index(&engine->history[0],
                                                                     engine->last_primary_anchor.primary_index),
                                           packet_history_find_index(&engine->history[1],
                                                                     engine->last_primary_anchor.secondary_index))) {
        result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
        result.reason = "before_anchor";
        return result;
    }
    if (primary->is_null || secondary_match->is_null ||
        !records_exact_non_error_match(primary, secondary_match)) {
        result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
        result.reason = "after_anchor";
        return result;
    }

    for (i = 0; i < primary_between; i++) {
        packet_record_t *primary_prefix = packet_history_find_index(&engine->history[0],
                                                                    engine->last_primary_anchor.primary_index + 1ULL + i);
        packet_record_t *secondary_prefix = packet_history_find_index(&engine->history[1],
                                                                      engine->last_primary_anchor.secondary_index + 1ULL + i);

        if (primary_prefix == NULL || secondary_prefix == NULL ||
            !records_exact_non_error_match(primary_prefix, secondary_prefix)) {
            result.reject_reason = RECOVERY_REJECT_AMBIGUOUS;
            result.reason = "stale_anchor_prefix";
            return result;
        }
    }

    memcpy(states, engine->output_pid_state, sizeof(states));
    first_candidate_index = engine->last_primary_anchor.secondary_index + primary_between + 1ULL;
    for (i = 0; i < missing_packets; i++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                               first_candidate_index + i);

        if (candidate == NULL) {
            result.reject_reason = RECOVERY_REJECT_MISSING_CANDIDATE;
            result.reason = "missing_candidate";
            return result;
        }
        if (candidate->transport_error) {
            result.reject_reason = RECOVERY_REJECT_TEI;
            result.reason = "secondary_tei";
            return result;
        }
        if (candidate->discontinuity_indicator) {
            result.reject_reason = RECOVERY_REJECT_DISCONTINUITY;
            result.reason = "secondary_discontinuity";
            return result;
        }
        if (!candidate->is_null) {
            saw_content = true;
        }
        if (!record_fits_output_states(states, candidate)) {
            result.reject_reason = RECOVERY_REJECT_WRONG_COUNTER;
            result.reason = "candidate_counter_contradiction";
            return result;
        }
        observe_output_states(states, candidate);
    }
    if (!saw_content) {
        result.reason = "not_mixed_position_gap";
        return result;
    }
    if (!record_fits_output_states(states, primary)) {
        uint64_t cc_missing_count = 0;

        if (has_previous_cc && infer_missing_from_cc(previous_cc, primary, &cc_missing_count) &&
            cc_missing_count <= missing_packets) {
            uint64_t matching_pid_count = 0;

            for (i = 0; i < missing_packets; i++) {
                packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                                       first_candidate_index + i);

                if (candidate != NULL && !candidate->is_null && candidate->pid == primary->pid) {
                    matching_pid_count++;
                }
            }
            if (matching_pid_count == 0) {
                result.reject_reason = RECOVERY_REJECT_WRONG_PID;
                result.reason = "wrong_pid";
                return result;
            }
        }
        result.reject_reason = RECOVERY_REJECT_WRONG_COUNTER;
        result.reason = "after_anchor_wrong_counter";
        return result;
    }

    result.valid = true;
    result.reject_reason = RECOVERY_REJECT_MISSING_CANDIDATE;
    result.reason = NULL;
    return result;
}

static int try_recover_mixed_position_gap(recovery_engine_t *engine,
                                          const packet_record_t *primary,
                                          const packet_record_t *secondary_match)
{
    uint64_t primary_between;
    uint64_t secondary_between;
    uint64_t missing_packets;
    uint64_t first_candidate_index;
    packet_record_t *before_primary;
    packet_record_t *before_secondary;
    output_pid_state_t states[REPORT_STATS_PIDS];
    uint8_t previous_cc = 0;
    bool has_previous_cc = previous_output_cc_for_record(engine, primary, &previous_cc);
    bool primary_fits;
    bool saw_content = false;
    bool saw_null = false;
    bool same_content_pid = true;
    uint16_t content_pid = 0;
    const char *decision;
    uint64_t i;

    if (primary->is_null || primary->transport_error || primary->discontinuity_indicator ||
        !engine->last_primary_anchor.valid || secondary_match == NULL) {
        return 0;
    }
    if (secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return 0;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (secondary_between <= primary_between) {
        return 0;
    }
    missing_packets = secondary_between - primary_between;
    if (missing_packets == 0) {
        return 0;
    }
    if (missing_packets > engine->config.max_content_burst_packets) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, true, true,
                            RECOVERY_REJECT_BURST_TOO_LARGE, "mixed_burst_too_large");
        return RECOVERY_DECISION_REJECTED;
    }
    if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, true, true,
                            RECOVERY_REJECT_LOW_ALIGNMENT, "low_alignment");
        return RECOVERY_DECISION_REJECTED;
    }

    before_primary = packet_history_find_index(&engine->history[0],
                                               engine->last_primary_anchor.primary_index);
    before_secondary = packet_history_find_index(&engine->history[1],
                                                 engine->last_primary_anchor.secondary_index);
    if (!records_are_non_null_exact_anchor(before_primary, before_secondary)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, false, true,
                            RECOVERY_REJECT_AMBIGUOUS, "before_anchor");
        return RECOVERY_DECISION_REJECTED;
    }
    if (primary->is_null || secondary_match->is_null ||
        !records_exact_non_error_match(primary, secondary_match)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, true, false,
                            RECOVERY_REJECT_AMBIGUOUS, "after_anchor");
        return RECOVERY_DECISION_REJECTED;
    }

    memcpy(states, engine->output_pid_state, sizeof(states));
    for (i = 0; i < primary_between; i++) {
        packet_record_t *primary_prefix = packet_history_find_index(&engine->history[0],
                                                                    engine->last_primary_anchor.primary_index + 1ULL + i);
        packet_record_t *secondary_prefix = packet_history_find_index(&engine->history[1],
                                                                      engine->last_primary_anchor.secondary_index + 1ULL + i);

        if (primary_prefix == NULL || secondary_prefix == NULL ||
            !records_exact_non_error_match(primary_prefix, secondary_prefix)) {
            return RECOVERY_DECISION_NONE;
        }
    }

    first_candidate_index = engine->last_primary_anchor.secondary_index + primary_between + 1ULL;
    for (i = 0; i < missing_packets; i++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                               first_candidate_index + i);

        if (candidate == NULL) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_packets, true, false, true, true,
                                RECOVERY_REJECT_MISSING_CANDIDATE, "missing_candidate");
            return RECOVERY_DECISION_REJECTED;
        }
        if (candidate->transport_error) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_packets, true, true, true, true,
                                RECOVERY_REJECT_TEI, "secondary_tei");
            return RECOVERY_DECISION_REJECTED;
        }
        if (candidate->discontinuity_indicator) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_packets, true, true, true, true,
                                RECOVERY_REJECT_DISCONTINUITY, "secondary_discontinuity");
            return RECOVERY_DECISION_REJECTED;
        }
        if (candidate->is_null) {
            saw_null = true;
        } else {
            if (!saw_content) {
                content_pid = candidate->pid;
            } else if (candidate->pid != content_pid) {
                same_content_pid = false;
            }
            saw_content = true;
        }
        if (!record_fits_output_states(states, candidate)) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_packets, true, true, true, true,
                                RECOVERY_REJECT_WRONG_COUNTER, "candidate_counter_contradiction");
            return RECOVERY_DECISION_REJECTED;
        }
        observe_output_states(states, candidate);
    }
    if (!saw_content) {
        return RECOVERY_DECISION_NONE;
    }

    primary_fits = record_fits_output_states(states, primary);
    if (!primary_fits) {
        uint64_t cc_missing_count = 0;

        if (has_previous_cc && infer_missing_from_cc(previous_cc, primary, &cc_missing_count) &&
            cc_missing_count <= missing_packets) {
            uint64_t matching_pid_count = 0;

            for (i = 0; i < missing_packets; i++) {
                packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                                       first_candidate_index + i);

                if (candidate != NULL && !candidate->is_null && candidate->pid == primary->pid) {
                    matching_pid_count++;
                }
            }
            if (matching_pid_count == 0) {
                recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                    missing_packets, true, true, true, true,
                                    RECOVERY_REJECT_WRONG_PID, "wrong_pid");
                return RECOVERY_DECISION_REJECTED;
            }
        }
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, true, true,
                            RECOVERY_REJECT_WRONG_COUNTER, "after_anchor_wrong_counter");
        return RECOVERY_DECISION_REJECTED;
    }

    if (!saw_null && same_content_pid && primary->pid == content_pid) {
        decision = missing_packets > 1 ? "recover_content_burst" : "recover_content";
    } else {
        decision = "recover_mixed_burst";
    }
    recovery_decision_log(engine, 0, primary, previous_cc, has_previous_cc,
                          missing_packets, true, true, true, true,
                          decision, NULL);
    for (i = 0; i < missing_packets; i++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                               first_candidate_index + i);

        if (candidate == NULL || output_record(engine, candidate) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, candidate->is_null);
    }
    if (missing_packets > 1) {
        engine->stats->recovered_content_bursts++;
    }
    return 1;
}

static int try_recover_null_position_gap(recovery_engine_t *engine,
                                         const packet_record_t *primary,
                                         const packet_record_t *secondary_match)
{
    uint64_t primary_between;
    uint64_t secondary_between;
    uint64_t missing_packets;
    uint64_t first_candidate_index;
    packet_record_t *before_primary;
    packet_record_t *before_secondary;
    uint8_t previous_cc = 0;
    bool has_previous_cc = previous_output_cc_for_record(engine, primary, &previous_cc);
    uint64_t i;

    if (primary->is_null || primary->transport_error || primary->discontinuity_indicator ||
        !engine->last_primary_anchor.valid || secondary_match == NULL) {
        return RECOVERY_DECISION_NONE;
    }
    if (secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return RECOVERY_DECISION_NONE;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (secondary_between <= primary_between) {
        return RECOVERY_DECISION_NONE;
    }
    missing_packets = secondary_between - primary_between;
    if (primary_between != 0 || missing_packets == 0) {
        return RECOVERY_DECISION_NONE;
    }
    if (missing_packets > engine->config.max_content_burst_packets) {
        uint64_t scan_limit = (uint64_t)engine->config.max_content_burst_packets + 1ULL;

        first_candidate_index = engine->last_primary_anchor.secondary_index + 1ULL;
        for (i = 0; i < scan_limit; i++) {
            packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                                   first_candidate_index + i);

            if (candidate == NULL) {
                recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                    missing_packets, true, false, true, true,
                                    RECOVERY_REJECT_MISSING_CANDIDATE, "missing_candidate");
                return RECOVERY_DECISION_REJECTED;
            }
            if (!candidate->is_null) {
                return RECOVERY_DECISION_NONE;
            }
        }
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, true, true,
                            RECOVERY_REJECT_BURST_TOO_LARGE, "null_burst_too_large");
        return RECOVERY_DECISION_REJECTED;
    }
    if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, true, true,
                            RECOVERY_REJECT_LOW_ALIGNMENT, "low_alignment");
        return RECOVERY_DECISION_REJECTED;
    }

    before_primary = packet_history_find_index(&engine->history[0],
                                               engine->last_primary_anchor.primary_index);
    before_secondary = packet_history_find_index(&engine->history[1],
                                                 engine->last_primary_anchor.secondary_index);
    if (!records_are_non_null_exact_anchor(before_primary, before_secondary)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, false, true,
                            RECOVERY_REJECT_AMBIGUOUS, "before_anchor");
        return RECOVERY_DECISION_REJECTED;
    }
    if (primary->is_null || secondary_match->is_null ||
        !records_exact_non_error_match(primary, secondary_match)) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, true, false,
                            RECOVERY_REJECT_AMBIGUOUS, "after_anchor");
        return RECOVERY_DECISION_REJECTED;
    }

    first_candidate_index = engine->last_primary_anchor.secondary_index + 1ULL;
    for (i = 0; i < missing_packets; i++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                               first_candidate_index + i);

        if (candidate == NULL) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_packets, true, false, true, true,
                                RECOVERY_REJECT_MISSING_CANDIDATE, "missing_candidate");
            return RECOVERY_DECISION_REJECTED;
        }
        if (!candidate->is_null) {
            return RECOVERY_DECISION_NONE;
        }
        if (candidate->transport_error) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_packets, true, true, true, true,
                                RECOVERY_REJECT_TEI, "secondary_tei");
            return RECOVERY_DECISION_REJECTED;
        }
        if (candidate->discontinuity_indicator) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_packets, true, true, true, true,
                                RECOVERY_REJECT_DISCONTINUITY, "secondary_discontinuity");
            return RECOVERY_DECISION_REJECTED;
        }
    }

    recovery_decision_log(engine, 0, primary, previous_cc, has_previous_cc,
                          missing_packets, true, true, true, true,
                          "recover_null_region", NULL);
    for (i = 0; i < missing_packets; i++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                               first_candidate_index + i);

        if (candidate == NULL || output_record(engine, candidate) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, true);
    }
    return RECOVERY_DECISION_INSERTED;
}

typedef recovery_candidate_validation_t (*candidate_validator_fn)(recovery_engine_t *engine,
                                                                  const packet_record_t *primary,
                                                                  const packet_record_t *secondary_match);
typedef int (*candidate_recovery_fn)(recovery_engine_t *engine,
                                     const packet_record_t *primary,
                                     const packet_record_t *secondary_match);

static bool validation_reject_is_definitive(const recovery_candidate_validation_t *validation)
{
    return validation->reject_reason == RECOVERY_REJECT_WRONG_COUNTER ||
           validation->reject_reason == RECOVERY_REJECT_TEI ||
           validation->reject_reason == RECOVERY_REJECT_DISCONTINUITY ||
           validation->reject_reason == RECOVERY_REJECT_BURST_TOO_LARGE ||
           validation->reject_reason == RECOVERY_REJECT_LOW_ALIGNMENT;
}

static int try_unique_candidate_recovery(recovery_engine_t *engine,
                                         const packet_record_t *primary,
                                         const secondary_after_anchor_candidates_t *candidates,
                                         candidate_validator_fn validator,
                                         candidate_recovery_fn recover,
                                         bool log_no_valid)
{
    const packet_record_t *winner = NULL;
    recovery_candidate_validation_t best_reject = {
        false, RECOVERY_REJECT_MISSING_CANDIDATE, "missing_candidate", 0, false
    };
    uint8_t previous_cc = 0;
    bool has_previous_cc = previous_output_cc_for_record(engine, primary, &previous_cc);
    size_t valid_count = 0;
    size_t i;

    if (candidates->truncated) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            0, false, true, engine->last_primary_anchor.valid, false,
                            RECOVERY_REJECT_AMBIGUOUS, "too_many_after_anchors");
        return RECOVERY_DECISION_REJECTED;
    }

    for (i = 0; i < candidates->count; i++) {
        recovery_candidate_validation_t validation =
            validator(engine, primary, candidates->records[i]);

        if (validation.valid) {
            winner = candidates->records[i];
            valid_count++;
            if (valid_count > 1) {
                recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                    validation.missing_packets,
                                    validation.has_missing_count,
                                    true, true, true,
                                    RECOVERY_REJECT_AMBIGUOUS,
                                    "multiple_valid_after_anchors");
                return RECOVERY_DECISION_REJECTED;
            }
            continue;
        }
        if (best_reject.reason == NULL ||
            (validation.has_missing_count && !best_reject.has_missing_count) ||
            validation.reject_reason != RECOVERY_REJECT_MISSING_CANDIDATE) {
            best_reject = validation;
        }
    }

    if (valid_count == 1 && winner != NULL) {
        return recover(engine, primary, winner);
    }
    if (candidates->count > 0 &&
        (log_no_valid || validation_reject_is_definitive(&best_reject))) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            best_reject.missing_packets, best_reject.has_missing_count,
                            true, engine->last_primary_anchor.valid, true,
                            best_reject.reject_reason,
                            best_reject.reason != NULL ? best_reject.reason : "no_valid_after_anchor");
        return RECOVERY_DECISION_REJECTED;
    }
    return RECOVERY_DECISION_NONE;
}

static void update_primary_anchor(recovery_engine_t *engine, const packet_record_t *primary,
                                  const packet_record_t *secondary_match)
{
    if (primary->is_null || secondary_match == NULL || secondary_match->is_null ||
        !records_exact_non_error_match(primary, secondary_match)) {
        return;
    }

    engine->last_primary_anchor.valid = true;
    engine->last_primary_anchor.primary_index = primary->stream_index;
    engine->last_primary_anchor.secondary_index = secondary_match->stream_index;
}

recovery_engine_config_t recovery_engine_default_config(void)
{
    recovery_engine_config_t config;

    config.primary_delay_ns = RECOVERY_ENGINE_DEFAULT_PRIMARY_DELAY_NS;
    config.max_secondary_latency_ns = RECOVERY_ENGINE_DEFAULT_MAX_SECONDARY_LATENCY_NS;
    config.alignment_window_ns = RECOVERY_ENGINE_DEFAULT_ALIGNMENT_WINDOW_NS;
    config.history_ms = RECOVERY_ENGINE_DEFAULT_HISTORY_MS;
    config.primary_outage_ns = RECOVERY_ENGINE_DEFAULT_PRIMARY_OUTAGE_NS;
    config.primary_return_ns = RECOVERY_ENGINE_DEFAULT_PRIMARY_RETURN_NS;
    config.min_alignment_confidence = RECOVERY_ENGINE_DEFAULT_MIN_ALIGNMENT_CONFIDENCE;
    config.max_content_burst_packets = RECOVERY_ENGINE_DEFAULT_MAX_CONTENT_BURST_PACKETS;
    return config;
}

int recovery_engine_init(recovery_engine_t *engine, packet_sink_t *sink, report_stats_t *stats)
{
    recovery_engine_config_t config = recovery_engine_default_config();

    return recovery_engine_init_with_config(engine, sink, stats, &config);
}

int recovery_engine_init_with_config(recovery_engine_t *engine, packet_sink_t *sink, report_stats_t *stats,
                                     const recovery_engine_config_t *config)
{
    size_t capacity = history_capacity_from_config(config);

    memset(engine, 0, sizeof(*engine));
    engine->sink = sink;
    engine->stats = stats;
    engine->config = *config;
    if (engine->config.max_content_burst_packets > 255U) {
        engine->config.max_content_burst_packets = 255U;
    }
    if (engine->config.min_alignment_confidence > 100U) {
        engine->config.min_alignment_confidence = 100U;
    }

    if (packet_history_init(&engine->history[0], capacity) != 0) {
        return -1;
    }
    if (packet_history_init(&engine->history[1], capacity) != 0) {
        packet_history_free(&engine->history[0]);
        return -1;
    }
    if (delay_queue_init(&engine->primary_queue, capacity, engine->config.primary_delay_ns) != 0) {
        packet_history_free(&engine->history[1]);
        packet_history_free(&engine->history[0]);
        return -1;
    }
    if (delay_queue_init(&engine->secondary_queue, capacity, engine->config.primary_delay_ns) != 0) {
        delay_queue_free(&engine->primary_queue);
        packet_history_free(&engine->history[1]);
        packet_history_free(&engine->history[0]);
        return -1;
    }

    engine->active_output_stream_id = 0;
    engine->stats->active_output_stream_id = 0;
    return 0;
}

int recovery_engine_push_packet(recovery_engine_t *engine, int stream_id, const uint8_t packet[TS_PACKET_SIZE],
                                const ts_packet_info_t *info)
{
    packet_record_t *record;
    uint64_t history_retention_ns;

    if (stream_id < 0 || stream_id > 1) {
        return -1;
    }

    record = packet_history_push(&engine->history[stream_id]);
    memset(record, 0, sizeof(*record));
    record->source_stream_id = stream_id;
    record->stream_index = engine->next_stream_index[stream_id]++;
    gettimeofday(&record->arrival_time, NULL);
    record->arrival_time_ns = report_stats_now_ns();
    record->pid = info->pid;
    record->continuity_counter = info->continuity_counter;
    record->has_payload = info->has_payload;
    record->has_pcr = info->has_pcr;
    record->is_null = info->is_null;
    record->transport_error = info->transport_error;
    record->discontinuity_indicator = info->discontinuity_indicator;
    record->stream_continuity_errors = engine->stats->continuity_errors[stream_id];
    record->stream_duplicate_counters = engine->stats->duplicate_counters[stream_id];
    record->pcr_value = info->has_pcr ? info->pcr_base : 0;
    record->hash = hash_packet(packet);
    memcpy(record->packet, packet, TS_PACKET_SIZE);
    engine->last_input_arrival_ns[stream_id] = record->arrival_time_ns;
    history_retention_ns = engine->config.history_ms * 1000000ULL;
    if (history_retention_ns > 0 && record->arrival_time_ns > history_retention_ns) {
        packet_history_prune_older_than(&engine->history[stream_id],
                                        record->arrival_time_ns - history_retention_ns);
    }

    engine->decision_candidate_count = 0;
    engine->decision_candidates_truncated = false;
    update_pcr_timing(engine, record);
    observe_input_path_gap(engine, stream_id, record);
    update_alignment(engine, stream_id, record);

    if (stream_id == 0) {
        if (delay_queue_push(&engine->primary_queue, record) != 0) {
            engine->stats->primary_delay_overflows++;
            delay_queue_pop(&engine->primary_queue);
            if (delay_queue_push(&engine->primary_queue, record) != 0) {
                return -1;
            }
        }
    } else if (delay_queue_push(&engine->secondary_queue, record) != 0) {
        delay_queue_pop(&engine->secondary_queue);
        if (delay_queue_push(&engine->secondary_queue, record) != 0) {
            return -1;
        }
    }

    return recovery_engine_drain(engine, false);
}

int recovery_engine_drain(recovery_engine_t *engine, bool force)
{
    uint64_t now_ns = report_stats_now_ns();

    if (engine->primary_queue.count == 0 ||
        !primary_return_holdoff_elapsed(engine, now_ns)) {
        return drain_secondary_on_primary_outage(engine, force, now_ns);
    }
    discard_primary_records_already_output_by_secondary(engine);
    if (engine->primary_queue.count == 0) {
        return drain_secondary_on_primary_outage(engine, force, now_ns);
    }
    set_active_output_stream(engine, 0);

    while (engine->primary_queue.count > 0) {
        packet_record_t *primary = delay_queue_front(&engine->primary_queue);
        secondary_after_anchor_candidates_t secondary_matches;
        packet_record_t *single_secondary_match = NULL;
        packet_record_t *anchor_secondary_match = NULL;
        bool primary_fits;
        bool logged_recovery_decision = false;
        bool inserted_recovery = false;

        if (primary == NULL) {
            break;
        }
        now_ns = report_stats_now_ns();
        if (engine->active_output_stream_id == 1 &&
            !primary_return_holdoff_elapsed(engine, now_ns)) {
            return drain_secondary_on_primary_outage(engine, force, now_ns);
        }
        set_active_output_stream(engine, 0);
        if (!force && primary->arrival_time_ns <= now_ns &&
            now_ns - primary->arrival_time_ns < engine->primary_queue.delay_ns) {
            break;
        }

        find_secondary_boundary_matches(engine, primary, &secondary_matches);
        engine->decision_candidate_count = secondary_matches.count;
        engine->decision_candidates_truncated = secondary_matches.truncated;
        if (secondary_matches.count == 1 && !secondary_matches.truncated) {
            single_secondary_match = secondary_matches.records[0];
        }
        anchor_secondary_match = single_secondary_match;
        if (anchor_secondary_match == NULL && engine->alignment.has_alignment) {
            anchor_secondary_match = find_aligned_secondary_record(engine, primary);
        }
        primary_fits = record_fits_next_output(engine, primary);
        if (primary->transport_error) {
            int recovered = try_recover_primary_tei_packet(engine, primary);

            if (recovered < 0) {
                return -1;
            }
            logged_recovery_decision = recovered != RECOVERY_DECISION_NONE;
            if (recovered == RECOVERY_DECISION_INSERTED) {
                delay_queue_pop(&engine->primary_queue);
                continue;
            }
        }
        if (!inserted_recovery && !logged_recovery_decision &&
            engine->last_primary_anchor.valid && secondary_matches.count > 0) {
            int recovered = try_unique_candidate_recovery(engine, primary, &secondary_matches,
                                                          validate_null_position_candidate,
                                                          try_recover_null_position_gap,
                                                          false);

            if (recovered < 0) {
                return -1;
            }
            if (recovered != RECOVERY_DECISION_NONE) {
                logged_recovery_decision = true;
                inserted_recovery = recovered == RECOVERY_DECISION_INSERTED;
                primary_fits = record_fits_next_output(engine, primary);
            }
        }
        if (!inserted_recovery && !logged_recovery_decision &&
            engine->last_primary_anchor.valid && secondary_matches.count > 0) {
            int recovered = try_unique_candidate_recovery(engine, primary, &secondary_matches,
                                                          validate_mixed_position_candidate,
                                                          try_recover_mixed_position_gap,
                                                          false);

            if (recovered < 0) {
                return -1;
            }
            if (recovered != RECOVERY_DECISION_NONE) {
                logged_recovery_decision = true;
                inserted_recovery = recovered == RECOVERY_DECISION_INSERTED;
                primary_fits = record_fits_next_output(engine, primary);
            }
        }
        if (!inserted_recovery && !logged_recovery_decision &&
            !primary_fits &&
            engine->last_primary_anchor.valid && secondary_matches.count > 0) {
            int recovered = try_unique_candidate_recovery(engine, primary, &secondary_matches,
                                                          validate_exact_content_candidate,
                                                          try_recover_exact_content_gap,
                                                          true);

            if (recovered < 0) {
                return -1;
            }
            if (recovered != RECOVERY_DECISION_NONE) {
                logged_recovery_decision = true;
                inserted_recovery = recovered == RECOVERY_DECISION_INSERTED;
                primary_fits = record_fits_next_output(engine, primary);
            }
        }
        if (!primary_fits && !logged_recovery_decision) {
            uint8_t previous_cc = 0;
            uint64_t missing_count = 0;
            bool has_previous_cc = previous_output_cc_for_record(engine, primary, &previous_cc);
            bool has_missing_count = has_previous_cc &&
                                     infer_missing_from_cc(previous_cc, primary, &missing_count);
            bool low_alignment = engine->alignment.confidence < engine->config.min_alignment_confidence;
            bool before_anchor = engine->last_primary_anchor.valid;
            const char *reason = "missing_anchor_or_candidate";
            recovery_reject_reason_t reject_reason = RECOVERY_REJECT_MISSING_CANDIDATE;

            if (low_alignment) {
                reason = "low_alignment";
                reject_reason = RECOVERY_REJECT_LOW_ALIGNMENT;
            } else if (before_anchor && secondary_matches.count == 0) {
                reason = "no_after_anchor";
                reject_reason = RECOVERY_REJECT_AMBIGUOUS;
            } else if (secondary_matches.truncated) {
                reason = "too_many_after_anchors";
                reject_reason = RECOVERY_REJECT_AMBIGUOUS;
            }

            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_count, has_missing_count,
                                secondary_matches.count > 0,
                                before_anchor,
                                secondary_matches.count > 0,
                                reject_reason, reason);
        }
        if (output_record(engine, primary) != 0) {
            return -1;
        }
        update_primary_anchor(engine, primary, anchor_secondary_match);
        engine->primary_gap.valid = true;
        engine->primary_gap.arrival_time_ns = primary->arrival_time_ns;
        engine->primary_gap.continuity_errors = primary->stream_continuity_errors;
        engine->primary_gap.duplicate_counters = primary->stream_duplicate_counters;
        delay_queue_pop(&engine->primary_queue);
    }

    if (engine->primary_queue.count == 0) {
        return drain_secondary_on_primary_outage(engine, force, report_stats_now_ns());
    }
    return 0;
}

int recovery_engine_flush(recovery_engine_t *engine)
{
    if (recovery_engine_drain(engine, true) != 0) {
        return -1;
    }
    return packet_sink_flush(engine->sink);
}

void recovery_engine_free(recovery_engine_t *engine)
{
    delay_queue_free(&engine->secondary_queue);
    delay_queue_free(&engine->primary_queue);
    packet_history_free(&engine->history[1]);
    packet_history_free(&engine->history[0]);
    memset(engine, 0, sizeof(*engine));
}
