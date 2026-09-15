#include "recovery_engine.h"
#include "output_udp.h"

#include <stdlib.h>
#include <string.h>

#define FAILOVER_ALIGNMENT_MIN_INFORMATIVE_PACKETS 128U
#define FAILOVER_ALIGNMENT_MAX_CANDIDATES 32768U
#define ALIGNMENT_STREAM_INDEX_SEARCH_PACKETS 64
#define ALIGNMENT_PCR_INDEX_SEARCH_PACKETS 2048
#define RECOVERY_INDEX_START_SEARCH_PACKETS 1024U

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

static int primary_delay_queue_init(primary_delay_queue_t *queue, size_t capacity, uint64_t delay_ns)
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

static void primary_delay_queue_free(primary_delay_queue_t *queue)
{
    free(queue->records);
    memset(queue, 0, sizeof(*queue));
}

static packet_record_t *primary_delay_queue_front(primary_delay_queue_t *queue)
{
    if (queue->count == 0) {
        return NULL;
    }

    return &queue->records[queue->start];
}

static void primary_delay_queue_pop(primary_delay_queue_t *queue)
{
    if (queue->count == 0) {
        return;
    }

    queue->start = (queue->start + 1U) % queue->capacity;
    queue->count--;
}

static int primary_delay_queue_push(primary_delay_queue_t *queue, const packet_record_t *record)
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

