#include "recovery_engine.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OUTPUT_TS_PACKETS_PER_DATAGRAM 7U
#define STAGE1_BOUNDARY_SEARCH_EXTRA 8U
#define WIDE_ALIGNMENT_PROBE_INTERVAL 512ULL
#define RECOVERY_DECISION_NONE 0
#define RECOVERY_DECISION_INSERTED 1
#define RECOVERY_DECISION_REJECTED 2

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
    printf(" align=%u candidate=%s before=%s after=%s decision=%s",
           engine->alignment.confidence, yes_no(candidate_available),
           yes_no(before_anchor), yes_no(after_anchor), decision);
    if (reason != NULL && reason[0] != '\0') {
        printf(" reason=%s", reason);
    }
    printf("\n");
    fflush(stdout);
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

static packet_record_t *find_matching_record_near(packet_history_t *history,
                                                  const packet_record_t *record,
                                                  int64_t expected_index,
                                                  uint64_t radius)
{
    int64_t delta;

    for (delta = -(int64_t)radius; delta <= (int64_t)radius; delta++) {
        int64_t index = expected_index + delta;
        packet_record_t *candidate;

        if (index < 0) {
            continue;
        }
        candidate = packet_history_find_index(history, (uint64_t)index);
        if (candidate != NULL && records_match(record, candidate)) {
            return candidate;
        }
    }
    return NULL;
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

static packet_record_t *find_secondary_boundary_match(recovery_engine_t *engine,
                                                      const packet_record_t *primary)
{
    uint64_t first_index;
    uint64_t last_index;
    uint64_t index;
    int64_t expected_index;
    packet_record_t *match;

    if (primary->transport_error || primary->discontinuity_indicator) {
        return NULL;
    }
    if (engine->alignment.has_alignment) {
        expected_index = (int64_t)primary->stream_index + engine->alignment.offset_packets;
        match = find_matching_record_near(&engine->history[1], primary, expected_index,
                                          STAGE1_BOUNDARY_SEARCH_EXTRA);
        if (match != NULL) {
            return match;
        }
        if (expected_index + (int64_t)STAGE1_BOUNDARY_SEARCH_EXTRA >= 0) {
            uint64_t first_exact = expected_index > (int64_t)STAGE1_BOUNDARY_SEARCH_EXTRA
                                       ? (uint64_t)(expected_index - (int64_t)STAGE1_BOUNDARY_SEARCH_EXTRA)
                                       : 0ULL;
            uint64_t last_exact = (uint64_t)(expected_index + (int64_t)STAGE1_BOUNDARY_SEARCH_EXTRA);

            for (index = first_exact; index <= last_exact; index++) {
                packet_record_t *candidate = packet_history_find_index(&engine->history[1], index);

                if (candidate != NULL && records_exact_non_error_match(primary, candidate)) {
                    return candidate;
                }
            }
        }
    }
    if (!engine->last_primary_anchor.valid ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return NULL;
    }

    first_index = engine->last_primary_anchor.secondary_index + 1ULL;
    last_index = first_index +
                 (primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL) +
                 (uint64_t)engine->config.max_content_burst_packets;
    for (index = first_index; index <= last_index; index++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1], index);

        if (candidate != NULL && records_exact_non_error_match(primary, candidate)) {
            return candidate;
        }
    }

    return NULL;
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
    packet_record_t *candidates[16] = {0};
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
    if (!has_cc_missing_count || cc_missing_count == 0) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            cc_missing_count, has_cc_missing_count, true, true, true,
                            RECOVERY_REJECT_BURST_TOO_LARGE, "not_content_gap");
        return RECOVERY_DECISION_REJECTED;
    }
    if (cc_missing_count > engine->config.max_content_burst_packets) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            cc_missing_count, true, true, true, true,
                            RECOVERY_REJECT_BURST_TOO_LARGE, "burst_too_large");
        return RECOVERY_DECISION_REJECTED;
    }
    if (missing_packets > cc_missing_count) {
        return 0;
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

    for (candidate_count = 0; candidate_count < cc_missing_count; candidate_count++) {
        uint64_t index;
        packet_record_t *candidate = NULL;

        expected_missing_cc = (uint8_t)((previous_cc + 1U + candidate_count) & 0x0fU);
        for (index = engine->last_primary_anchor.secondary_index + 1ULL;
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
    if (primary_between != 0) {
        return RECOVERY_DECISION_NONE;
    }
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
        if (!record_fits_output_states(states, candidate)) {
            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_packets, true, true, true, true,
                                RECOVERY_REJECT_WRONG_COUNTER, "candidate_counter_contradiction");
            return RECOVERY_DECISION_REJECTED;
        }
        observe_output_states(states, candidate);
    }

    primary_fits = record_fits_output_states(states, primary);
    if (!primary_fits) {
        recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                            missing_packets, true, true, true, true,
                            RECOVERY_REJECT_WRONG_COUNTER, "after_anchor_wrong_counter");
        return RECOVERY_DECISION_REJECTED;
    }

    recovery_decision_log(engine, 0, primary, previous_cc, has_previous_cc,
                          missing_packets, true, true, true, true,
                          "recover_mixed_burst", NULL);
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

    while (engine->primary_queue.count > 0) {
        packet_record_t *primary = delay_queue_front(&engine->primary_queue);
        packet_record_t *secondary_match;
        bool primary_fits;
        bool logged_recovery_decision = false;
        bool inserted_recovery = false;

        if (primary == NULL) {
            break;
        }
        now_ns = report_stats_now_ns();
        if (!force && primary->arrival_time_ns <= now_ns &&
            now_ns - primary->arrival_time_ns < engine->primary_queue.delay_ns) {
            break;
        }

        secondary_match = find_secondary_boundary_match(engine, primary);
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
        if (!primary_fits && engine->last_primary_anchor.valid && secondary_match != NULL) {
            int recovered = try_recover_exact_content_gap(engine, primary, secondary_match);

            if (recovered < 0) {
                return -1;
            }
            logged_recovery_decision = recovered != RECOVERY_DECISION_NONE;
            inserted_recovery = recovered == RECOVERY_DECISION_INSERTED;
            primary_fits = record_fits_next_output(engine, primary);
        }
        if (!inserted_recovery && !logged_recovery_decision &&
            engine->last_primary_anchor.valid && secondary_match != NULL) {
            int recovered = try_recover_mixed_position_gap(engine, primary, secondary_match);

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
            } else if (before_anchor && secondary_match == NULL) {
                reason = "no_after_anchor";
                reject_reason = RECOVERY_REJECT_AMBIGUOUS;
            }

            recovery_reject_log(engine, primary, previous_cc, has_previous_cc,
                                missing_count, has_missing_count,
                                secondary_match != NULL,
                                before_anchor,
                                secondary_match != NULL,
                                reject_reason, reason);
        }
        if (output_record(engine, primary) != 0) {
            return -1;
        }
        update_primary_anchor(engine, primary, secondary_match);
        engine->primary_gap.valid = true;
        engine->primary_gap.arrival_time_ns = primary->arrival_time_ns;
        engine->primary_gap.continuity_errors = primary->stream_continuity_errors;
        engine->primary_gap.duplicate_counters = primary->stream_duplicate_counters;
        delay_queue_pop(&engine->primary_queue);
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
