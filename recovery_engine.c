#include "recovery_engine.h"

#include <stdlib.h>
#include <string.h>

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
    size_t i;

    for (i = 0; i < history->count; i++) {
        size_t index = (history->start + i) % history->capacity;
        if (history->records[index].stream_index == stream_index) {
            return &history->records[index];
        }
    }

    return NULL;
}

static bool record_is_informative(const packet_record_t *record)
{
    return !record->is_null && !record->transport_error;
}

static void observe_output_record(recovery_engine_t *engine, const packet_record_t *record)
{
    if (!record->is_null && !record->transport_error && !record->discontinuity_indicator) {
        engine->output_pid_state[record->pid].valid = true;
        engine->output_pid_state[record->pid].continuity_counter = record->continuity_counter;
    }
}

static int output_record(recovery_engine_t *engine, const packet_record_t *record)
{
    if (output_udp_send_ts_packet(engine->output, record->packet) != 0) {
        return -1;
    }

    engine->stats->output_packets++;
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

    if (engine->pcr_model[0].confidence > 0 && engine->pcr_model[1].confidence > 0) {
        engine->stats->pcr_delay_ns = engine->pcr_model[1].estimated_delay_ns -
                                      engine->pcr_model[0].estimated_delay_ns;
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
    }

    return score;
}

static packet_record_t *find_secondary_match_for_primary(recovery_engine_t *engine,
                                                         const packet_record_t *primary)
{
    int64_t expected;
    int64_t delta;
    packet_history_t *secondary = &engine->history[1];

    if (!record_is_informative(primary) || !engine->alignment.has_alignment ||
        engine->alignment.confidence < 20) {
        return NULL;
    }

    expected = (int64_t)primary->stream_index + engine->alignment.offset_packets;
    for (delta = -8; delta <= 8; delta++) {
        int64_t candidate_index = expected + delta;
        packet_record_t *candidate;

        if (candidate_index < 0) {
            continue;
        }

        candidate = packet_history_find_index(secondary, (uint64_t)candidate_index);
        if (candidate != NULL && score_alignment_match(primary, candidate) >=
                                     RECOVERY_ENGINE_ALIGNMENT_MATCH_THRESHOLD) {
            return candidate;
        }
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
        engine->stats->recovered_packets++;
        engine->stats->recovered_null_packets++;
    }

    return 0;
}

static int recover_single_content_before_primary(recovery_engine_t *engine, const packet_record_t *primary,
                                                 const packet_record_t *secondary_match)
{
    uint8_t expected_counter;
    uint8_t missing_count;
    uint64_t index;

    if (!engine->last_primary_anchor.valid || secondary_match == NULL || engine->alignment.confidence < 40 ||
        !continuity_gap_for_record(engine, primary, &expected_counter, &missing_count) || missing_count != 1 ||
        secondary_match->stream_index <= engine->last_primary_anchor.secondary_index) {
        return 0;
    }

    for (index = engine->last_primary_anchor.secondary_index + 1ULL; index < secondary_match->stream_index; index++) {
        packet_record_t *candidate = packet_history_find_index(&engine->history[1], index);

        if (candidate != NULL && !candidate->is_null && !candidate->transport_error &&
            !candidate->discontinuity_indicator && candidate->pid == primary->pid &&
            candidate->continuity_counter == expected_counter && candidate->has_payload) {
            if (output_record(engine, candidate) != 0) {
                return -1;
            }
            engine->stats->recovered_packets++;
            engine->stats->recovered_content_packets++;
            return 0;
        }
    }

    engine->stats->unrecoverable_loss++;
    return 0;
}

static void update_alignment(recovery_engine_t *engine, int stream_id, const packet_record_t *record)
{
    packet_history_t *other_history;
    packet_record_t *best = NULL;
    int best_score = 0;
    size_t limit;
    size_t age;

    if (!record_is_informative(record)) {
        return;
    }

    other_history = &engine->history[stream_id == 0 ? 1 : 0];
    limit = other_history->count < RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS
                ? other_history->count
                : RECOVERY_ENGINE_ALIGNMENT_SEARCH_PACKETS;

    for (age = 0; age < limit; age++) {
        packet_record_t *candidate = packet_history_get_newest(other_history, age);
        int score;

        if (candidate == NULL) {
            break;
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
            if (engine->alignment.confidence > 5) {
                engine->alignment.confidence -= 5;
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

int recovery_engine_init(recovery_engine_t *engine, output_udp_t *output, report_stats_t *stats)
{
    memset(engine, 0, sizeof(*engine));
    engine->output = output;
    engine->stats = stats;

    if (packet_history_init(&engine->history[0], RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS) != 0) {
        return -1;
    }
    if (packet_history_init(&engine->history[1], RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS) != 0) {
        packet_history_free(&engine->history[0]);
        return -1;
    }
    if (primary_delay_queue_init(&engine->primary_queue, RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS,
                                 RECOVERY_ENGINE_DEFAULT_DELAY_NS) != 0) {
        packet_history_free(&engine->history[1]);
        packet_history_free(&engine->history[0]);
        return -1;
    }

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
    record->stream_index = engine->next_stream_index[stream_id]++;
    record->arrival_time_ns = report_stats_now_ns();
    record->pid = info->pid;
    record->continuity_counter = info->continuity_counter;
    record->has_payload = info->has_payload;
    record->has_pcr = info->has_pcr;
    record->is_null = info->is_null;
    record->transport_error = info->transport_error;
    record->discontinuity_indicator = info->discontinuity_indicator;
    record->pcr_value = info->has_pcr ? packet_info_pcr_value(info) : 0;
    record->hash = hash_packet(packet);
    memcpy(record->packet, packet, TS_PACKET_SIZE);

    update_pcr_timing(engine, stream_id, record);
    update_alignment(engine, stream_id, record);

    if (stream_id == 0 && primary_delay_queue_push(&engine->primary_queue, record) != 0) {
        packet_record_t *oldest = primary_delay_queue_front(&engine->primary_queue);
        if (oldest == NULL || output_record(engine, oldest) != 0) {
            return -1;
        }
        engine->stats->primary_delay_overflows++;
        primary_delay_queue_pop(&engine->primary_queue);
        if (primary_delay_queue_push(&engine->primary_queue, record) != 0) {
            return -1;
        }
    }

    return recovery_engine_drain(engine, false);
}

int recovery_engine_drain(recovery_engine_t *engine, bool force)
{
    uint64_t now_ns = report_stats_now_ns();

    while (engine->primary_queue.count > 0) {
        packet_record_t *record = primary_delay_queue_front(&engine->primary_queue);
        packet_record_t *secondary_match;

        if (!force && now_ns - record->arrival_time_ns < engine->primary_queue.delay_ns) {
            break;
        }

        secondary_match = find_secondary_match_for_primary(engine, record);
        if (recover_nulls_before_primary(engine, record, secondary_match) != 0) {
            return -1;
        }

        if (recover_single_content_before_primary(engine, record, secondary_match) != 0) {
            return -1;
        }

        if (output_record(engine, record) != 0) {
            return -1;
        }
        if (secondary_match != NULL) {
            engine->last_primary_anchor.valid = true;
            engine->last_primary_anchor.primary_index = record->stream_index;
            engine->last_primary_anchor.secondary_index = secondary_match->stream_index;
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
    return output_udp_flush(engine->output);
}

void recovery_engine_free(recovery_engine_t *engine)
{
    primary_delay_queue_free(&engine->primary_queue);
    packet_history_free(&engine->history[0]);
    packet_history_free(&engine->history[1]);
}