static packet_record_t *packet_history_get_newest(packet_history_t *history, size_t age)
{
    size_t index;

    if (age >= history->count) {
        return NULL;
    }

    index = (history->start + history->count - 1U - age) % history->capacity;
    return &history->records[index];
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

static bool record_is_informative(const packet_record_t *record)
{
    return !record->is_null && !record->transport_error;
}

static uint64_t time_difference_ns(uint64_t a, uint64_t b)
{
    return a >= b ? a - b : b - a;
}

static void observe_output_record(recovery_engine_t *engine, const packet_record_t *record)
{
    output_pid_state_t *state;

    if (record->is_null || record->transport_error || record->discontinuity_indicator) {
        return;
    }

    state = &engine->output_pid_state[record->pid];
    if (state->valid) {
        if (record->has_payload) {
            uint8_t expected = (uint8_t)((state->continuity_counter + 1U) & 0x0fU);
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

static bool continuity_gap_for_record(recovery_engine_t *engine, const packet_record_t *record,
                                      uint8_t *expected_counter, uint8_t *missing_count)
{
    output_pid_state_t *state;
    uint8_t expected;
    uint8_t gap;

    if (record->is_null || record->transport_error || record->discontinuity_indicator || !record->has_payload) {
        return false;
    }

    state = &engine->output_pid_state[record->pid];
    if (!state->valid) {
        return false;
    }

    expected = (uint8_t)((state->continuity_counter + 1U) & 0x0fU);
    if (record->continuity_counter == expected || record->continuity_counter == state->continuity_counter) {
        return false;
    }

    gap = (uint8_t)((record->continuity_counter + 16U - expected) & 0x0fU);
    if (gap == 0) {
        return false;
    }

    *expected_counter = expected;
    *missing_count = gap;
    return true;
}

static bool record_fits_next_output(recovery_engine_t *engine, const packet_record_t *record)
{
    output_pid_state_t *state;
    uint8_t expected;

    if (record->is_null) {
        return true;
    }
    if (record->transport_error || record->discontinuity_indicator) {
        return false;
    }

    state = &engine->output_pid_state[record->pid];
    if (!state->valid) {
        return true;
    }

    if (!record->has_payload) {
        return record->continuity_counter == state->continuity_counter;
    }

    expected = (uint8_t)((state->continuity_counter + 1U) & 0x0fU);
    return record->continuity_counter == expected;
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

static packet_record_t *delay_queue_get(primary_delay_queue_t *queue, size_t offset)
{
    if (offset >= queue->count) {
        return NULL;
    }
    return &queue->records[(queue->start + offset) % queue->capacity];
}

static bool delay_queue_run_fits_output(recovery_engine_t *engine,
                                        primary_delay_queue_t *queue,
                                        size_t start_offset)
{
    output_pid_state_t states[REPORT_STATS_PIDS];
    size_t informative = 0;
    size_t offset;

    memcpy(states, engine->output_pid_state, sizeof(states));
    for (offset = start_offset; offset < queue->count; offset++) {
        packet_record_t *record = delay_queue_get(queue, offset);

        if (record == NULL) {
            break;
        }
        if (!record_fits_output_states(states, record)) {
            return false;
        }
        observe_output_states(states, record);
        if (record_is_informative(record)) {
            informative++;
            if (informative >= FAILOVER_ALIGNMENT_MIN_INFORMATIVE_PACKETS) {
                return true;
            }
        }
    }

    return informative > 0 && start_offset == 0;
}

static bool align_delay_queue_to_output(recovery_engine_t *engine, primary_delay_queue_t *queue)
{
    size_t limit = queue->count < FAILOVER_ALIGNMENT_MAX_CANDIDATES
                       ? queue->count
                       : FAILOVER_ALIGNMENT_MAX_CANDIDATES;
    size_t offset;

    for (offset = 0; offset < limit; offset++) {
        if (delay_queue_run_fits_output(engine, queue, offset)) {
            while (offset > 0) {
                primary_delay_queue_pop(queue);
                offset--;
            }
            return true;
        }
    }

    report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
    return false;
}

static uint64_t packet_info_pcr_value(const ts_packet_info_t *info)
{
    return (info->pcr_base * 300ULL) + info->pcr_extension;
}

static uint64_t pcr_delta(uint64_t later, uint64_t earlier)
{
    const uint64_t wrap = 1ULL << 42U;

    if (later >= earlier) {
        return later - earlier;
    }

    return (wrap - earlier) + later;
}

static int64_t signed_pcr_delta(uint64_t later, uint64_t earlier)
{
    uint64_t forward = pcr_delta(later, earlier);
    uint64_t backward = pcr_delta(earlier, later);

    return forward <= backward ? (int64_t)forward : -(int64_t)backward;
}

static double pcr_ticks_to_ns(int64_t ticks)
{
    return ((double)ticks / 27000000.0) * 1000000000.0;
}

static void observe_pcr_arrival_delay(recovery_engine_t *engine, int stream_id,
                                      const packet_record_t *record)
{
    packet_history_t *history = &engine->history[stream_id == 0 ? 1 : 0];
    packet_record_t *best = NULL;
    uint64_t best_pcr_delta = UINT64_MAX;
    size_t max_search = history->count < RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS
                            ? history->count
                            : RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS;
    size_t i;

    if (!record->has_pcr) {
        return;
    }

    for (i = 0; i < max_search; i++) {
        packet_record_t *candidate = packet_history_get_newest(history, i);
        int64_t pcr_delta_ticks;
        uint64_t pcr_delta_abs;

        if (candidate == NULL) {
            break;
        }
        if (!candidate->has_pcr || candidate->pid != record->pid) {
            continue;
        }
        if (engine->config.alignment_window_ns > 0 &&
            time_difference_ns(record->arrival_time_ns, candidate->arrival_time_ns) >
                engine->config.alignment_window_ns) {
            continue;
        }

        pcr_delta_ticks = signed_pcr_delta(record->pcr_value, candidate->pcr_value);
        pcr_delta_abs = pcr_delta_ticks >= 0 ? (uint64_t)pcr_delta_ticks : (uint64_t)-pcr_delta_ticks;
        if (pcr_delta_abs < best_pcr_delta) {
            best_pcr_delta = pcr_delta_abs;
            best = candidate;
        }
    }

    if (best != NULL) {
        double sample_ns;

        if (stream_id == 0) {
            sample_ns = ((double)best->arrival_time_ns - (double)record->arrival_time_ns) -
                        pcr_ticks_to_ns(signed_pcr_delta(best->pcr_value, record->pcr_value));
        } else {
            sample_ns = ((double)record->arrival_time_ns - (double)best->arrival_time_ns) -
                        pcr_ticks_to_ns(signed_pcr_delta(record->pcr_value, best->pcr_value));
        }
        if (engine->config.max_secondary_latency_ns == 0 ||
            sample_ns <= (double)engine->config.max_secondary_latency_ns) {
            report_stats_observe_pcr_delay(engine->stats, sample_ns);
        }
    }
}

static pcr_pid_model_t *get_pcr_pid_model(pcr_timing_model_t *model, uint16_t pid)
{
    pcr_pid_model_t *empty = NULL;
    size_t i;

    for (i = 0; i < RECOVERY_ENGINE_MAX_PCR_PIDS; i++) {
        if (model->pid_models[i].active && model->pid_models[i].pid == pid) {
            return &model->pid_models[i];
        }
        if (!model->pid_models[i].active && empty == NULL) {
            empty = &model->pid_models[i];
        }
    }

    if (empty != NULL) {
        memset(empty, 0, sizeof(*empty));
        empty->active = true;
        empty->pid = pid;
    }

    return empty;
}

static void refresh_pcr_summary(recovery_engine_t *engine)
{
    int stream_id;

    for (stream_id = 0; stream_id < 2; stream_id++) {
        double best_bitrate = 0.0;
        uint32_t best_confidence = 0;
        size_t i;

        for (i = 0; i < RECOVERY_ENGINE_MAX_PCR_PIDS; i++) {
            pcr_pid_model_t *pid_model = &engine->pcr_model[stream_id].pid_models[i];
            if (pid_model->active && pid_model->confidence > best_confidence) {
                best_confidence = pid_model->confidence;
                best_bitrate = pid_model->bitrate_bps;
            }
        }

        engine->pcr_model[stream_id].confidence = best_confidence;
        engine->stats->pcr_timing_confidence[stream_id] = best_confidence;
        engine->stats->pcr_bitrate_bps[stream_id] = best_bitrate;
    }
}

static void update_pcr_timing(recovery_engine_t *engine, int stream_id, const packet_record_t *record)
{
    pcr_pid_model_t *pid_model;

    if (!record->has_pcr) {
        return;
    }

    pid_model = get_pcr_pid_model(&engine->pcr_model[stream_id], record->pid);
    if (pid_model == NULL) {
        return;
    }

    if (pid_model->confidence > 0) {
        uint64_t packet_delta = record->stream_index - pid_model->last_stream_index;
        uint64_t pcr_ticks = pcr_delta(record->pcr_value, pid_model->last_pcr);

        if (packet_delta > 0 && pcr_ticks > 0) {
            double pcr_seconds = (double)pcr_ticks / 27000000.0;
            double arrival_delta_ns = (double)(record->arrival_time_ns - pid_model->last_arrival_time_ns);
            double expected_delta_ns = pcr_seconds * 1000000000.0;
            double sample_bitrate = ((double)packet_delta * 188.0 * 8.0) / pcr_seconds;
            double sample_packets_per_second = (double)packet_delta / pcr_seconds;
            double sample_jitter = arrival_delta_ns > expected_delta_ns
                                       ? arrival_delta_ns - expected_delta_ns
                                       : expected_delta_ns - arrival_delta_ns;

            if (pid_model->bitrate_bps == 0.0) {
                pid_model->bitrate_bps = sample_bitrate;
                pid_model->packets_per_second = sample_packets_per_second;
                pid_model->jitter_ns = sample_jitter;
            } else {
                pid_model->bitrate_bps = (pid_model->bitrate_bps * 0.875) + (sample_bitrate * 0.125);
                pid_model->packets_per_second = (pid_model->packets_per_second * 0.875) +
                                                (sample_packets_per_second * 0.125);
                pid_model->jitter_ns = (pid_model->jitter_ns * 0.875) + (sample_jitter * 0.125);
            }

            if (pid_model->confidence < 100) {
                pid_model->confidence += 4;
                if (pid_model->confidence > 100) {
                    pid_model->confidence = 100;
                }
            }
        }
    } else {
        pid_model->confidence = 1;
    }

    pid_model->last_pcr = record->pcr_value;
    pid_model->last_stream_index = record->stream_index;
    pid_model->last_arrival_time_ns = record->arrival_time_ns;

    if (pid_model->packets_per_second > 0.0) {
        engine->pcr_model[stream_id].estimated_delay_ns =
            ((double)record->stream_index / pid_model->packets_per_second * 1000000000.0) -
            (double)record->arrival_time_ns;
    }

    observe_pcr_arrival_delay(engine, stream_id, record);
    refresh_pcr_summary(engine);
}

static int score_alignment_match(const packet_record_t *record, const packet_record_t *candidate)
{
    int score = 0;

    if (!record_is_informative(record) || !record_is_informative(candidate)) {
        return 0;
    }

    if (record->hash == candidate->hash) {
        score += 8;
    }
    if (record->pid == candidate->pid && record->continuity_counter == candidate->continuity_counter) {
        score += 2;
    }
    if (record->has_pcr && candidate->has_pcr) {
        score += 2;
        if (record->pid == candidate->pid) {
            int64_t delta = signed_pcr_delta(record->pcr_value, candidate->pcr_value);
            uint64_t abs_delta = delta >= 0 ? (uint64_t)delta : (uint64_t)-delta;

            if (abs_delta <= 27U) {
                score += 8;
            }
        }
    }

    return score;
}

static bool records_within_alignment_window(recovery_engine_t *engine, const packet_record_t *record,
                                            const packet_record_t *candidate)
{
    return engine->config.alignment_window_ns == 0 ||
           time_difference_ns(record->arrival_time_ns, candidate->arrival_time_ns) <=
               engine->config.alignment_window_ns;
}

static bool secondary_match_within_latency(recovery_engine_t *engine, const packet_record_t *primary,
                                           const packet_record_t *secondary)
{
    if (engine->config.max_secondary_latency_ns == 0) {
        return true;
    }
    if (secondary->arrival_time_ns < primary->arrival_time_ns) {
        return true;
    }

    if (secondary->arrival_time_ns - primary->arrival_time_ns <=
        engine->config.max_secondary_latency_ns) {
        return true;
    }

    report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_SECONDARY_LATE);
    return false;
}

static packet_record_t *find_secondary_match_for_primary(recovery_engine_t *engine,
                                                         const packet_record_t *primary)
{
    int64_t expected;
    int64_t delta;
    packet_history_t *secondary = &engine->history[1];

    if (!record_is_informative(primary)) {
        return NULL;
    }

    if (engine->alignment.has_alignment &&
        engine->alignment.confidence >= engine->config.min_alignment_confidence / 2U) {
        expected = (int64_t)primary->stream_index + engine->alignment.offset_packets;
        for (delta = -8; delta <= 8; delta++) {
            int64_t candidate_index = expected + delta;
            packet_record_t *candidate;

            if (candidate_index < 0) {
                continue;
            }

            candidate = packet_history_find_index(secondary, (uint64_t)candidate_index);
            if (candidate != NULL && secondary_match_within_latency(engine, primary, candidate) &&
                score_alignment_match(primary, candidate) >= RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD) {
                return candidate;
            }
        }
    }

    if (engine->last_primary_anchor.valid &&
        primary->stream_index > engine->last_primary_anchor.primary_index) {
        uint64_t primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
        uint64_t first_index = engine->last_primary_anchor.secondary_index + 1ULL;
        uint64_t last_index = first_index + primary_between +
                              (uint64_t)engine->config.max_content_burst_packets;
        uint64_t index;

        for (index = first_index; index <= last_index; index++) {
            packet_record_t *candidate = packet_history_find_index(secondary, index);

            if (candidate == NULL) {
                continue;
            }
            if (candidate->stream_index <= engine->last_primary_anchor.secondary_index) {
                continue;
            }
            if (secondary_match_within_latency(engine, primary, candidate) &&
                score_alignment_match(primary, candidate) >= RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD) {
                return candidate;
            }
        }
    }

    if (!engine->alignment.has_alignment ||
        engine->alignment.confidence < engine->config.min_alignment_confidence / 2U) {
        report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_LOW_ALIGNMENT);
    }

    return NULL;
}

static bool secondary_range_is_all_null(recovery_engine_t *engine, uint64_t first_index, uint64_t last_index)
{
    uint64_t index;

    if (first_index > last_index) {
        return false;
    }

    for (index = first_index; index <= last_index; index++) {
        packet_record_t *record = packet_history_find_index(&engine->history[1], index);
        if (record == NULL || !record->is_null || record->transport_error) {
            return false;
        }
    }

    return true;
}

static int recover_nulls_before_primary(recovery_engine_t *engine, const packet_record_t *primary,
                                        const packet_record_t *secondary_match)
{
    uint64_t primary_between;
    uint64_t secondary_between;
    uint64_t missing_nulls;
    uint64_t output_index;

    if (!engine->last_primary_anchor.valid || secondary_match == NULL ||
        secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return 0;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (secondary_between <= primary_between) {
        return 0;
    }

    missing_nulls = secondary_between - primary_between;
    if (!secondary_range_is_all_null(engine, engine->last_primary_anchor.secondary_index + 1ULL,
                                     secondary_match->stream_index - 1ULL)) {
        engine->stats->unrecoverable_null_regions++;
        return 0;
    }

    for (output_index = secondary_match->stream_index - missing_nulls; output_index < secondary_match->stream_index;
         output_index++) {
        packet_record_t *null_record = packet_history_find_index(&engine->history[1], output_index);
        if (null_record == NULL || output_record(engine, null_record) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, true);
    }

    return 0;
}

static int recover_anchored_secondary_gap_before_primary(recovery_engine_t *engine,
                                                         const packet_record_t *primary,
                                                         const packet_record_t *secondary_match)
{
    uint64_t primary_between;
    uint64_t secondary_between;
    uint64_t missing_packets;
    uint64_t index;
    packet_record_t *candidates[RECOVERY_ENGINE_MAX_STREAM_GAP_RECOVERY_PACKETS];
    output_pid_state_t states[REPORT_STATS_PIDS];
    uint64_t recovered = 0;
    uint64_t recovered_content = 0;
    uint8_t expected_counter;
    uint8_t cc_missing_count;
    bool primary_cc_gap;

    if (!engine->last_primary_anchor.valid || secondary_match == NULL ||
        secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return 0;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (secondary_between <= primary_between) {
        return 0;
    }

    missing_packets = secondary_between - primary_between;
    primary_cc_gap = continuity_gap_for_record(engine, primary, &expected_counter, &cc_missing_count);
    if (missing_packets < 16U) {
        if (!primary_cc_gap) {
            return 0;
        }
        if ((uint64_t)cc_missing_count != missing_packets) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_AMBIGUOUS);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
    }
    if (missing_packets > engine->config.max_content_burst_packets) {
        report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_BURST_TOO_LARGE);
        return 0;
    }
    if (missing_packets > RECOVERY_ENGINE_MAX_STREAM_GAP_RECOVERY_PACKETS) {
        report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_BURST_TOO_LARGE);
        return 0;
    }

    memcpy(states, engine->output_pid_state, sizeof(states));
    for (index = engine->last_primary_anchor.secondary_index + 1ULL; index < secondary_match->stream_index; index++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1], index);

        if (candidate == NULL) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_MISSING_CANDIDATE);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        if (candidate->transport_error) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_TEI);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        if (candidate->discontinuity_indicator) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_DISCONTINUITY);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        if (missing_packets < 16U && !candidate->is_null && candidate->pid != primary->pid) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_PID);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        if (!record_fits_output_states(states, candidate)) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        observe_output_states(states, candidate);
        candidates[recovered] = candidate;
        if (!candidate->is_null) {
            recovered_content++;
        }
        recovered++;
    }

    if (recovered != missing_packets) {
        report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_MISSING_CANDIDATE);
        engine->stats->unrecoverable_loss++;
        return 0;
    }
    for (index = 0; index < recovered; index++) {
        if (output_record(engine, candidates[index]) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, candidates[index]->is_null);
    }
    if (recovered_content > 1) {
        engine->stats->recovered_content_bursts++;
    }

    return (int)recovered;
}

