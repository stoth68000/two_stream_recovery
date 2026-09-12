#include "../recovery_engine.h"
#include "../report_stats.h"
#include "../ts_packet.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAPTURE_MAX_PACKETS 4096

typedef struct capture_sink {
    uint8_t packets[CAPTURE_MAX_PACKETS][TS_PACKET_SIZE];
    size_t packet_count;
    size_t flush_count;
} capture_sink_t;

static int capture_send_ts_packet(void *ctx, const uint8_t packet[TS_PACKET_SIZE])
{
    capture_sink_t *sink = (capture_sink_t *)ctx;

    assert(sink->packet_count < CAPTURE_MAX_PACKETS);
    memcpy(sink->packets[sink->packet_count], packet, TS_PACKET_SIZE);
    sink->packet_count++;
    return 0;
}

static int capture_flush(void *ctx)
{
    capture_sink_t *sink = (capture_sink_t *)ctx;

    sink->flush_count++;
    return 0;
}

static packet_sink_t capture_as_packet_sink(capture_sink_t *capture)
{
    packet_sink_t sink;

    sink.ctx = capture;
    sink.send_ts_packet = capture_send_ts_packet;
    sink.flush = capture_flush;
    return sink;
}

static void make_packet(uint8_t packet[TS_PACKET_SIZE], uint16_t pid, uint8_t continuity_counter,
                        uint8_t marker)
{
    size_t i;

    memset(packet, 0xff, TS_PACKET_SIZE);
    packet[0] = TS_SYNC_BYTE;
    packet[1] = (uint8_t)((pid >> 8U) & 0x1fU);
    packet[2] = (uint8_t)(pid & 0xffU);
    packet[3] = (uint8_t)(0x10U | (continuity_counter & 0x0fU));
    for (i = 4; i < TS_PACKET_SIZE; i++) {
        packet[i] = (uint8_t)(marker + i);
    }
}

static void make_null_packet(uint8_t packet[TS_PACKET_SIZE], uint8_t marker)
{
    make_packet(packet, TS_NULL_PID, 0, marker);
}

static void make_pcr_packet(uint8_t packet[TS_PACKET_SIZE], uint16_t pid, uint8_t continuity_counter,
                            uint64_t pcr_base)
{
    make_packet(packet, pid, continuity_counter, (uint8_t)pcr_base);
    packet[3] = (uint8_t)(0x30U | (continuity_counter & 0x0fU));
    packet[4] = 7;
    packet[5] = 0x10;
    packet[6] = (uint8_t)(pcr_base >> 25U);
    packet[7] = (uint8_t)(pcr_base >> 17U);
    packet[8] = (uint8_t)(pcr_base >> 9U);
    packet[9] = (uint8_t)(pcr_base >> 1U);
    packet[10] = (uint8_t)((pcr_base & 0x01U) << 7U);
    packet[11] = 0;
}

static void push_packet(recovery_engine_t *engine, report_stats_t *stats, int stream_id,
                        const uint8_t packet[TS_PACKET_SIZE])
{
    ts_packet_info_t info;

    assert(ts_packet_parse(packet, &info));
    report_stats_observe_packet(stats, stream_id, &info);
    assert(recovery_engine_push_packet(engine, stream_id, packet, &info) == 0);
}

static void push_pair(recovery_engine_t *engine, report_stats_t *stats, uint16_t pid,
                      uint8_t continuity_counter, uint8_t marker)
{
    uint8_t packet[TS_PACKET_SIZE];

    make_packet(packet, pid, continuity_counter, marker);
    push_packet(engine, stats, 0, packet);
    push_packet(engine, stats, 1, packet);
}

static void init_engine(recovery_engine_t *engine, report_stats_t *stats, capture_sink_t *capture,
                        packet_sink_t *sink)
{
    memset(capture, 0, sizeof(*capture));
    report_stats_init(stats);
    *sink = capture_as_packet_sink(capture);
    assert(recovery_engine_init(engine, sink, stats) == 0);
}

