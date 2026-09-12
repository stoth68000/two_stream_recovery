#include "../packet_sink.h"
#include "../recovery_engine.h"
#include "../report_stats.h"
#include "../ts_packet.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REAL_TS_PATH "1280x720p5994-avc-dwts-2xac3-20mb-5min.ts"
#define SOAK_PACKETS 8192
#define SOAK_CAPTURE_PACKETS 4096

typedef struct capture_sink {
    uint8_t packets[SOAK_CAPTURE_PACKETS][TS_PACKET_SIZE];
    size_t packet_count;
} capture_sink_t;

static int capture_send_ts_packet(void *ctx, const uint8_t packet[TS_PACKET_SIZE])
{
    capture_sink_t *capture = (capture_sink_t *)ctx;

    assert(capture->packet_count < SOAK_CAPTURE_PACKETS);
    memcpy(capture->packets[capture->packet_count++], packet, TS_PACKET_SIZE);
    return 0;
}

static packet_sink_t capture_as_sink(capture_sink_t *capture)
{
    packet_sink_t sink;

    sink.ctx = capture;
    sink.send_ts_packet = capture_send_ts_packet;
    sink.flush = NULL;
    return sink;
}

static size_t load_packets(uint8_t packets[][TS_PACKET_SIZE], size_t max_packets)
{
    FILE *file = fopen(REAL_TS_PATH, "rb");
    size_t count = 0;

    if (file == NULL) {
        return 0;
    }

    while (count < max_packets && fread(packets[count], 1, TS_PACKET_SIZE, file) == TS_PACKET_SIZE) {
        count++;
    }

    fclose(file);
    return count;
}

static void push_packet(recovery_engine_t *engine, report_stats_t *stats, int stream_id,
                        const uint8_t packet[TS_PACKET_SIZE])
{
    ts_packet_info_t info;

    assert(ts_packet_parse(packet, &info));
    report_stats_observe_packet(stats, stream_id, &info);
    assert(recovery_engine_push_packet(engine, stream_id, packet, &info) == 0);
}

static bool recoverable_content_packet(const uint8_t packet[TS_PACKET_SIZE], ts_packet_info_t *info)
{
    return ts_packet_parse(packet, info) && !info->is_null && !info->transport_error &&
           !info->discontinuity_indicator && info->has_payload;
}

static bool find_same_pid_run(uint8_t packets[][TS_PACKET_SIZE], size_t packet_count,
                              size_t run_packets, size_t *run_start)
{
    size_t i;

    for (i = 0; i + run_packets <= packet_count; i++) {
        ts_packet_info_t first;
        size_t j;

        if (!recoverable_content_packet(packets[i], &first)) {
            continue;
        }

        for (j = 1; j < run_packets; j++) {
            ts_packet_info_t current;
            uint8_t expected = (uint8_t)((first.continuity_counter + j) & 0x0fU);

            if (!recoverable_content_packet(packets[i + j], &current) ||
                current.pid != first.pid || current.continuity_counter != expected) {
                break;
            }
        }

        if (j == run_packets) {
            *run_start = i;
            return true;
        }
    }

    return false;
}

int main(void)
{
    static uint8_t packets[SOAK_PACKETS][TS_PACKET_SIZE];
    recovery_engine_t engine;
    recovery_engine_config_t config = recovery_engine_default_config();
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    size_t packet_count = load_packets(packets, SOAK_PACKETS);
    size_t run_start = 0;
    size_t i;

    if (packet_count == 0) {
        printf("soak_replay: skipping, %s not present\n", REAL_TS_PATH);
        return 0;
    }

    assert(packet_count == SOAK_PACKETS);
    assert(find_same_pid_run(packets, packet_count, 24, &run_start));
    memset(&capture, 0, sizeof(capture));
    report_stats_init(&stats);
    sink = capture_as_sink(&capture);
    config.min_alignment_confidence = 20;
    config.max_content_burst_packets = 8;
    assert(recovery_engine_init_with_config(&engine, &sink, &stats, &config) == 0);

    for (i = 0; i < 12; i++) {
        push_packet(&engine, &stats, 0, packets[run_start + i]);
        push_packet(&engine, &stats, 1, packets[run_start + i]);
    }
    assert(recovery_engine_flush(&engine) == 0);

    for (i = 12; i < 15; i++) {
        push_packet(&engine, &stats, 1, packets[run_start + i]);
    }
    for (i = 15; i < 24; i++) {
        push_packet(&engine, &stats, 0, packets[run_start + i]);
        push_packet(&engine, &stats, 1, packets[run_start + i]);
    }

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 24);
    for (i = 0; i < 24; i++) {
        assert(memcmp(capture.packets[i], packets[run_start + i], TS_PACKET_SIZE) == 0);
    }
    assert(stats.recovered_packets == 3);
    assert(stats.recovered_content_packets == 3);
    assert(stats.unrecoverable_loss == 0);
    recovery_engine_free(&engine);

    printf("soak_replay: ok run_start=%zu output=%zu recovered=%llu unrecoverable=%llu\n",
           run_start, capture.packet_count,
           (unsigned long long)stats.recovered_packets,
           (unsigned long long)stats.unrecoverable_loss);
    return 0;
}