static int recover_content_burst_by_counter_before_primary(recovery_engine_t *engine,
                                                           const packet_record_t *primary,
                                                           uint8_t expected_counter,
                                                           uint8_t missing_count);

static int64_t estimated_secondary_time_offset_ns(recovery_engine_t *engine)
{
    if (engine->stats->pcr_timing_confidence[0] >= 70U &&
        engine->stats->pcr_timing_confidence[1] >= 70U) {
        return (int64_t)engine->stats->pcr_delay_ns;
    }

    if (engine->stats->observed_secondary_latency_samples > 0) {
        return (int64_t)engine->stats->observed_secondary_latency_avg_ns;
    }

    return 0;
}

static uint64_t secondary_failover_output_delay_ns(recovery_engine_t *engine)
{
    double latency_ns = 0.0;
    double pcr_latency_ns = 0.0;

    if (engine->stats->observed_secondary_latency_samples > 0) {
        latency_ns = engine->stats->observed_secondary_latency_avg_ns;
    }
    if (engine->stats->pcr_timing_confidence[0] >= 70U &&
        engine->stats->pcr_timing_confidence[1] >= 70U) {
        pcr_latency_ns = engine->stats->pcr_delay_ns < 0.0
                             ? -engine->stats->pcr_delay_ns
                             : engine->stats->pcr_delay_ns;
        if (pcr_latency_ns > latency_ns) {
            latency_ns = pcr_latency_ns;
        }
    }

    if (latency_ns <= 0.0) {
        return engine->config.primary_delay_ns;
    }
    if (latency_ns >= (double)engine->config.primary_delay_ns) {
        return 0;
    }

    return engine->config.primary_delay_ns - (uint64_t)latency_ns;
}

static bool estimated_alignment_offset_packets(recovery_engine_t *engine, int64_t *offset_packets)
{
    double bitrate_bps;
    double packets_per_second;
    double offset;

    if (engine->alignment.has_alignment) {
        *offset_packets = engine->alignment.offset_packets;
        return true;
    }
    if (engine->stats->pcr_timing_confidence[0] < 70U ||
        engine->stats->pcr_timing_confidence[1] < 70U) {
        return false;
    }

    bitrate_bps = engine->stats->pcr_bitrate_bps[0] > 0.0
                      ? engine->stats->pcr_bitrate_bps[0]
                      : engine->stats->pcr_bitrate_bps[1];
    if (bitrate_bps <= 0.0) {
        return false;
    }

    packets_per_second = bitrate_bps / ((double)TS_PACKET_SIZE * 8.0);
    offset = (engine->stats->pcr_delay_ns / 1000000000.0) * packets_per_second;
    *offset_packets = offset >= 0.0 ? (int64_t)(offset + 0.5) : (int64_t)(offset - 0.5);
    return true;
}

static uint64_t apply_time_offset(uint64_t value, int64_t offset)
{
    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-offset);
        return value > magnitude ? value - magnitude : 0;
    }

    return value + (uint64_t)offset;
}

static void set_active_output_stream(recovery_engine_t *engine, int stream_id)
{
    int previous_stream_id = engine->active_output_stream_id;

    if (engine->active_output_stream_id == stream_id) {
        return;
    }

    engine->active_output_stream_id = stream_id;
    engine->stats->active_output_stream_id = stream_id;
    engine->stats->source_switches[stream_id]++;
    if (stream_id == 1) {
        engine->primary_return_start_ns = 0;
        engine->primary_switchback_guard = false;
        engine->primary_switchback_guard_informative = 0;
    } else if (previous_stream_id == 1) {
        engine->primary_switchback_guard = true;
        engine->primary_switchback_guard_informative =
            FAILOVER_ALIGNMENT_MIN_INFORMATIVE_PACKETS;
    }
}

static void prune_secondary_queue_through(recovery_engine_t *engine, uint64_t secondary_index)
{
    while (engine->secondary_queue.count > 0) {
        packet_record_t *record = primary_delay_queue_front(&engine->secondary_queue);
        if (record == NULL || record->stream_index > secondary_index) {
            break;
        }
        primary_delay_queue_pop(&engine->secondary_queue);
    }
}