static void test_ts_packet_parse(void)
{
    uint8_t packet[TS_PACKET_SIZE];
    ts_packet_info_t info;

    make_pcr_packet(packet, 0x0100, 7, 0x12345678ULL);
    assert(ts_packet_parse(packet, &info));
    assert(info.pid == 0x0100);
    assert(info.continuity_counter == 7);
    assert(info.has_payload);
    assert(info.has_adaptation);
    assert(info.has_pcr);
    assert(info.pcr_base == 0x12345678ULL);

    make_null_packet(packet, 0x22);
    assert(ts_packet_parse(packet, &info));
    assert(info.is_null);
    assert(info.pid == TS_NULL_PID);

    packet[0] = 0x00;
    assert(!ts_packet_parse(packet, &info));
}

static void test_primary_pass_through(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t packets[3][TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 3; i++) {
        make_packet(packets[i], 0x100, (uint8_t)i, (uint8_t)(0x30 + i));
        push_packet(&engine, &stats, 0, packets[i]);
    }

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 3);
    for (i = 0; i < 3; i++) {
        assert(memcmp(capture.packets[i], packets[i], TS_PACKET_SIZE) == 0);
    }
    assert(stats.output_packets == 3);
    recovery_engine_free(&engine);
}

static void test_single_packet_recovery(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_d[TS_PACKET_SIZE];
    uint8_t secondary_c[TS_PACKET_SIZE];
    uint8_t secondary_d[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x100, (uint8_t)i, (uint8_t)(0x40 + i));
    }

    make_packet(secondary_c, 0x100, 12, 0x70);
    make_packet(primary_d, 0x100, 13, 0x71);
    make_packet(secondary_d, 0x100, 13, 0x71);
    push_packet(&engine, &stats, 1, secondary_c);
    push_packet(&engine, &stats, 0, primary_d);
    push_packet(&engine, &stats, 1, secondary_d);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 14);
    assert(memcmp(capture.packets[12], secondary_c, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[13], primary_d, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_content_packets == 1);
    assert(stats.recovered_packets == 1);
    recovery_engine_free(&engine);
}

static void test_burst_recovery(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t secondary_gap[3][TS_PACKET_SIZE];
    uint8_t primary_h[TS_PACKET_SIZE];
    uint8_t secondary_h[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x101, (uint8_t)i, (uint8_t)(0x80 + i));
    }

    for (i = 0; i < 3; i++) {
        make_packet(secondary_gap[i], 0x101, (uint8_t)(12 + i), (uint8_t)(0xa0 + i));
        push_packet(&engine, &stats, 1, secondary_gap[i]);
    }
    make_packet(primary_h, 0x101, 15, 0xb0);
    make_packet(secondary_h, 0x101, 15, 0xb0);
    push_packet(&engine, &stats, 0, primary_h);
    push_packet(&engine, &stats, 1, secondary_h);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 16);
    for (i = 0; i < 3; i++) {
        assert(memcmp(capture.packets[12 + i], secondary_gap[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[15], primary_h, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_content_packets == 3);
    assert(stats.recovered_content_bursts == 1);
    recovery_engine_free(&engine);
}

static void test_secondary_loss_diagnosis(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_extra[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x102, (uint8_t)i, (uint8_t)(0xc0 + i));
    }

    make_packet(primary_extra, 0x102, 12, 0xd0);
    push_packet(&engine, &stats, 0, primary_extra);
    push_pair(&engine, &stats, 0x102, 13, 0xd1);

    assert(recovery_engine_flush(&engine) == 0);
    assert(stats.secondary_loss_events == 1);
    assert(stats.secondary_missing_packets == 1);
    assert(capture.packet_count == 14);
    recovery_engine_free(&engine);
}

int main(void)
{
    test_ts_packet_parse();
    test_primary_pass_through();
    test_single_packet_recovery();
    test_burst_recovery();
    test_secondary_loss_diagnosis();
    printf("test_recovery: ok\n");
    return 0;
}
