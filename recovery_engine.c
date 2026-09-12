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

static bool record_is_informative(const packet_record_t *record)
{
    return !record->is_null && !record->transport_error;
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
    record->hash = hash_packet(packet);
    memcpy(record->packet, packet, TS_PACKET_SIZE);

    update_alignment(engine, stream_id, record);

    if (stream_id == 0 && primary_delay_queue_push(&engine->primary_queue, record) != 0) {
        packet_record_t *oldest = primary_delay_queue_front(&engine->primary_queue);
        if (oldest == NULL || output_udp_send_ts_packet(engine->output, oldest->packet) != 0) {
            return -1;
        }
        engine->stats->output_packets++;
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

        if (!force && now_ns - record->arrival_time_ns < engine->primary_queue.delay_ns) {
            break;
        }

        if (output_udp_send_ts_packet(engine->output, record->packet) != 0) {
            return -1;
        }
        engine->stats->output_packets++;
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