static bool primary_record_already_covered_by_secondary(recovery_engine_t *engine,
                                                        const packet_record_t *primary)
{
    int64_t secondary_index;

    if (!engine->has_output_stream_index[1] || !engine->alignment.has_alignment) {
        return false;
    }

    secondary_index = (int64_t)primary->stream_index + engine->alignment.offset_packets;
    if (secondary_index < 0) {
        return false;
    }

    return (uint64_t)secondary_index <= engine->last_output_stream_index[1];
}

static packet_record_t *find_recent_secondary_match_for_primary(recovery_engine_t *engine,
                                                                const packet_record_t *primary)
{
    packet_history_t *secondary = &engine->history[1];
    size_t limit = secondary->count < FAILOVER_ALIGNMENT_MAX_CANDIDATES
                       ? secondary->count
                       : FAILOVER_ALIGNMENT_MAX_CANDIDATES;
    size_t age;

    if (!engine->has_output_stream_index[1] || !record_is_informative(primary)) {
        return NULL;
    }

    for (age = 0; age < limit; age++) {
        packet_record_t *candidate = packet_history_get_newest(secondary, age);

        if (candidate == NULL) {
            break;
        }
        if (candidate->hash == primary->hash &&
            score_alignment_match(primary, candidate) >= RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD &&
            records_within_alignment_window(engine, primary, candidate)) {
            return candidate;
        }
    }

    return NULL;
}

static bool align_primary_queue_for_switchback(recovery_engine_t *engine)
{
    size_t limit = engine->primary_queue.count < FAILOVER_ALIGNMENT_MAX_CANDIDATES
                       ? engine->primary_queue.count
                       : FAILOVER_ALIGNMENT_MAX_CANDIDATES;
    size_t offset;

    for (offset = 0; offset < limit; offset++) {
        packet_record_t *record = delay_queue_get(&engine->primary_queue, offset);
        packet_record_t *match;

        if (record == NULL || !delay_queue_run_fits_output(engine, &engine->primary_queue, offset)) {
            continue;
        }

        match = find_recent_secondary_match_for_primary(engine, record);
        if (match == NULL) {
            continue;
        }

        while (offset > 0) {
            primary_delay_queue_pop(&engine->primary_queue);
            offset--;
        }
        engine->alignment.has_alignment = true;
        engine->alignment.offset_packets = (int64_t)match->stream_index - (int64_t)record->stream_index;
        if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
            engine->alignment.confidence = engine->config.min_alignment_confidence;
        }
        engine->stats->alignment_offset_packets = engine->alignment.offset_packets;
        engine->stats->alignment_confidence = engine->alignment.confidence;
        return true;
    }

    report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
    return engine->config.primary_delay_ns == 0 &&
           align_delay_queue_to_output(engine, &engine->primary_queue);
}

static void prune_primary_queue_covered_by_secondary(recovery_engine_t *engine)
{
    while (engine->primary_queue.count > 0) {
        packet_record_t *record = primary_delay_queue_front(&engine->primary_queue);
        if (record == NULL) {
            break;
        }
        if (record_fits_next_output(engine, record)) {
            break;
        }
        if (!primary_record_already_covered_by_secondary(engine, record)) {
            break;
        }
        primary_delay_queue_pop(&engine->primary_queue);
    }
}

static void prune_due_primary_backlog(recovery_engine_t *engine, uint64_t now_ns)
{
    if (engine->config.primary_delay_ns == 0) {
        return;
    }

    while (engine->primary_queue.count > 0) {
        packet_record_t *record = primary_delay_queue_front(&engine->primary_queue);
        if (record == NULL ||
            record->arrival_time_ns > now_ns ||
            now_ns - record->arrival_time_ns < engine->config.primary_delay_ns) {
            break;
        }
        primary_delay_queue_pop(&engine->primary_queue);
    }
}

static bool stream_has_recent_input(recovery_engine_t *engine, int stream_id, uint64_t now_ns,
                                    uint64_t quiet_ns)
{
    return engine->last_input_arrival_ns[stream_id] != 0 &&
           now_ns >= engine->last_input_arrival_ns[stream_id] &&
           now_ns - engine->last_input_arrival_ns[stream_id] <= quiet_ns;
}

static bool failover_is_allowed(recovery_engine_t *engine)
{
    if (engine->secondary_queue.count == 0) {
        return false;
    }
    if (engine->alignment.has_alignment &&
        engine->alignment.confidence >= engine->config.min_alignment_confidence / 2U) {
        return true;
    }
    return engine->stats->pcr_timing_confidence[0] >= 70U &&
           engine->stats->pcr_timing_confidence[1] >= 70U;
}

static void maybe_update_active_source(recovery_engine_t *engine, uint64_t now_ns)
{
    bool primary_recent = stream_has_recent_input(engine, 0, now_ns, engine->config.primary_outage_ns);
    bool secondary_recent = stream_has_recent_input(engine, 1, now_ns, engine->config.primary_outage_ns);

    if (engine->active_output_stream_id == 0) {
        if (!primary_recent && secondary_recent && failover_is_allowed(engine)) {
            if (engine->last_primary_anchor.valid) {
                prune_secondary_queue_through(engine, engine->last_primary_anchor.secondary_index);
            }
            if (align_delay_queue_to_output(engine, &engine->secondary_queue)) {
                set_active_output_stream(engine, 1);
            }
        }
        return;
    }

    prune_primary_queue_covered_by_secondary(engine);
    if (!secondary_recent && engine->primary_queue.count > 0) {
        prune_due_primary_backlog(engine, now_ns);
        if (!align_delay_queue_to_output(engine, &engine->primary_queue)) {
            return;
        }
        set_active_output_stream(engine, 0);
        return;
    }

    if (!primary_recent || engine->primary_queue.count == 0) {
        engine->primary_return_start_ns = 0;
        return;
    }
    if (engine->primary_return_start_ns == 0) {
        engine->primary_return_start_ns = now_ns;
        return;
    }
    if (now_ns - engine->primary_return_start_ns >= engine->config.primary_return_ns) {
        prune_due_primary_backlog(engine, now_ns);
        if (align_primary_queue_for_switchback(engine)) {
            set_active_output_stream(engine, 0);
        }
    }
}

static int drain_secondary_failover(recovery_engine_t *engine, bool force)
{
    uint64_t now_ns = report_stats_now_ns();
    uint64_t output_delay_ns = secondary_failover_output_delay_ns(engine);

    while (engine->secondary_queue.count > 0) {
        packet_record_t *record = primary_delay_queue_front(&engine->secondary_queue);

        if (record == NULL) {
            break;
        }
        if (!force && output_delay_ns > 0 &&
            (record->arrival_time_ns > now_ns ||
             now_ns - record->arrival_time_ns < output_delay_ns)) {
            break;
        }
        if (engine->last_primary_anchor.valid &&
            record->stream_index <= engine->last_primary_anchor.secondary_index) {
            primary_delay_queue_pop(&engine->secondary_queue);
            continue;
        }
        if (!record_fits_next_output(engine, record)) {
            primary_delay_queue_pop(&engine->secondary_queue);
            continue;
        }
        if (output_record(engine, record) != 0) {
            return -1;
        }
        primary_delay_queue_pop(&engine->secondary_queue);
    }

    return 0;
}

static int recover_secondary_time_range(recovery_engine_t *engine,
                                        const packet_record_t *primary,
                                        uint64_t start_ns,
                                        uint64_t end_ns,
                                        uint64_t max_recover,
                                        bool null_only,
                                        bool emit)
{
    packet_history_t *secondary = &engine->history[1];
    uint32_t recovered = 0;
    uint32_t recovered_content = 0;
    uint64_t considered = 0;
    bool started = false;
    size_t i;

    if (start_ns >= RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS) {
        start_ns -= RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS;
    } else {
        start_ns = 0;
    }
    end_ns += RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS;

    for (i = 0; i < secondary->count; i++) {
        size_t index = (secondary->start + i) % secondary->capacity;
        packet_record_t *candidate = &secondary->records[index];

        if (candidate->arrival_time_ns <= start_ns || candidate->arrival_time_ns >= end_ns ||
            candidate->arrival_time_ns <= engine->primary_gap.last_recovered_secondary_arrival_ns) {
            continue;
        }

        if (!emit) {
            recovered++;
            continue;
        }
        if (started && considered >= max_recover) {
            break;
        }
        if (null_only && !candidate->is_null) {
            continue;
        }
        if (primary != NULL &&
            candidate->hash == primary->hash) {
            continue;
        }
        if (candidate->transport_error) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_TEI);
            if (started) {
                considered++;
            }
            continue;
        }
        if (candidate->discontinuity_indicator) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_DISCONTINUITY);
            if (started) {
                considered++;
            }
            continue;
        }
        if (!record_fits_next_output(engine, candidate)) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
            if (started) {
                considered++;
            }
            continue;
        }
        started = true;
        considered++;
        recovered++;
        if (recovered > RECOVERY_ENGINE_MAX_STREAM_GAP_RECOVERY_PACKETS) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_BURST_TOO_LARGE);
            return 0;
        }
        if (output_record(engine, candidate) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, candidate->is_null);
        if (!candidate->is_null) {
            recovered_content++;
        }
        engine->primary_gap.last_recovered_secondary_arrival_ns = candidate->arrival_time_ns;
    }

    if (emit && recovered_content > 1) {
        engine->stats->recovered_content_bursts++;
    }

    return (int)recovered;
}

