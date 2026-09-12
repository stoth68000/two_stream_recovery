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

    if (stream_id != 0) {
        return 0;
    }

    if (output_udp_send_ts_packet(engine->output, packet) != 0) {
        return -1;
    }

    engine->stats->output_packets++;
    return 0;
}

int recovery_engine_flush(recovery_engine_t *engine)
{
    return output_udp_flush(engine->output);
}

void recovery_engine_free(recovery_engine_t *engine)
{
    packet_history_free(&engine->history[0]);
    packet_history_free(&engine->history[1]);
}