static int recover_secondary_index_deficit(recovery_engine_t *engine,
                                           const packet_record_t *primary,
                                           uint64_t max_recover,
                                           bool null_only)
{
    uint64_t start_index;
    uint64_t recovered = 0;
    uint64_t recovered_content = 0;
    uint64_t scanned;

    if (!engine->last_primary_anchor.valid || max_recover == 0) {
        return 0;
    }

    start_index = engine->last_primary_anchor.secondary_index + 1ULL +
                  engine->primary_gap.recovered_stream_packets;
    if (engine->has_output_stream_index[0] &&
        engine->last_output_stream_index[0] > engine->last_primary_anchor.primary_index) {
        start_index += engine->last_output_stream_index[0] -
                       engine->last_primary_anchor.primary_index;
    }
    for (scanned = 0; scanned < max_recover; scanned++) {
        uint64_t index = start_index + scanned;
        packet_record_t *candidate = packet_history_find_index(&engine->history[1], index);

        if (candidate == NULL) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_MISSING_CANDIDATE);
            break;
        }
        if (primary != NULL && candidate->hash == primary->hash) {
            break;
        }
        if (candidate->transport_error) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_TEI);
            break;
        }
        if (candidate->discontinuity_indicator) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_DISCONTINUITY);
            break;
        }
        if (null_only && !candidate->is_null) {
            break;
        }
        if (!record_fits_next_output(engine, candidate)) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
            if (recovered == 0 && scanned + 1U < max_recover &&
                scanned < RECOVERY_INDEX_START_SEARCH_PACKETS) {
                continue;
            }
            break;
        }
        if (output_record(engine, candidate) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, candidate->is_null);
        if (!candidate->is_null) {
            recovered_content++;
        }
        engine->primary_gap.last_recovered_secondary_arrival_ns = candidate->arrival_time_ns;
        recovered++;
    }

    if (recovered_content > 1) {
        engine->stats->recovered_content_bursts++;
    }

    return (int)recovered;
}

static int recover_secondary_bridge_to_primary(recovery_engine_t *engine,
                                               const packet_record_t *primary,
                                               uint64_t max_recover)
{
    packet_history_t *secondary = &engine->history[1];
    int64_t offset_ns = estimated_secondary_time_offset_ns(engine);
    uint64_t start_arrival_ns = engine->primary_gap.last_recovered_secondary_arrival_ns;
    uint64_t recovered = 0;
    uint64_t recovered_content = 0;
    size_t i;

    if (start_arrival_ns == 0 && engine->primary_gap.valid) {
        start_arrival_ns = apply_time_offset(engine->primary_gap.arrival_time_ns, offset_ns);
        if (start_arrival_ns >= RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS) {
            start_arrival_ns -= RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS;
        }
    }

    for (i = 0; i < secondary->count; i++) {
        size_t index = (secondary->start + i) % secondary->capacity;
        packet_record_t *candidate = &secondary->records[index];

        if (candidate->arrival_time_ns <= start_arrival_ns) {
            continue;
        }
        if (candidate->hash == primary->hash) {
            break;
        }
        if (recovered >= max_recover) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_BURST_TOO_LARGE);
            break;
        }
        if (candidate->transport_error) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_TEI);
            break;
        }
        if (candidate->discontinuity_indicator) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_DISCONTINUITY);
            break;
        }
        if (!record_fits_next_output(engine, candidate)) {
            continue;
        }
        if (output_record(engine, candidate) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, candidate->is_null);
        if (!candidate->is_null) {
            recovered_content++;
        }
        engine->primary_gap.last_recovered_secondary_arrival_ns = candidate->arrival_time_ns;
        recovered++;
    }

    if (recovered_content > 1) {
        engine->stats->recovered_content_bursts++;
    }

    return (int)recovered;
}

static int recover_due_secondary_before_primary(recovery_engine_t *engine,
                                                const packet_record_t *primary,
                                                uint64_t now_ns)
{
    uint64_t output_delay_ns;
    uint64_t start_index;
    uint64_t recovered = 0;
    uint64_t recovered_content = 0;

    if (!engine->last_primary_anchor.valid) {
        return 0;
    }

    output_delay_ns = secondary_failover_output_delay_ns(engine);
    start_index = engine->last_primary_anchor.secondary_index + 1ULL;
    if (engine->has_output_stream_index[0] &&
        engine->last_output_stream_index[0] > engine->last_primary_anchor.primary_index) {
        start_index += engine->last_output_stream_index[0] -
                       engine->last_primary_anchor.primary_index;
    }
    if (engine->has_output_stream_index[1] &&
        engine->last_output_stream_index[1] + 1ULL > start_index) {
        start_index = engine->last_output_stream_index[1] + 1ULL;
    }
    if (engine->primary_gap.secondary_recovery_index > start_index) {
        start_index = engine->primary_gap.secondary_recovery_index;
    } else {
        engine->primary_gap.secondary_recovery_index = start_index;
    }

    while (recovered < RECOVERY_ENGINE_MAX_STREAM_GAP_RECOVERY_PACKETS) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1],
                                                               engine->primary_gap.secondary_recovery_index);

        if (candidate == NULL) {
            break;
        }
        if (primary != NULL && candidate->hash == primary->hash) {
            break;
        }
        if (output_delay_ns > 0 &&
            (candidate->arrival_time_ns > now_ns ||
             now_ns - candidate->arrival_time_ns < output_delay_ns)) {
            break;
        }
        engine->primary_gap.secondary_recovery_index = candidate->stream_index + 1ULL;
        if (candidate->transport_error) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_TEI);
            continue;
        }
        if (candidate->discontinuity_indicator) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_DISCONTINUITY);
            continue;
        }
        if (!record_fits_next_output(engine, candidate)) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
            continue;
        }
        if (output_record(engine, candidate) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, candidate->is_null);
        if (!candidate->is_null) {
            recovered_content++;
        }
        engine->primary_gap.last_recovered_secondary_arrival_ns = candidate->arrival_time_ns;
        recovered++;
        engine->primary_gap.recovered_stream_packets++;
    }

    if (recovered_content > 1) {
        engine->stats->recovered_content_bursts++;
    }

    return (int)recovered;
}

static int recover_stream_gap_before_primary(recovery_engine_t *engine,
                                             const packet_record_t *primary)
{
    uint64_t primary_gap_ns;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t stream_deficit;
    uint64_t max_recover;
    int64_t offset_ns;
    int recovered;
    bool content_gap;
    bool null_only;

    if (!engine->primary_gap.valid || primary->arrival_time_ns <= engine->primary_gap.arrival_time_ns) {
        return 0;
    }

    primary_gap_ns = primary->arrival_time_ns - engine->primary_gap.arrival_time_ns;
    if (primary_gap_ns < RECOVERY_ENGINE_STREAM_GAP_MIN_NS) {
        return 0;
    }
    if (engine->next_stream_index[1] <= engine->next_stream_index[0]) {
        return 0;
    }

    stream_deficit = engine->next_stream_index[1] - engine->next_stream_index[0];
    if (stream_deficit <= engine->primary_gap.recovered_stream_packets) {
        return 0;
    }
    max_recover = stream_deficit - engine->primary_gap.recovered_stream_packets;
    content_gap = primary->stream_continuity_errors > engine->primary_gap.continuity_errors ||
                  primary->stream_duplicate_counters > engine->primary_gap.duplicate_counters;
    if (content_gap && max_recover > 255U && !engine->last_primary_anchor.valid &&
        (!engine->alignment.has_alignment ||
         engine->alignment.confidence < engine->config.min_alignment_confidence / 2U)) {
        report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_BURST_TOO_LARGE);
        return 0;
    }
    null_only = !content_gap;
    if (null_only && max_recover < RECOVERY_ENGINE_MIN_NULL_GAP_RECOVERY_PACKETS) {
        return 0;
    }

    if (max_recover > 255U) {
        recovered = recover_secondary_index_deficit(engine, primary, max_recover, null_only);
        if (recovered < 0) {
            return -1;
        }
        if (recovered > 0) {
            engine->primary_gap.recovered_stream_packets += (uint64_t)recovered;
            max_recover -= (uint64_t)recovered;
            if (max_recover == 0) {
                return 0;
            }
        }
    }

    offset_ns = estimated_secondary_time_offset_ns(engine);
    start_ns = apply_time_offset(engine->primary_gap.arrival_time_ns, offset_ns);
    end_ns = apply_time_offset(primary->arrival_time_ns, offset_ns);

    recovered = recover_secondary_time_range(engine, primary, start_ns, end_ns, max_recover, null_only, true);
    if (recovered == 0 && offset_ns != 0) {
        recovered = recover_secondary_time_range(engine,
                                                primary,
                                                engine->primary_gap.arrival_time_ns,
                                                primary->arrival_time_ns,
                                                max_recover,
                                                null_only,
                                                true);
    }
    if (recovered > 0) {
        engine->primary_gap.recovered_stream_packets += (uint64_t)recovered;
    }

    return recovered < 0 ? -1 : 0;
}

static int recover_pid_time_range(recovery_engine_t *engine,
                                  output_pid_state_t *state,
                                  const packet_record_t *primary,
                                  uint64_t start_ns,
                                  uint64_t end_ns)
{
    packet_history_t *secondary = &engine->history[1];
    uint32_t recovered = 0;
    size_t i;
    bool started = false;

    if (start_ns >= RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS) {
        start_ns -= RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS;
    } else {
        start_ns = 0;
    }
    end_ns += RECOVERY_ENGINE_STREAM_GAP_TIME_MARGIN_NS;

    for (i = 0; i < secondary->count; i++) {
        size_t index = (secondary->start + i) % secondary->capacity;
        packet_record_t *candidate = &secondary->records[index];

        if (candidate->arrival_time_ns <= start_ns ||
            candidate->arrival_time_ns >= end_ns ||
            candidate->arrival_time_ns <= state->last_recovered_secondary_arrival_ns) {
            continue;
        }
        if (candidate->pid != primary->pid || candidate->is_null) {
            continue;
        }
        if (candidate->hash == primary->hash) {
            if (started) {
                break;
            }
            continue;
        }
        if (candidate->transport_error) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_TEI);
            continue;
        }
        if (candidate->discontinuity_indicator || !candidate->has_payload) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_DISCONTINUITY);
            continue;
        }
        if (!record_fits_next_output(engine, candidate)) {
            if (started) {
                break;
            }
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
            continue;
        }

        started = true;
        recovered++;
        if (recovered > RECOVERY_ENGINE_MAX_PID_GAP_RECOVERY_PACKETS) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_BURST_TOO_LARGE);
            return 0;
        }
        if (output_record(engine, candidate) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, false);
        state->last_recovered_secondary_arrival_ns = candidate->arrival_time_ns;
    }

    if (recovered > 1) {
        engine->stats->recovered_content_bursts++;
    }

    return (int)recovered;
}

static int recover_pid_gap_before_primary(recovery_engine_t *engine,
                                          const packet_record_t *primary)
{
    output_pid_state_t *state;
    uint64_t primary_gap_ns;
    int64_t offset_ns;
    int64_t offset_magnitude_ns;
    int recovered;

    if (primary->is_null || primary->transport_error ||
        primary->discontinuity_indicator || !primary->has_payload) {
        return 0;
    }

    state = &engine->output_pid_state[primary->pid];
    if (!state->valid || primary->arrival_time_ns <= state->last_arrival_time_ns) {
        return 0;
    }
    if (primary->stream_continuity_errors <= state->last_primary_continuity_errors) {
        return 0;
    }

    primary_gap_ns = primary->arrival_time_ns - state->last_arrival_time_ns;
    if (primary_gap_ns < RECOVERY_ENGINE_PID_GAP_MIN_NS) {
        return 0;
    }

    offset_ns = estimated_secondary_time_offset_ns(engine);
    offset_magnitude_ns = offset_ns < 0 ? -offset_ns : offset_ns;
    recovered = recover_pid_time_range(engine, state, primary,
                                       apply_time_offset(state->last_arrival_time_ns, offset_magnitude_ns),
                                       apply_time_offset(primary->arrival_time_ns, offset_magnitude_ns));
    if (recovered == 0 && offset_magnitude_ns != 0) {
        recovered = recover_pid_time_range(engine, state, primary,
                                           apply_time_offset(state->last_arrival_time_ns, -offset_magnitude_ns),
                                           apply_time_offset(primary->arrival_time_ns, -offset_magnitude_ns));
    }
    if (recovered == 0 && offset_magnitude_ns != 0) {
        recovered = recover_pid_time_range(engine, state, primary,
                                           state->last_arrival_time_ns,
                                           primary->arrival_time_ns);
    }

    return recovered < 0 ? -1 : 0;
}

static int recover_content_burst_before_primary(recovery_engine_t *engine, const packet_record_t *primary,
                                                const packet_record_t *secondary_match)
{
    uint8_t expected_counter;
    uint8_t missing_count;
    uint64_t index;
    packet_record_t *candidates[256];
    uint8_t found = 0;
    uint8_t next_counter;

    if (!continuity_gap_for_record(engine, primary, &expected_counter, &missing_count)) {
        return 0;
    }
    if (missing_count > engine->config.max_content_burst_packets || missing_count > 255U) {
        report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_BURST_TOO_LARGE);
        return 0;
    }

    engine->stats->recovery_exact_cc_gap++;

    if (engine->alignment.confidence < engine->config.min_alignment_confidence) {
        report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_LOW_ALIGNMENT);
        return 0;
    }
    if (!engine->last_primary_anchor.valid || secondary_match == NULL ||
        secondary_match->stream_index <= engine->last_primary_anchor.secondary_index) {
        return recover_content_burst_by_counter_before_primary(engine, primary,
                                                               expected_counter, missing_count);
    }

    next_counter = expected_counter;
    for (index = engine->last_primary_anchor.secondary_index + 1ULL; index < secondary_match->stream_index; index++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1], index);

        if (candidate == NULL) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_MISSING_CANDIDATE);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        if (candidate->transport_error) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_TEI);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        if (candidate->discontinuity_indicator) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_DISCONTINUITY);
            engine->stats->unrecoverable_loss++;
            return 0;
        }

        if (candidate->is_null) {
            continue;
        }

        if (candidate->pid != primary->pid || !candidate->has_payload) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_PID);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        if (candidate->continuity_counter != next_counter) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
            engine->stats->unrecoverable_loss++;
            return 0;
        }
        if (found >= missing_count) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_AMBIGUOUS);
            engine->stats->unrecoverable_loss++;
            return 0;
        }

        candidates[found++] = candidate;
        next_counter = (uint8_t)((next_counter + 1U) & 0x0fU);
    }

    if (found != missing_count) {
        report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_MISSING_CANDIDATE);
        engine->stats->unrecoverable_loss++;
        return 0;
    }

    for (index = 0; index < found; index++) {
        if (output_record(engine, candidates[index]) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, false);
    }

    if (found > 1) {
        engine->stats->recovered_content_bursts++;
    }

    return 0;
}

static packet_record_t *find_secondary_counter_candidate(recovery_engine_t *engine,
                                                         const packet_record_t *primary,
                                                         uint8_t continuity_counter,
                                                         packet_record_t **chosen,
                                                         uint8_t chosen_count)
{
    packet_history_t *secondary = &engine->history[1];
    packet_record_t *best = NULL;
    uint64_t best_distance = 0;
    size_t age;

    for (age = 0; age < secondary->count; age++) {
        packet_record_t *candidate = packet_history_get_newest(secondary, age);
        uint64_t distance;
        uint8_t i;
        bool already_chosen = false;

        if (candidate == NULL) {
            break;
        }
        if (engine->config.max_secondary_latency_ns > 0) {
            if (candidate->arrival_time_ns > primary->arrival_time_ns + engine->config.max_secondary_latency_ns) {
                continue;
            }
            if (candidate->arrival_time_ns + engine->config.max_secondary_latency_ns < primary->arrival_time_ns) {
                break;
            }
        }
        if (candidate->pid != primary->pid || candidate->continuity_counter != continuity_counter ||
            !candidate->has_payload || candidate->is_null || candidate->transport_error ||
            candidate->discontinuity_indicator) {
            continue;
        }

        for (i = 0; i < chosen_count; i++) {
            if (chosen[i] == candidate) {
                already_chosen = true;
                break;
            }
        }
        if (already_chosen) {
            continue;
        }

        distance = time_difference_ns(primary->arrival_time_ns, candidate->arrival_time_ns);
        if (best == NULL || distance < best_distance) {
            best = candidate;
            best_distance = distance;
        }
    }

    return best;
}

static int recover_content_burst_by_counter_before_primary(recovery_engine_t *engine,
                                                           const packet_record_t *primary,
                                                           uint8_t expected_counter,
                                                           uint8_t missing_count)
{
    packet_record_t *candidates[256];
    uint8_t found = 0;
    uint8_t next_counter = expected_counter;
    uint8_t i;

    while (found < missing_count) {
        packet_record_t *candidate =
            find_secondary_counter_candidate(engine, primary, next_counter, candidates, found);

        if (candidate == NULL) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_MISSING_CANDIDATE);
            engine->stats->unrecoverable_loss++;
            return 0;
        }

        candidates[found++] = candidate;
        next_counter = (uint8_t)((next_counter + 1U) & 0x0fU);
    }

    for (i = 0; i < found; i++) {
        if (output_record(engine, candidates[i]) != 0) {
            return -1;
        }
        report_stats_observe_recovery(engine->stats, false);
    }

    if (found > 1) {
        engine->stats->recovered_content_bursts++;
    }

    return 0;
}

static void diagnose_secondary_loss_before_primary(recovery_engine_t *engine, const packet_record_t *primary,
                                                   const packet_record_t *secondary_match)
{
    uint64_t primary_between;
    uint64_t secondary_between;

    if (!record_is_informative(primary) || !engine->last_primary_anchor.valid) {
        return;
    }

    if (secondary_match == NULL) {
        if (engine->alignment.confidence >= engine->config.min_alignment_confidence) {
            engine->stats->secondary_missing_anchors++;
        }
        return;
    }

    if (secondary_match->stream_index <= engine->last_primary_anchor.secondary_index ||
        primary->stream_index <= engine->last_primary_anchor.primary_index) {
        return;
    }

    primary_between = primary->stream_index - engine->last_primary_anchor.primary_index - 1ULL;
    secondary_between = secondary_match->stream_index - engine->last_primary_anchor.secondary_index - 1ULL;
    if (primary_between > secondary_between) {
        engine->stats->secondary_loss_events++;
        engine->stats->secondary_missing_packets += primary_between - secondary_between;
    }
}

static void update_alignment(recovery_engine_t *engine, int stream_id, const packet_record_t *record)
{
    packet_history_t *other_history;
    packet_record_t *best = NULL;
    int best_score = 0;
    int64_t delta;
    size_t limit;
    size_t age;

    if (!record_is_informative(record)) {
        return;
    }

    other_history = &engine->history[stream_id == 0 ? 1 : 0];
    for (delta = -ALIGNMENT_STREAM_INDEX_SEARCH_PACKETS;
         delta <= ALIGNMENT_STREAM_INDEX_SEARCH_PACKETS; delta++) {
        int64_t candidate_index = (int64_t)record->stream_index + delta;
        packet_record_t *candidate;
        int score;

        if (candidate_index < 0) {
            continue;
        }
        candidate = packet_history_find_index(other_history, (uint64_t)candidate_index);
        if (candidate == NULL || !records_within_alignment_window(engine, record, candidate)) {
            continue;
        }
        score = score_alignment_match(record, candidate);
        if (score > best_score) {
            best_score = score;
            best = candidate;
        }
    }

    if (best_score < RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD) {
        int64_t offset_packets;

        if (estimated_alignment_offset_packets(engine, &offset_packets)) {
            int64_t expected_index = stream_id == 0
                                         ? (int64_t)record->stream_index + offset_packets
                                         : (int64_t)record->stream_index - offset_packets;

            for (delta = -ALIGNMENT_PCR_INDEX_SEARCH_PACKETS;
                 delta <= ALIGNMENT_PCR_INDEX_SEARCH_PACKETS; delta++) {
                int64_t candidate_index = expected_index + delta;
                packet_record_t *candidate;
                int score;

                if (candidate_index < 0) {
                    continue;
                }
                candidate = packet_history_find_index(other_history, (uint64_t)candidate_index);
                if (candidate == NULL || !records_within_alignment_window(engine, record, candidate)) {
                    continue;
                }
                score = score_alignment_match(record, candidate);
                if (score > best_score) {
                    best_score = score;
                    best = candidate;
                }
            }
        }
    }

    limit = other_history->count < RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS
                ? other_history->count
                : RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS;

    for (age = 0; age < limit; age++) {
        packet_record_t *candidate = packet_history_get_newest(other_history, age);
        int score;

        if (candidate == NULL) {
            break;
        }

        if (!records_within_alignment_window(engine, record, candidate)) {
            continue;
        }

        score = score_alignment_match(record, candidate);
        if (score > best_score) {
            best_score = score;
            best = candidate;
        }
    }

    if (best != NULL && best_score >= RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD) {
        int64_t offset;

        if (stream_id == 0) {
            offset = (int64_t)best->stream_index - (int64_t)record->stream_index;
        } else {
            offset = (int64_t)record->stream_index - (int64_t)best->stream_index;
        }

        if (engine->alignment.has_alignment &&
            llabs(offset - engine->alignment.offset_packets) > 4) {
            engine->stats->stream_disagreements++;
            if (stream_id == 0 &&
                (record->stream_continuity_errors > engine->primary_gap.continuity_errors ||
                 record->stream_duplicate_counters > engine->primary_gap.duplicate_counters)) {
                engine->alignment.consecutive_misses++;
                engine->stats->alignment_offset_packets = engine->alignment.offset_packets;
                engine->stats->alignment_confidence = engine->alignment.confidence;
                return;
            }
        } else if (engine->alignment.confidence < 100) {
            engine->alignment.confidence += 4;
            if (engine->alignment.confidence > 100) {
                engine->alignment.confidence = 100;
            }
        }

        engine->alignment.has_alignment = true;
        engine->alignment.offset_packets = offset;
        engine->alignment.consecutive_matches++;
        engine->alignment.consecutive_misses = 0;
        if (stream_id == 0) {
            report_stats_observe_latency(engine->stats, record->arrival_time_ns,
                                         best->arrival_time_ns,
                                         engine->config.max_secondary_latency_ns);
        } else {
            report_stats_observe_latency(engine->stats, best->arrival_time_ns,
                                         record->arrival_time_ns,
                                         engine->config.max_secondary_latency_ns);
        }
    } else {
        engine->alignment.consecutive_misses++;
        if (engine->alignment.consecutive_misses >= 64 && engine->alignment.confidence > 0) {
            engine->alignment.confidence--;
            engine->alignment.consecutive_misses = 0;
        }
    }

    engine->stats->alignment_offset_packets = engine->alignment.offset_packets;
    engine->stats->alignment_confidence = engine->alignment.confidence;
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

static size_t history_capacity_from_config(const recovery_engine_config_t *config)
{
    uint64_t capacity = RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS;

    if (config->history_ms > 0) {
        capacity = (RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS * config->history_ms) /
                   RECOVERY_ENGINE_DEFAULT_HISTORY_MS;
    }
    if (capacity < 8192ULL) {
        capacity = 8192ULL;
    }
    if (capacity > 1048576ULL) {
        capacity = 1048576ULL;
    }

    return (size_t)capacity;
}

int recovery_engine_init(recovery_engine_t *engine, packet_sink_t *sink, report_stats_t *stats)
{
    recovery_engine_config_t config = recovery_engine_default_config();

    return recovery_engine_init_with_config(engine, sink, stats, &config);
}

int recovery_engine_init_with_config(recovery_engine_t *engine, packet_sink_t *sink, report_stats_t *stats,
                                     const recovery_engine_config_t *config)
{
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

    if (packet_history_init(&engine->history[0], history_capacity_from_config(&engine->config)) != 0) {
        return -1;
    }
    if (packet_history_init(&engine->history[1], history_capacity_from_config(&engine->config)) != 0) {
        packet_history_free(&engine->history[0]);
        return -1;
    }
    if (primary_delay_queue_init(&engine->primary_queue, history_capacity_from_config(&engine->config),
                                 engine->config.primary_delay_ns) != 0) {
        packet_history_free(&engine->history[1]);
        packet_history_free(&engine->history[0]);
        return -1;
    }
    if (primary_delay_queue_init(&engine->secondary_queue, history_capacity_from_config(&engine->config),
                                 engine->config.primary_delay_ns) != 0) {
        primary_delay_queue_free(&engine->primary_queue);
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

    if (stream_id < 0 || stream_id > 1) {
        return -1;
    }

    record = packet_history_push(&engine->history[stream_id]);
    record->source_stream_id = stream_id;
    record->stream_index = engine->next_stream_index[stream_id]++;
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
    record->pcr_value = info->has_pcr ? packet_info_pcr_value(info) : 0;
    record->hash = hash_packet(packet);
    memcpy(record->packet, packet, TS_PACKET_SIZE);
    engine->last_input_arrival_ns[stream_id] = record->arrival_time_ns;

    update_pcr_timing(engine, stream_id, record);
    update_alignment(engine, stream_id, record);

    if (stream_id == 0 && primary_delay_queue_push(&engine->primary_queue, record) != 0) {
        packet_record_t *oldest = primary_delay_queue_front(&engine->primary_queue);
        if (oldest == NULL) {
            return -1;
        }
        engine->stats->primary_delay_overflows++;
        if (engine->active_output_stream_id == 0 && output_record(engine, oldest) != 0) {
            return -1;
        }
        primary_delay_queue_pop(&engine->primary_queue);
        if (primary_delay_queue_push(&engine->primary_queue, record) != 0) {
            return -1;
        }
    }
    if (stream_id == 1 && primary_delay_queue_push(&engine->secondary_queue, record) != 0) {
        primary_delay_queue_pop(&engine->secondary_queue);
        if (primary_delay_queue_push(&engine->secondary_queue, record) != 0) {
            return -1;
        }
    }

    return recovery_engine_drain(engine, false);
}

int recovery_engine_drain(recovery_engine_t *engine, bool force)
{
    uint64_t now_ns = report_stats_now_ns();

    maybe_update_active_source(engine, now_ns);

    if (engine->active_output_stream_id == 1) {
        prune_primary_queue_covered_by_secondary(engine);
        return drain_secondary_failover(engine, force);
    }

    if (!force && engine->primary_queue.count == 0 &&
        engine->last_input_arrival_ns[0] > 0 &&
        now_ns >= engine->last_input_arrival_ns[0] &&
        now_ns - engine->last_input_arrival_ns[0] >= RECOVERY_ENGINE_STREAM_GAP_MIN_NS &&
        stream_has_recent_input(engine, 1, now_ns, RECOVERY_ENGINE_STREAM_GAP_MIN_NS * 4ULL)) {
        if (recover_due_secondary_before_primary(engine, NULL, now_ns) < 0) {
            return -1;
        }
    }

    while (engine->primary_queue.count > 0) {
        packet_record_t *record = primary_delay_queue_front(&engine->primary_queue);
        packet_record_t *secondary_match = NULL;
        uint64_t recovered_before = engine->stats->recovered_packets;

        now_ns = report_stats_now_ns();

        if (!force && now_ns - record->arrival_time_ns < engine->primary_queue.delay_ns) {
            if ((!record_fits_next_output(engine, record) ||
                 record->stream_continuity_errors > engine->primary_gap.continuity_errors ||
                 record->stream_duplicate_counters > engine->primary_gap.duplicate_counters) &&
                recover_due_secondary_before_primary(engine, record, now_ns) < 0) {
                return -1;
            }
            break;
        }

        if (engine->primary_switchback_guard) {
            if (!record_fits_next_output(engine, record)) {
                report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
                engine->stats->unrecoverable_loss++;
                primary_delay_queue_pop(&engine->primary_queue);
                continue;
            }
        } else {
            if (recover_stream_gap_before_primary(engine, record) != 0) {
                return -1;
            }

            secondary_match = find_secondary_match_for_primary(engine, record);
            {
                int anchored_recovered =
                    recover_anchored_secondary_gap_before_primary(engine, record, secondary_match);

                if (anchored_recovered < 0) {
                    return -1;
                }
                if (anchored_recovered == 0) {
                    if (recover_nulls_before_primary(engine, record, secondary_match) != 0) {
                        return -1;
                    }
                }
            }
            if (!record_fits_next_output(engine, record)) {
                if (recover_pid_gap_before_primary(engine, record) != 0) {
                    return -1;
                }

                if (recover_content_burst_before_primary(engine, record, secondary_match) != 0) {
                    return -1;
                }
            }

            diagnose_secondary_loss_before_primary(engine, record, secondary_match);

            if (engine->stats->recovered_packets > recovered_before &&
                !record_fits_next_output(engine, record)) {
                int bridge_recovered = recover_secondary_bridge_to_primary(
                    engine, record, RECOVERY_ENGINE_MAX_STREAM_GAP_RECOVERY_PACKETS);

                if (bridge_recovered < 0) {
                    return -1;
                }
            }
        }

        if ((engine->primary_switchback_guard ||
             engine->stats->recovered_packets > recovered_before ||
             record->stream_continuity_errors > engine->primary_gap.continuity_errors ||
             record->stream_duplicate_counters > engine->primary_gap.duplicate_counters) &&
            !record_fits_next_output(engine, record)) {
            report_stats_reject_recovery(engine->stats, RECOVERY_REJECT_WRONG_COUNTER);
            engine->stats->unrecoverable_loss++;
            primary_delay_queue_pop(&engine->primary_queue);
            continue;
        }
        if (output_record(engine, record) != 0) {
            return -1;
        }
        if (engine->primary_switchback_guard && record_is_informative(record) &&
            engine->primary_switchback_guard_informative > 0) {
            engine->primary_switchback_guard_informative--;
            if (engine->primary_switchback_guard_informative == 0) {
                engine->primary_switchback_guard = false;
            }
        }
        engine->primary_gap.valid = true;
        engine->primary_gap.arrival_time_ns = record->arrival_time_ns;
        engine->primary_gap.continuity_errors = record->stream_continuity_errors;
        engine->primary_gap.duplicate_counters = record->stream_duplicate_counters;
        engine->primary_gap.recovered_stream_packets = 0;
        engine->primary_gap.last_recovered_secondary_arrival_ns = 0;
        engine->primary_gap.secondary_recovery_index = 0;
        if (secondary_match != NULL) {
            engine->last_primary_anchor.valid = true;
            engine->last_primary_anchor.primary_index = record->stream_index;
            engine->last_primary_anchor.secondary_index = secondary_match->stream_index;
            prune_secondary_queue_through(engine, secondary_match->stream_index);
        }
        primary_delay_queue_pop(&engine->primary_queue);
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
    primary_delay_queue_free(&engine->secondary_queue);
    primary_delay_queue_free(&engine->primary_queue);
    packet_history_free(&engine->history[0]);
    packet_history_free(&engine->history[1]);
}
