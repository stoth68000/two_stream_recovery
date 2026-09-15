#include "../command_line.h"
#include "../input_udp.h"
#include "../output_udp.h"
#include "../recovery_engine.h"
#include "../report_stats.h"
#include "../ts_packet.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define CAPTURE_MAX_PACKETS 4096
#define REAL_TS_PATH "1280x720p5994-avc-dwts-2xac3-20mb-5min.ts"
#define REAL_TS_MAX_PACKETS 8192

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

static void stamp_packet_unique(uint8_t packet[TS_PACKET_SIZE], uint16_t scenario, uint16_t sequence)
{
    packet[4] = (uint8_t)(scenario >> 8U);
    packet[5] = (uint8_t)(scenario & 0xffU);
    packet[6] = (uint8_t)(sequence >> 8U);
    packet[7] = (uint8_t)(sequence & 0xffU);
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

static void mark_transport_error(uint8_t packet[TS_PACKET_SIZE])
{
    packet[1] |= 0x80U;
}

static void mark_discontinuity(uint8_t packet[TS_PACKET_SIZE])
{
    packet[3] = (uint8_t)(0x30U | (packet[3] & 0x0fU));
    packet[4] = 1;
    packet[5] = 0x80;
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

static void init_engine_with_config(recovery_engine_t *engine, report_stats_t *stats,
                                    capture_sink_t *capture, packet_sink_t *sink,
                                    const recovery_engine_config_t *config)
{
    memset(capture, 0, sizeof(*capture));
    report_stats_init(stats);
    *sink = capture_as_packet_sink(capture);
    assert(recovery_engine_init_with_config(engine, sink, stats, config) == 0);
}

static void test_command_line_parse(void)
{
    command_line_options_t options;
    char *valid[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://239.1.1.1:5000",
        "--input-secondary-url", "udp://239.1.1.2:5001",
        "--input-primary-interface", "192.168.1.10",
        "--input-secondary-interface", "192.168.1.11",
        "--output-url", "udp://127.0.0.1:4501",
        "--primary-delay-ms", "3000",
        "--max-secondary-latency-ms", "7000",
        "--alignment-window-ms", "9000",
        "--history-ms", "12000",
        "--primary-outage-ms", "750",
        "--primary-return-ms", "180000",
        "--max-content-burst-packets", "25",
        "--min-alignment-confidence", "55",
        "--http-port", "9601",
        "--console-report"
    };
    char *defaults[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-secondary-url", "udp://127.0.0.1:5001"
    };
    char *missing_secondary[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000"
    };
    char *unknown[] = {
        "two_stream_recovery",
        "--wat", "1",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-secondary-url", "udp://127.0.0.1:5001"
    };
    char *invalid_ms[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-secondary-url", "udp://127.0.0.1:5001",
        "--primary-delay-ms", "12x"
    };
    char *invalid_confidence[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-secondary-url", "udp://127.0.0.1:5001",
        "--min-alignment-confidence", "101"
    };
    char *invalid_burst[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-secondary-url", "udp://127.0.0.1:5001",
        "--max-content-burst-packets", "300"
    };
    char *invalid_http_port[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-secondary-url", "udp://127.0.0.1:5001",
        "--http-port", "0"
    };
    char *duplicate_http_port[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-secondary-url", "udp://127.0.0.1:5001",
        "--http-port", "9601",
        "--http-port", "9602"
    };
    char *duplicate_console_report[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-secondary-url", "udp://127.0.0.1:5001",
        "--console-report",
        "--console-report"
    };
    char *duplicate_primary[] = {
        "two_stream_recovery",
        "--input-primary-url", "udp://127.0.0.1:5000",
        "--input-primary-url", "udp://127.0.0.1:5002",
        "--input-secondary-url", "udp://127.0.0.1:5001"
    };
    char *help[] = {"two_stream_recovery", "--help"};

    assert(command_line_parse((int)(sizeof(valid) / sizeof(valid[0])), valid, &options) == 0);
    assert(strcmp(options.input_primary_url, "udp://239.1.1.1:5000") == 0);
    assert(strcmp(options.input_secondary_url, "udp://239.1.1.2:5001") == 0);
    assert(strcmp(options.input_primary_interface, "192.168.1.10") == 0);
    assert(strcmp(options.input_secondary_interface, "192.168.1.11") == 0);
    assert(strcmp(options.output_url, "udp://127.0.0.1:4501") == 0);
    assert(options.recovery_config.primary_delay_ns == 3000000000ULL);
    assert(options.recovery_config.max_secondary_latency_ns == 7000000000ULL);
    assert(options.recovery_config.alignment_window_ns == 9000000000ULL);
    assert(options.recovery_config.history_ms == 12000ULL);
    assert(options.recovery_config.primary_outage_ns == 750000000ULL);
    assert(options.recovery_config.primary_return_ns == 180000000000ULL);
    assert(options.recovery_config.max_content_burst_packets == 25U);
    assert(options.recovery_config.min_alignment_confidence == 55U);
    assert(options.http_port == 9601U);
    assert(options.console_report);

    assert(command_line_parse((int)(sizeof(defaults) / sizeof(defaults[0])), defaults, &options) == 0);
    assert(strcmp(options.output_url, DEFAULT_OUTPUT_URL) == 0);
    assert(options.http_port == 0);
    assert(!options.console_report);
    assert(options.recovery_config.primary_delay_ns == RECOVERY_ENGINE_DEFAULT_PRIMARY_DELAY_NS);
    assert(options.recovery_config.max_secondary_latency_ns ==
           RECOVERY_ENGINE_DEFAULT_MAX_SECONDARY_LATENCY_NS);
    assert(options.recovery_config.alignment_window_ns == RECOVERY_ENGINE_DEFAULT_ALIGNMENT_WINDOW_NS);
    assert(options.recovery_config.history_ms == RECOVERY_ENGINE_DEFAULT_HISTORY_MS);
    assert(options.recovery_config.primary_outage_ns == RECOVERY_ENGINE_DEFAULT_PRIMARY_OUTAGE_NS);
    assert(options.recovery_config.primary_return_ns == RECOVERY_ENGINE_DEFAULT_PRIMARY_RETURN_NS);
    assert(options.recovery_config.max_content_burst_packets ==
           RECOVERY_ENGINE_DEFAULT_MAX_CONTENT_BURST_PACKETS);
    assert(options.recovery_config.min_alignment_confidence ==
           RECOVERY_ENGINE_DEFAULT_MIN_ALIGNMENT_CONFIDENCE);

    assert(command_line_parse_silent((int)(sizeof(missing_secondary) / sizeof(missing_secondary[0])),
                                     missing_secondary, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(unknown) / sizeof(unknown[0])), unknown, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(invalid_ms) / sizeof(invalid_ms[0])), invalid_ms, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(invalid_confidence) / sizeof(invalid_confidence[0])),
                                     invalid_confidence, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(invalid_burst) / sizeof(invalid_burst[0])),
                                     invalid_burst, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(invalid_http_port) / sizeof(invalid_http_port[0])),
                                     invalid_http_port, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(duplicate_http_port) / sizeof(duplicate_http_port[0])),
                                     duplicate_http_port, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(duplicate_console_report) / sizeof(duplicate_console_report[0])),
                                     duplicate_console_report, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(duplicate_primary) / sizeof(duplicate_primary[0])),
                                     duplicate_primary, &options) < 0);
    assert(command_line_parse_silent((int)(sizeof(help) / sizeof(help[0])), help, &options) > 0);
}

static void test_udp_url_parsing(void)
{
    char host[128];
    uint16_t port;

    assert(input_udp_parse_url("udp://239.1.2.3:5000", host, sizeof(host), &port) == 0);
    assert(strcmp(host, "239.1.2.3") == 0);
    assert(port == 5000);
    assert(input_udp_is_multicast_host(host));

    assert(input_udp_parse_url("127.0.0.1:6000", host, sizeof(host), &port) == 0);
    assert(strcmp(host, "127.0.0.1") == 0);
    assert(port == 6000);
    assert(!input_udp_is_multicast_host(host));

    assert(input_udp_parse_url("udp://*:7000", host, sizeof(host), &port) == 0);
    assert(strcmp(host, "*") == 0);
    assert(port == 7000);

    assert(output_udp_parse_url("udp://127.0.0.1:4500", host, sizeof(host), &port) == 0);
    assert(strcmp(host, "127.0.0.1") == 0);
    assert(port == 4500);

    assert(input_udp_is_multicast_host("224.0.0.0"));
    assert(input_udp_is_multicast_host("239.255.255.255"));
    assert(!input_udp_is_multicast_host("223.255.255.255"));
    assert(!input_udp_is_multicast_host("240.0.0.0"));
    assert(!input_udp_is_multicast_host("not-an-ip"));

    assert(input_udp_parse_url("udp://127.0.0.1", host, sizeof(host), &port) != 0);
    assert(input_udp_parse_url("udp://127.0.0.1:65536", host, sizeof(host), &port) != 0);
    assert(input_udp_parse_url("udp://127.0.0.1:abc", host, sizeof(host), &port) != 0);
    assert(output_udp_parse_url("udp://:4500", host, sizeof(host), &port) != 0);
    assert(output_udp_parse_url("udp://127.0.0.1:", host, sizeof(host), &port) != 0);
    assert(output_udp_parse_url("udp://127.0.0.1:4500", host, 4, &port) != 0);
}

static void test_output_udp_pending_batch(void)
{
    output_udp_t output;
    uint8_t packet[TS_PACKET_SIZE];
    size_t i;

    memset(&output, 0, sizeof(output));
    output.fd = -1;
    make_packet(packet, 0x100, 1, 0x66);

    for (i = 0; i < OUTPUT_TS_PACKETS_PER_DATAGRAM - 1; i++) {
        assert(output_udp_send_ts_packet(&output, packet) == 0);
        assert(output.pending_packets == i + 1);
    }
    assert(output_udp_flush(&output) == 0);
    assert(output.pending_packets == 0);

    output_udp_close(&output);
    assert(output.fd == -1);
}

static void test_packet_sink_helpers(void)
{
    capture_sink_t capture;
    packet_sink_t sink;
    packet_sink_t no_flush_sink;
    uint8_t packet[TS_PACKET_SIZE];

    memset(&capture, 0, sizeof(capture));
    sink = capture_as_packet_sink(&capture);
    make_packet(packet, 0x100, 1, 0x20);
    assert(packet_sink_send_ts_packet(&sink, packet) == 0);
    assert(packet_sink_flush(&sink) == 0);
    assert(capture.packet_count == 1);
    assert(capture.flush_count == 1);
    assert(memcmp(capture.packets[0], packet, TS_PACKET_SIZE) == 0);

    no_flush_sink = sink;
    no_flush_sink.flush = NULL;
    assert(packet_sink_flush(&no_flush_sink) == 0);
}

static void test_recovery_engine_config(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    recovery_engine_config_t config = recovery_engine_default_config();

    assert(config.primary_delay_ns == RECOVERY_ENGINE_DEFAULT_PRIMARY_DELAY_NS);
    assert(config.max_secondary_latency_ns == RECOVERY_ENGINE_DEFAULT_MAX_SECONDARY_LATENCY_NS);
    assert(config.alignment_window_ns == RECOVERY_ENGINE_DEFAULT_ALIGNMENT_WINDOW_NS);
    assert(config.history_ms == 10000ULL);

    config.primary_delay_ns = 123000000ULL;
    config.max_secondary_latency_ns = 456000000ULL;
    config.alignment_window_ns = 789000000ULL;
    config.history_ms = 12000ULL;
    config.max_content_burst_packets = 255U;
    config.min_alignment_confidence = 101U;

    memset(&capture, 0, sizeof(capture));
    report_stats_init(&stats);
    sink = capture_as_packet_sink(&capture);
    assert(recovery_engine_init_with_config(&engine, &sink, &stats, &config) == 0);
    assert(engine.config.primary_delay_ns == 123000000ULL);
    assert(engine.config.max_secondary_latency_ns == 456000000ULL);
    assert(engine.config.alignment_window_ns == 789000000ULL);
    assert(engine.config.history_ms == 12000ULL);
    assert(engine.config.max_content_burst_packets == 255U);
    assert(engine.config.min_alignment_confidence == 100U);
    assert(engine.history[0].capacity > RECOVERY_ENGINE_DEFAULT_HISTORY_PACKETS);
    recovery_engine_free(&engine);
}

static void test_packet_metadata_history_retains_configured_time_window(void)
{
    recovery_engine_config_t config = recovery_engine_default_config();
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    packet_record_t *first;
    packet_record_t *second;
    uint64_t first_hash;
    uint8_t first_packet[TS_PACKET_SIZE];
    uint8_t second_packet[TS_PACKET_SIZE];

    config.history_ms = 1ULL;
    init_engine_with_config(&engine, &stats, &capture, &sink, &config);

    make_packet(first_packet, 0x120, 0, 0x11);
    push_packet(&engine, &stats, 0, first_packet);
    assert(engine.history[0].count == 1);
    first = &engine.history[0].records[engine.history[0].start];
    assert(first->stream_index == 0);
    assert(first->arrival_time.tv_sec != 0 || first->arrival_time.tv_usec != 0);
    assert(first->arrival_time_ns != 0);
    assert(memcmp(first->packet, first_packet, TS_PACKET_SIZE) == 0);
    first_hash = first->hash;
    first->arrival_time_ns = 0;

    make_packet(second_packet, 0x120, 1, 0x11);
    second_packet[TS_PACKET_SIZE - 1] ^= 0x01U;
    push_packet(&engine, &stats, 0, second_packet);

    assert(engine.history[0].count == 1);
    second = &engine.history[0].records[engine.history[0].start];
    assert(second->stream_index == 1);
    assert(second->hash != first_hash);
    assert(memcmp(second->packet, second_packet, TS_PACKET_SIZE) == 0);

    recovery_engine_free(&engine);
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

static void test_ts_packet_parse_edges(void)
{
    uint8_t packet[TS_PACKET_SIZE];
    ts_packet_info_t info;

    make_packet(packet, 0x120, 4, 0x55);
    packet[3] = 0x00;
    assert(!ts_packet_parse(packet, &info));

    make_packet(packet, 0x120, 4, 0x55);
    packet[3] = 0x20 | 4;
    packet[4] = 184;
    assert(!ts_packet_parse(packet, &info));

    make_packet(packet, 0x120, 4, 0x55);
    mark_transport_error(packet);
    assert(ts_packet_parse(packet, &info));
    assert(info.transport_error);

    make_packet(packet, 0x120, 4, 0x55);
    mark_discontinuity(packet);
    assert(ts_packet_parse(packet, &info));
    assert(info.discontinuity_indicator);
    assert(info.has_adaptation);
    assert(info.has_payload);
}

static void test_report_stats_observe_edges(void)
{
    report_stats_t stats;
    char json[8192];
    uint8_t packet[TS_PACKET_SIZE];
    ts_packet_info_t info;

    report_stats_init(&stats);
    assert(report_stats_format_json(&stats, json, sizeof(json)) > 0);
    assert(strstr(json, "\"last_recovery_event\":\"Never\"") != NULL);

    make_packet(packet, 0x120, 0, 0x40);
    assert(ts_packet_parse(packet, &info));
    report_stats_observe_packet(&stats, 0, &info);

    make_packet(packet, 0x120, 0, 0x41);
    assert(ts_packet_parse(packet, &info));
    report_stats_observe_packet(&stats, 0, &info);
    assert(stats.duplicate_counters[0] == 1);
    assert(stats.continuity_errors[0] == 0);

    make_packet(packet, 0x120, 3, 0x42);
    assert(ts_packet_parse(packet, &info));
    report_stats_observe_packet(&stats, 0, &info);
    assert(stats.continuity_errors[0] == 1);

    make_null_packet(packet, 0x43);
    assert(ts_packet_parse(packet, &info));
    report_stats_observe_packet(&stats, 1, &info);
    assert(stats.null_packets[1] == 1);

    make_pcr_packet(packet, 0x100, 1, 0x1234);
    assert(ts_packet_parse(packet, &info));
    report_stats_observe_packet(&stats, 1, &info);
    assert(stats.pcr_packets[1] == 1);

    mark_transport_error(packet);
    assert(ts_packet_parse(packet, &info));
    report_stats_observe_packet(&stats, 1, &info);
    assert(stats.transport_errors[1] == 1);

    report_stats_observe_pcr_delay(&stats, -2000000000.0);
    assert(stats.pcr_delay_samples == 1);
    assert(stats.pcr_delay_ns == -2000000000.0);
    assert(stats.observed_secondary_latency_samples == 1);
    assert(stats.observed_secondary_latency_min_ns == 2000000000.0);
    assert(stats.observed_secondary_latency_avg_ns == 2000000000.0);
    assert(stats.observed_secondary_latency_max_ns == 2000000000.0);

    report_stats_observe_recovery(&stats, false);
    assert(stats.recovered_packets == 1);
    assert(stats.recovered_content_packets == 1);
    assert(stats.recovered_null_packets == 0);
    assert(stats.last_recovery_event_time != 0);
    assert(report_stats_format_json(&stats, json, sizeof(json)) > 0);
    assert(strstr(json, "\"last_recovery_event\":\"Never\"") == NULL);

    report_stats_observe_recovery(&stats, true);
    assert(stats.recovered_packets == 2);
    assert(stats.recovered_content_packets == 1);
    assert(stats.recovered_null_packets == 1);
}

static void test_report_stats_health_recovers_after_quiet_window(void)
{
    report_stats_t stats;
    char json[8192];
    size_t i;

    report_stats_init(&stats);
    stats.packets_received[0] = 1000;
    stats.packets_received[1] = 1000;
    stats.output_packets = 1000;
    stats.recovered_packets = 1;
    assert(report_stats_format_json(&stats, json, sizeof(json)) > 0);
    assert(strstr(json, "\"health\":[\"degraded\",\"healthy\"]") != NULL);

    for (i = 0; i < REPORT_STATS_ROLLING_SECONDS; i++) {
        stats.rolling_samples[i].packets_received[0] = stats.packets_received[0];
        stats.rolling_samples[i].packets_received[1] = stats.packets_received[1];
        stats.rolling_samples[i].output_packets = stats.output_packets;
        stats.rolling_samples[i].recovered_packets = stats.recovered_packets;
        stats.rolling_samples[i].unrecoverable_loss = stats.unrecoverable_loss;
    }
    stats.rolling_count = REPORT_STATS_ROLLING_SECONDS;
    stats.rolling_index = 0;
    stats.packets_received[0] += 1000;
    stats.packets_received[1] += 1000;
    stats.output_packets += 1000;

    assert(report_stats_format_json(&stats, json, sizeof(json)) > 0);
    assert(strstr(json, "\"health\":[\"healthy\",\"healthy\"]") != NULL);
    assert(strstr(json, "\"output_health\":\"healthy\"") != NULL);
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

static void test_clean_dual_input_baseline_no_recovery(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t packets[32][TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 32; i++) {
        make_packet(packets[i], 0x100, (uint8_t)(i & 0x0fU), (uint8_t)(0x50 + i));
        push_packet(&engine, &stats, 0, packets[i]);
        push_packet(&engine, &stats, 1, packets[i]);
    }

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 32);
    for (i = 0; i < 32; i++) {
        assert(memcmp(capture.packets[i], packets[i], TS_PACKET_SIZE) == 0);
    }
    assert(stats.packets_received[0] == 32);
    assert(stats.packets_received[1] == 32);
    assert(stats.output_packets == 32);
    assert(stats.output_continuity_errors == 0);
    assert(stats.output_duplicate_counters == 0);
    assert(stats.recovered_packets == 0);
    assert(stats.recovered_content_packets == 0);
    assert(stats.recovered_null_packets == 0);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.active_output_stream_id == 0);
    assert(stats.source_switches[0] == 0);
    assert(stats.source_switches[1] == 0);

    recovery_engine_free(&engine);
}

static void test_exact_single_packet_content_recovery(void)
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
    assert(stats.recovered_null_packets == 0);
    assert(stats.recovered_packets == 1);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.output_continuity_errors == 0);
    recovery_engine_free(&engine);
}

static void test_exact_single_packet_recovery_after_stale_global_anchor(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_interloper[TS_PACKET_SIZE];
    uint8_t secondary_interloper[TS_PACKET_SIZE];
    uint8_t secondary_c[TS_PACKET_SIZE];
    uint8_t primary_d[TS_PACKET_SIZE];
    uint8_t secondary_d[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x032, (uint8_t)i, (uint8_t)(0x40 + i));
    }

    make_packet(primary_interloper, 0x200, 0, 0x90);
    make_packet(secondary_interloper, 0x200, 0, 0xa0);
    make_packet(secondary_c, 0x032, 12, 0x70);
    make_packet(primary_d, 0x032, 13, 0x71);
    make_packet(secondary_d, 0x032, 13, 0x71);

    push_packet(&engine, &stats, 0, primary_interloper);
    push_packet(&engine, &stats, 1, secondary_interloper);
    push_packet(&engine, &stats, 1, secondary_c);
    push_packet(&engine, &stats, 0, primary_d);
    push_packet(&engine, &stats, 1, secondary_d);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 15);
    assert(memcmp(capture.packets[12], primary_interloper, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[13], secondary_c, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[14], primary_d, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_content_packets == 1);
    assert(stats.recovered_packets == 1);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.output_continuity_errors == 0);

    recovery_engine_free(&engine);
}

static void test_null_match_does_not_replace_recovery_anchor(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t null_packet[TS_PACKET_SIZE];
    uint8_t secondary_c[TS_PACKET_SIZE];
    uint8_t primary_d[TS_PACKET_SIZE];
    uint8_t secondary_d[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x033, (uint8_t)i, (uint8_t)(0x50 + i));
    }

    make_null_packet(null_packet, 0x70);
    make_packet(secondary_c, 0x033, 12, 0x71);
    make_packet(primary_d, 0x033, 13, 0x72);
    make_packet(secondary_d, 0x033, 13, 0x72);

    push_packet(&engine, &stats, 0, null_packet);
    push_packet(&engine, &stats, 1, null_packet);
    push_packet(&engine, &stats, 1, secondary_c);
    push_packet(&engine, &stats, 0, primary_d);
    push_packet(&engine, &stats, 1, secondary_d);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 15);
    assert(memcmp(capture.packets[12], null_packet, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[13], secondary_c, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[14], primary_d, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_content_packets == 1);
    assert(stats.output_continuity_errors == 0);
    assert(stats.recovery_rejects[RECOVERY_REJECT_AMBIGUOUS] == 0);

    recovery_engine_free(&engine);
}

static void test_primary_tei_packet_replaced_from_clean_secondary(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_c[TS_PACKET_SIZE];
    uint8_t secondary_c[TS_PACKET_SIZE];
    uint8_t primary_d[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x120, (uint8_t)i, (uint8_t)(0x50 + i));
    }

    make_packet(primary_c, 0x120, 12, 0x70);
    make_packet(secondary_c, 0x120, 12, 0x70);
    mark_transport_error(primary_c);
    make_packet(primary_d, 0x120, 13, 0x71);

    push_packet(&engine, &stats, 1, secondary_c);
    push_packet(&engine, &stats, 0, primary_c);
    push_packet(&engine, &stats, 1, primary_d);
    push_packet(&engine, &stats, 0, primary_d);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 14);
    assert(memcmp(capture.packets[12], secondary_c, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[12], primary_c, TS_PACKET_SIZE) != 0);
    assert(memcmp(capture.packets[13], primary_d, TS_PACKET_SIZE) == 0);
    assert(stats.transport_errors[0] == 1);
    assert(stats.recovered_content_packets == 1);
    assert(stats.recovered_null_packets == 0);
    assert(stats.recovered_packets == 1);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.output_continuity_errors == 0);

    recovery_engine_free(&engine);
}

static void test_primary_tei_packet_not_replaced_from_tei_secondary(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_c[TS_PACKET_SIZE];
    uint8_t secondary_c[TS_PACKET_SIZE];
    uint8_t primary_d[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x121, (uint8_t)i, (uint8_t)(0x60 + i));
    }

    make_packet(primary_c, 0x121, 12, 0x80);
    make_packet(secondary_c, 0x121, 12, 0x80);
    mark_transport_error(primary_c);
    mark_transport_error(secondary_c);
    make_packet(primary_d, 0x121, 13, 0x81);

    push_packet(&engine, &stats, 1, secondary_c);
    push_packet(&engine, &stats, 0, primary_c);
    push_packet(&engine, &stats, 1, primary_d);
    push_packet(&engine, &stats, 0, primary_d);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 14);
    assert(memcmp(capture.packets[12], primary_c, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[12], secondary_c, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[13], primary_d, TS_PACKET_SIZE) == 0);
    assert(stats.transport_errors[0] == 1);
    assert(stats.transport_errors[1] == 1);
    assert(stats.recovered_content_packets == 0);
    assert(stats.recovered_packets == 0);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.recovery_rejects[RECOVERY_REJECT_TEI] == 1);

    recovery_engine_free(&engine);
}

typedef enum single_recovery_negative_case {
    SINGLE_NEGATIVE_TEI,
    SINGLE_NEGATIVE_WRONG_PID,
    SINGLE_NEGATIVE_WRONG_CC,
    SINGLE_NEGATIVE_MISSING_CANDIDATE,
    SINGLE_NEGATIVE_LOW_ALIGNMENT,
    SINGLE_NEGATIVE_NO_AFTER_ANCHOR
} single_recovery_negative_case_t;

static void run_single_packet_negative_recovery_case(single_recovery_negative_case_t negative_case,
                                                     recovery_reject_reason_t expected_reason)
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
    engine.config.min_alignment_confidence = 80;
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x110, (uint8_t)i, (uint8_t)(0x40 + i));
    }

    make_packet(secondary_c, 0x110, 12, 0x70);
    make_packet(primary_d, 0x110, 13, 0x71);
    make_packet(secondary_d, 0x110, 13, 0x71);
    if (negative_case == SINGLE_NEGATIVE_TEI) {
        mark_transport_error(secondary_c);
    } else if (negative_case == SINGLE_NEGATIVE_WRONG_PID) {
        make_packet(secondary_c, 0x111, 12, 0x70);
    } else if (negative_case == SINGLE_NEGATIVE_WRONG_CC) {
        make_packet(secondary_c, 0x110, 11, 0x70);
    } else if (negative_case == SINGLE_NEGATIVE_LOW_ALIGNMENT) {
        engine.alignment.confidence = 0;
        stats.alignment_confidence = 0;
    }
    if (negative_case != SINGLE_NEGATIVE_MISSING_CANDIDATE) {
        push_packet(&engine, &stats, 1, secondary_c);
    }
    push_packet(&engine, &stats, 0, primary_d);
    if (negative_case != SINGLE_NEGATIVE_NO_AFTER_ANCHOR) {
        push_packet(&engine, &stats, 1, secondary_d);
    }

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 13);
    assert(memcmp(capture.packets[12], primary_d, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[12], secondary_c, TS_PACKET_SIZE) != 0);
    assert(stats.recovered_content_packets == 0);
    assert(stats.recovered_packets == 0);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.output_continuity_errors == 1);
    assert(stats.recovery_rejects[expected_reason] == 1);

    recovery_engine_free(&engine);
}

static void test_exact_single_packet_content_recovery_negative_cases(void)
{
    run_single_packet_negative_recovery_case(SINGLE_NEGATIVE_TEI, RECOVERY_REJECT_TEI);
    run_single_packet_negative_recovery_case(SINGLE_NEGATIVE_WRONG_PID, RECOVERY_REJECT_WRONG_PID);
    run_single_packet_negative_recovery_case(SINGLE_NEGATIVE_WRONG_CC,
                                             RECOVERY_REJECT_WRONG_COUNTER);
    run_single_packet_negative_recovery_case(SINGLE_NEGATIVE_MISSING_CANDIDATE,
                                             RECOVERY_REJECT_MISSING_CANDIDATE);
    run_single_packet_negative_recovery_case(SINGLE_NEGATIVE_LOW_ALIGNMENT,
                                             RECOVERY_REJECT_LOW_ALIGNMENT);
    run_single_packet_negative_recovery_case(SINGLE_NEGATIVE_NO_AFTER_ANCHOR,
                                             RECOVERY_REJECT_AMBIGUOUS);
}

static void test_burst_gap_recovery(void)
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
    assert(stats.recovered_packets == 3);
    assert(stats.recovered_content_bursts == 1);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.output_continuity_errors == 0);
    recovery_engine_free(&engine);
}

static void run_bounded_same_pid_burst_recovery(unsigned gap)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t secondary_gap[10][TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;

    assert(gap >= 2 && gap <= 10);
    init_engine(&engine, &stats, &capture, &sink);
    engine.config.max_content_burst_packets = 10;

    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x134, (uint8_t)i, (uint8_t)(0x20 + i));
    }

    for (i = 0; i < gap; i++) {
        make_packet(secondary_gap[i], 0x134, (uint8_t)((12U + i) & 0x0fU),
                    (uint8_t)(0x60 + i));
        push_packet(&engine, &stats, 1, secondary_gap[i]);
    }
    make_packet(primary_after, 0x134, (uint8_t)((12U + gap) & 0x0fU), 0x80);
    make_packet(secondary_after, 0x134, (uint8_t)((12U + gap) & 0x0fU), 0x80);
    push_packet(&engine, &stats, 0, primary_after);
    push_packet(&engine, &stats, 1, secondary_after);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 13 + gap);
    for (i = 0; i < gap; i++) {
        assert(memcmp(capture.packets[12 + i], secondary_gap[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[12 + gap], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_content_packets == gap);
    assert(stats.recovered_packets == gap);
    assert(stats.recovered_content_bursts == 1);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.output_continuity_errors == 0);

    recovery_engine_free(&engine);
}

static void test_bounded_same_pid_burst_recovery_counts(void)
{
    run_bounded_same_pid_burst_recovery(2);
    run_bounded_same_pid_burst_recovery(3);
    run_bounded_same_pid_burst_recovery(5);
    run_bounded_same_pid_burst_recovery(10);
}

static void test_same_pid_gap_recovers_from_wider_stale_anchor_interval(void)
{
    enum {
        PID = 0x0031,
        OTHER_PID = 0x0032,
        WARMUP_PACKETS = 12,
        PRIMARY_INTERLOPERS = 20,
        CONTENT_GAP = 8,
        SECONDARY_INTERVAL = 24
    };
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    uint8_t secondary_gap[CONTENT_GAP][TS_PACKET_SIZE];
    size_t i;
    size_t content_index = 0;

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.max_content_burst_packets = 255;

    for (i = 0; i < WARMUP_PACKETS; i++) {
        push_pair(&engine, &stats, PID, (uint8_t)i, (uint8_t)(0x20 + i));
    }
    assert(recovery_engine_flush(&engine) == 0);
    assert(engine.last_primary_anchor.valid);

    for (i = 0; i < PRIMARY_INTERLOPERS; i++) {
        uint8_t packet[TS_PACKET_SIZE];

        make_packet(packet, OTHER_PID, (uint8_t)(i & 0x0fU), (uint8_t)(0x40 + i));
        stamp_packet_unique(packet, 0x3150, (uint16_t)i);
        push_packet(&engine, &stats, 0, packet);
    }
    assert(recovery_engine_flush(&engine) == 0);

    for (i = 0; i < SECONDARY_INTERVAL; i++) {
        uint8_t packet[TS_PACKET_SIZE];

        if ((i % 3U) == 0U && content_index < CONTENT_GAP) {
            make_packet(packet, PID, (uint8_t)(WARMUP_PACKETS + content_index),
                        (uint8_t)(0x70 + content_index));
            stamp_packet_unique(packet, 0x3160, (uint16_t)content_index);
            memcpy(secondary_gap[content_index], packet, TS_PACKET_SIZE);
            content_index++;
        } else {
            make_packet(packet, OTHER_PID, (uint8_t)((PRIMARY_INTERLOPERS + i) & 0x0fU),
                        (uint8_t)(0x90 + i));
            stamp_packet_unique(packet, 0x3170, (uint16_t)i);
        }
        push_packet(&engine, &stats, 1, packet);
    }
    assert(content_index == CONTENT_GAP);

    make_packet(primary_after, PID, (uint8_t)(WARMUP_PACKETS + CONTENT_GAP), 0xd0);
    make_packet(secondary_after, PID, (uint8_t)(WARMUP_PACKETS + CONTENT_GAP), 0xd0);
    stamp_packet_unique(primary_after, 0x3180, 0);
    stamp_packet_unique(secondary_after, 0x3180, 0);
    push_packet(&engine, &stats, 1, secondary_after);
    push_packet(&engine, &stats, 0, primary_after);

    assert(recovery_engine_flush(&engine) == 0);
    assert(stats.recovered_packets == CONTENT_GAP);
    assert(stats.recovered_content_packets == CONTENT_GAP);
    assert(stats.recovered_content_bursts == 1);
    assert(stats.output_continuity_errors == 0);
    assert(stats.unrecoverable_loss == 0);
    for (i = 0; i < CONTENT_GAP; i++) {
        assert(memcmp(capture.packets[WARMUP_PACKETS + PRIMARY_INTERLOPERS + i],
                      secondary_gap[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[WARMUP_PACKETS + PRIMARY_INTERLOPERS + CONTENT_GAP],
                  primary_after, TS_PACKET_SIZE) == 0);

    recovery_engine_free(&engine);
}

static void test_bounded_same_pid_burst_respects_configured_max(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t secondary_gap[5][TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.max_content_burst_packets = 3;

    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x135, (uint8_t)i, (uint8_t)(0x30 + i));
    }
    for (i = 0; i < 5; i++) {
        make_packet(secondary_gap[i], 0x135, (uint8_t)((12U + i) & 0x0fU),
                    (uint8_t)(0x70 + i));
        push_packet(&engine, &stats, 1, secondary_gap[i]);
    }
    make_packet(primary_after, 0x135, 1, 0x90);
    make_packet(secondary_after, 0x135, 1, 0x90);
    push_packet(&engine, &stats, 0, primary_after);
    push_packet(&engine, &stats, 1, secondary_after);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 13);
    assert(memcmp(capture.packets[12], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_content_packets == 0);
    assert(stats.recovered_packets == 0);
    assert(stats.recovered_content_bursts == 0);
    assert(stats.output_continuity_errors == 1);
    assert(stats.recovery_rejects[RECOVERY_REJECT_BURST_TOO_LARGE] == 1);

    recovery_engine_free(&engine);
}

static void test_mixed_pid_burst_recovery_with_null(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t missing[5][TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        uint16_t pid = (uint16_t)(0x140 + (i % 3U));
        uint8_t cc = (uint8_t)(i / 3U);

        push_pair(&engine, &stats, pid, cc, (uint8_t)(0x30 + i));
    }

    make_packet(missing[0], 0x140, 4, 0x60);
    make_null_packet(missing[1], 0x61);
    make_packet(missing[2], 0x141, 4, 0x62);
    make_packet(missing[3], 0x142, 4, 0x63);
    make_packet(missing[4], 0x140, 5, 0x64);
    for (i = 0; i < 5; i++) {
        push_packet(&engine, &stats, 1, missing[i]);
    }
    make_packet(primary_after, 0x141, 5, 0x80);
    make_packet(secondary_after, 0x141, 5, 0x80);
    push_packet(&engine, &stats, 0, primary_after);
    push_packet(&engine, &stats, 1, secondary_after);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 18);
    for (i = 0; i < 5; i++) {
        assert(memcmp(capture.packets[12 + i], missing[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[17], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == 5);
    assert(stats.recovered_content_packets == 4);
    assert(stats.recovered_null_packets == 1);
    assert(stats.recovered_content_bursts == 1);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.output_continuity_errors == 0);

    recovery_engine_free(&engine);
}

static void test_mixed_pid_burst_rejects_counter_contradiction(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t missing[3][TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        uint16_t pid = (uint16_t)(0x150 + (i % 3U));
        uint8_t cc = (uint8_t)(i / 3U);

        push_pair(&engine, &stats, pid, cc, (uint8_t)(0x40 + i));
    }

    make_packet(missing[0], 0x150, 4, 0x70);
    make_packet(missing[1], 0x151, 6, 0x71);
    make_packet(missing[2], 0x152, 4, 0x72);
    for (i = 0; i < 3; i++) {
        push_packet(&engine, &stats, 1, missing[i]);
    }
    make_packet(primary_after, 0x151, 5, 0x90);
    make_packet(secondary_after, 0x151, 5, 0x90);
    push_packet(&engine, &stats, 0, primary_after);
    push_packet(&engine, &stats, 1, secondary_after);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 13);
    assert(memcmp(capture.packets[12], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == 0);
    assert(stats.recovered_content_packets == 0);
    assert(stats.recovered_null_packets == 0);
    assert(stats.output_continuity_errors == 1);
    assert(stats.recovery_rejects[RECOVERY_REJECT_WRONG_COUNTER] == 1);

    recovery_engine_free(&engine);
}

static void test_primary_outage_fails_over_to_secondary(void)
{
    recovery_engine_config_t config = recovery_engine_default_config();
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t packet[TS_PACKET_SIZE];
    uint64_t now_ns;
    size_t i;

    config.primary_delay_ns = 0;
    config.primary_outage_ns = 1000000000ULL;
    config.primary_return_ns = 1000000000ULL;
    config.min_alignment_confidence = 0;
    init_engine_with_config(&engine, &stats, &capture, &sink, &config);

    for (i = 0; i < 4; i++) {
        make_packet(packet, 0x120, (uint8_t)i, (uint8_t)(0x20 + i));
        push_packet(&engine, &stats, 1, packet);
        push_packet(&engine, &stats, 0, packet);
    }
    assert(capture.packet_count == 4);
    assert(engine.active_output_stream_id == 0);
    engine.config.primary_outage_ns = 1000000000ULL;

    for (i = 4; i < 9; i++) {
        make_packet(packet, 0x120, (uint8_t)i, (uint8_t)(0x20 + i));
        push_packet(&engine, &stats, 1, packet);
    }

    now_ns = report_stats_now_ns();
    engine.last_input_arrival_ns[0] = now_ns - 2000000000ULL;
    engine.last_input_arrival_ns[1] = now_ns;
    assert(recovery_engine_drain(&engine, false) == 0);

    assert(engine.active_output_stream_id == 1);
    assert(stats.active_output_stream_id == 1);
    assert(stats.source_switches[1] == 1);
    assert(capture.packet_count == 9);
    for (i = 0; i < 9; i++) {
        make_packet(packet, 0x120, (uint8_t)i, (uint8_t)(0x20 + i));
        assert(memcmp(capture.packets[i], packet, TS_PACKET_SIZE) == 0);
    }

    recovery_engine_free(&engine);
}

static void test_primary_return_switches_back_after_holdoff(void)
{
    recovery_engine_config_t config = recovery_engine_default_config();
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t packet[TS_PACKET_SIZE];
    uint64_t now_ns;
    size_t i;

    config.primary_delay_ns = 0;
    config.primary_outage_ns = 1000000000ULL;
    config.primary_return_ns = 0;
    config.min_alignment_confidence = 0;
    init_engine_with_config(&engine, &stats, &capture, &sink, &config);

    for (i = 0; i < 4; i++) {
        make_packet(packet, 0x121, (uint8_t)i, (uint8_t)(0x30 + i));
        push_packet(&engine, &stats, 1, packet);
        push_packet(&engine, &stats, 0, packet);
    }
    for (i = 4; i < 7; i++) {
        make_packet(packet, 0x121, (uint8_t)i, (uint8_t)(0x30 + i));
        push_packet(&engine, &stats, 1, packet);
    }

    now_ns = report_stats_now_ns();
    engine.config.primary_outage_ns = 1000000000ULL;
    engine.last_input_arrival_ns[0] = now_ns - 2000000000ULL;
    engine.last_input_arrival_ns[1] = now_ns;
    assert(recovery_engine_drain(&engine, false) == 0);
    assert(engine.active_output_stream_id == 1);
    assert(capture.packet_count == 7);
    engine.config.primary_outage_ns = 1000000000ULL;

    for (i = 7; i < 10; i++) {
        make_packet(packet, 0x121, (uint8_t)i, (uint8_t)(0x30 + i));
        push_packet(&engine, &stats, 0, packet);
    }
    now_ns = report_stats_now_ns();
    engine.last_input_arrival_ns[0] = now_ns;
    engine.last_input_arrival_ns[1] = now_ns;
    assert(recovery_engine_drain(&engine, false) == 0);
    assert(recovery_engine_drain(&engine, false) == 0);

    assert(engine.active_output_stream_id == 0);
    assert(stats.active_output_stream_id == 0);
    assert(stats.source_switches[1] == 1);
    assert(stats.source_switches[0] == 1);
    assert(capture.packet_count == 10);
    for (i = 0; i < 10; i++) {
        make_packet(packet, 0x121, (uint8_t)i, (uint8_t)(0x30 + i));
        assert(memcmp(capture.packets[i], packet, TS_PACKET_SIZE) == 0);
    }

    recovery_engine_free(&engine);
}

static void test_primary_switchback_guard_survives_null_until_informative_fit(void)
{
    recovery_engine_config_t config = recovery_engine_default_config();
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t packet[TS_PACKET_SIZE];

    config.primary_delay_ns = 0;
    config.min_alignment_confidence = 0;
    init_engine_with_config(&engine, &stats, &capture, &sink, &config);

    make_packet(packet, 0x130, 0, 0x40);
    push_packet(&engine, &stats, 0, packet);

    engine.primary_switchback_guard = true;
    engine.primary_switchback_guard_informative = 1;

    make_null_packet(packet, 0x41);
    push_packet(&engine, &stats, 0, packet);

    make_packet(packet, 0x130, 2, 0x42);
    push_packet(&engine, &stats, 0, packet);

    make_packet(packet, 0x130, 1, 0x43);
    push_packet(&engine, &stats, 0, packet);

    assert(stats.output_continuity_errors == 0);
    assert(stats.output_duplicate_counters == 0);
    assert(capture.packet_count == 3);

    recovery_engine_free(&engine);
}

static void test_secondary_outage_keeps_primary_output(void)
{
    recovery_engine_config_t config = recovery_engine_default_config();
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t packet[TS_PACKET_SIZE];
    size_t i;

    config.primary_delay_ns = 0;
    config.primary_outage_ns = 1000000000ULL;
    config.min_alignment_confidence = 0;
    init_engine_with_config(&engine, &stats, &capture, &sink, &config);

    for (i = 0; i < 4; i++) {
        make_packet(packet, 0x122, (uint8_t)i, (uint8_t)(0x40 + i));
        push_packet(&engine, &stats, 1, packet);
        push_packet(&engine, &stats, 0, packet);
    }
    for (i = 4; i < 10; i++) {
        make_packet(packet, 0x122, (uint8_t)i, (uint8_t)(0x40 + i));
        push_packet(&engine, &stats, 0, packet);
    }

    assert(engine.active_output_stream_id == 0);
    assert(stats.source_switches[0] == 0);
    assert(stats.source_switches[1] == 0);
    assert(capture.packet_count == 10);
    for (i = 0; i < 10; i++) {
        make_packet(packet, 0x122, (uint8_t)i, (uint8_t)(0x40 + i));
        assert(memcmp(capture.packets[i], packet, TS_PACKET_SIZE) == 0);
    }

    recovery_engine_free(&engine);
}

static void test_primary_overflow_does_not_emit_while_failed_over(void)
{
    recovery_engine_config_t config = recovery_engine_default_config();
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t oldest[TS_PACKET_SIZE];
    uint8_t incoming[TS_PACKET_SIZE];

    config.primary_delay_ns = 0;
    init_engine_with_config(&engine, &stats, &capture, &sink, &config);

    make_packet(oldest, 0x131, 0, 0x50);
    make_packet(incoming, 0x131, 1, 0x51);
    engine.active_output_stream_id = 1;
    stats.active_output_stream_id = 1;
    engine.last_input_arrival_ns[1] = report_stats_now_ns();
    engine.primary_queue.count = engine.primary_queue.capacity;
    memcpy(engine.primary_queue.records[engine.primary_queue.start].packet, oldest, TS_PACKET_SIZE);

    push_packet(&engine, &stats, 0, incoming);

    assert(capture.packet_count == 0);
    assert(stats.primary_delay_overflows == 1);
    assert(engine.primary_queue.count == engine.primary_queue.capacity);

    recovery_engine_free(&engine);
}

static void test_due_secondary_fills_primary_stall_before_primary_returns(void)
{
    recovery_engine_config_t config = recovery_engine_default_config();
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_anchor[TS_PACKET_SIZE];
    uint8_t secondary_repair[TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint64_t now_ns;
    size_t secondary_queue_index;
    size_t secondary_history_index;

    config.primary_delay_ns = 0;
    config.min_alignment_confidence = 0;
    init_engine_with_config(&engine, &stats, &capture, &sink, &config);

    make_packet(primary_anchor, 0x132, 0, 0x60);
    push_packet(&engine, &stats, 1, primary_anchor);
    push_packet(&engine, &stats, 0, primary_anchor);
    assert(recovery_engine_flush(&engine) == 0);
    assert(engine.last_primary_anchor.valid);
    assert(capture.packet_count == 1);

    engine.config.primary_delay_ns = 1000000000ULL;
    engine.primary_queue.delay_ns = 1000000000ULL;
    engine.secondary_queue.delay_ns = 1000000000ULL;
    stats.observed_secondary_latency_samples = 1;
    stats.observed_secondary_latency_avg_ns = 500000000.0;

    make_packet(secondary_repair, 0x132, 1, 0x61);
    push_packet(&engine, &stats, 1, secondary_repair);
    make_packet(primary_after, 0x132, 2, 0x62);
    push_packet(&engine, &stats, 0, primary_after);

    now_ns = report_stats_now_ns();
    secondary_queue_index = (engine.secondary_queue.start + engine.secondary_queue.count - 1U) %
                            engine.secondary_queue.capacity;
    secondary_history_index = (engine.history[1].start + engine.history[1].count - 1U) %
                              engine.history[1].capacity;
    engine.secondary_queue.records[secondary_queue_index].arrival_time_ns = now_ns - 600000000ULL;
    engine.history[1].records[secondary_history_index].arrival_time_ns = now_ns - 600000000ULL;
    engine.primary_queue.records[engine.primary_queue.start].arrival_time_ns = now_ns;

    assert(recovery_engine_drain(&engine, false) == 0);
    assert(capture.packet_count == 2);
    assert(memcmp(capture.packets[1], secondary_repair, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == 1);
    assert(stats.output_continuity_errors == 0);

    recovery_engine_free(&engine);
}

static void test_due_secondary_fills_empty_primary_stall(void)
{
    recovery_engine_config_t config = recovery_engine_default_config();
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_anchor[TS_PACKET_SIZE];
    uint8_t secondary_repair[TS_PACKET_SIZE];
    uint64_t now_ns;
    size_t secondary_queue_index;
    size_t secondary_history_index;

    config.primary_delay_ns = 0;
    config.min_alignment_confidence = 0;
    init_engine_with_config(&engine, &stats, &capture, &sink, &config);

    make_packet(primary_anchor, 0x133, 0, 0x70);
    push_packet(&engine, &stats, 1, primary_anchor);
    push_packet(&engine, &stats, 0, primary_anchor);
    assert(recovery_engine_flush(&engine) == 0);
    assert(engine.last_primary_anchor.valid);
    assert(capture.packet_count == 1);

    engine.config.primary_delay_ns = 1000000000ULL;
    engine.primary_queue.delay_ns = 1000000000ULL;
    engine.secondary_queue.delay_ns = 1000000000ULL;
    stats.observed_secondary_latency_samples = 1;
    stats.observed_secondary_latency_avg_ns = 0.0;
    stats.pcr_timing_confidence[0] = 100;
    stats.pcr_timing_confidence[1] = 100;
    stats.pcr_delay_ns = -500000000.0;

    make_packet(secondary_repair, 0x133, 1, 0x71);
    push_packet(&engine, &stats, 1, secondary_repair);

    now_ns = report_stats_now_ns();
    secondary_queue_index = (engine.secondary_queue.start + engine.secondary_queue.count - 1U) %
                            engine.secondary_queue.capacity;
    secondary_history_index = (engine.history[1].start + engine.history[1].count - 1U) %
                              engine.history[1].capacity;
    engine.secondary_queue.records[secondary_queue_index].arrival_time_ns = now_ns - 600000000ULL;
    engine.history[1].records[secondary_history_index].arrival_time_ns = now_ns - 600000000ULL;
    engine.last_input_arrival_ns[0] = now_ns - RECOVERY_ENGINE_STREAM_GAP_MIN_NS;
    engine.last_input_arrival_ns[1] = now_ns;

    assert(recovery_engine_drain(&engine, false) == 0);
    assert(capture.packet_count == 2);
    assert(memcmp(capture.packets[1], secondary_repair, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == 1);
    assert(stats.output_continuity_errors == 0);

    recovery_engine_free(&engine);
}

static void test_counter_fallback_recovery_without_anchor(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_a[TS_PACKET_SIZE];
    uint8_t secondary_b[TS_PACKET_SIZE];
    uint8_t primary_c[TS_PACKET_SIZE];

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.min_alignment_confidence = 0;
    stats.pcr_timing_confidence[0] = 100;
    stats.pcr_timing_confidence[1] = 100;
    stats.pcr_delay_ns = -1900000000.0;

    make_packet(primary_a, 0x120, 0, 0x11);
    make_packet(secondary_b, 0x120, 1, 0x22);
    make_packet(primary_c, 0x120, 2, 0x33);

    push_packet(&engine, &stats, 0, primary_a);
    assert(recovery_engine_flush(&engine) == 0);
    push_packet(&engine, &stats, 1, secondary_b);
    push_packet(&engine, &stats, 0, primary_c);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 3);
    assert(memcmp(capture.packets[0], primary_a, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[1], secondary_b, TS_PACKET_SIZE) == 0);
    assert(memcmp(capture.packets[2], primary_c, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_content_packets == 1);
    assert(stats.recovered_packets == 1);
    assert(stats.unrecoverable_loss == 0);
    recovery_engine_free(&engine);
}

static void test_stream_time_range_recovery(void)
{
    enum {
        GAP_PACKETS = 20
    };
    const uint64_t base_ns = 1000000000ULL;
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_a[TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_gap[GAP_PACKETS][TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.min_alignment_confidence = 0;

    make_packet(primary_a, 0x121, 0, 0x10);
    push_packet(&engine, &stats, 0, primary_a);
    assert(recovery_engine_flush(&engine) == 0);

    engine.primary_gap.valid = true;
    engine.primary_gap.arrival_time_ns = base_ns;
    engine.primary_gap.last_recovered_secondary_arrival_ns = 0;
    engine.next_stream_index[1] = engine.next_stream_index[0] + GAP_PACKETS;

    for (i = 0; i < GAP_PACKETS; i++) {
        size_t index;

        make_packet(secondary_gap[i], 0x121, (uint8_t)((i + 1U) & 0x0fU),
                    (uint8_t)(0x30 + i));
        push_packet(&engine, &stats, 1, secondary_gap[i]);
        index = (engine.history[1].start + engine.history[1].count - 1U) %
                engine.history[1].capacity;
        engine.history[1].records[index].arrival_time_ns = base_ns + ((uint64_t)(i + 1U) * 1000000ULL);
    }

    make_packet(primary_after, 0x121, 5, 0x60);
    push_packet(&engine, &stats, 0, primary_after);
    assert(engine.primary_queue.count == 1);
    engine.primary_queue.records[engine.primary_queue.start].arrival_time_ns =
        base_ns + 50000000ULL;
    engine.next_stream_index[1] = engine.next_stream_index[0] + GAP_PACKETS;

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 2 + GAP_PACKETS);
    assert(memcmp(capture.packets[0], primary_a, TS_PACKET_SIZE) == 0);
    for (i = 0; i < GAP_PACKETS; i++) {
        assert(memcmp(capture.packets[1 + i], secondary_gap[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[1 + GAP_PACKETS], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == GAP_PACKETS);
    assert(stats.recovered_content_packets == GAP_PACKETS);
    assert(stats.unrecoverable_loss == 0);
    recovery_engine_free(&engine);
}

static void test_stream_time_range_skips_primary_boundary_twin(void)
{
    enum {
        GAP_PACKETS = 5
    };
    const uint64_t base_ns = 2000000000ULL;
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_a[TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_gap[GAP_PACKETS][TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;
    size_t index;

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.min_alignment_confidence = 0;

    make_packet(primary_a, 0x122, 0, 0x10);
    push_packet(&engine, &stats, 0, primary_a);
    assert(recovery_engine_flush(&engine) == 0);

    engine.primary_gap.valid = true;
    engine.primary_gap.arrival_time_ns = base_ns;
    engine.primary_gap.last_recovered_secondary_arrival_ns = 0;
    engine.next_stream_index[1] = engine.next_stream_index[0] + GAP_PACKETS;

    for (i = 0; i < GAP_PACKETS; i++) {
        make_packet(secondary_gap[i], 0x122, (uint8_t)((i + 1U) & 0x0fU),
                    (uint8_t)(0x40 + i));
        push_packet(&engine, &stats, 1, secondary_gap[i]);
        index = (engine.history[1].start + engine.history[1].count - 1U) %
                engine.history[1].capacity;
        engine.history[1].records[index].arrival_time_ns = base_ns + ((uint64_t)(i + 1U) * 1000000ULL);
    }

    make_packet(primary_after, 0x122, 6, 0x70);
    make_packet(secondary_after, 0x122, 6, 0x71);
    push_packet(&engine, &stats, 1, secondary_after);
    index = (engine.history[1].start + engine.history[1].count - 1U) %
            engine.history[1].capacity;
    engine.history[1].records[index].arrival_time_ns = base_ns + 50000000ULL;

    push_packet(&engine, &stats, 0, primary_after);
    assert(engine.primary_queue.count == 1);
    engine.primary_queue.records[engine.primary_queue.start].arrival_time_ns =
        base_ns + 50000000ULL;
    engine.next_stream_index[1] = engine.next_stream_index[0] + GAP_PACKETS;

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 2 + GAP_PACKETS);
    assert(memcmp(capture.packets[0], primary_a, TS_PACKET_SIZE) == 0);
    for (i = 0; i < GAP_PACKETS; i++) {
        assert(memcmp(capture.packets[1 + i], secondary_gap[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[1 + GAP_PACKETS], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == GAP_PACKETS);
    assert(stats.recovered_content_packets == GAP_PACKETS);
    assert(stats.unrecoverable_loss == 0);
    recovery_engine_free(&engine);
}

static void test_stream_time_range_recovers_null_only_gap(void)
{
    enum {
        GAP_PACKETS = 12
    };
    const uint64_t base_ns = 3000000000ULL;
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_a[TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_nulls[GAP_PACKETS][TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.min_alignment_confidence = 0;

    make_packet(primary_a, 0x123, 0, 0x10);
    push_packet(&engine, &stats, 0, primary_a);
    assert(recovery_engine_flush(&engine) == 0);

    engine.primary_gap.valid = true;
    engine.primary_gap.arrival_time_ns = base_ns;
    engine.primary_gap.last_recovered_secondary_arrival_ns = 0;
    engine.next_stream_index[1] = engine.next_stream_index[0] + GAP_PACKETS;

    for (i = 0; i < GAP_PACKETS; i++) {
        size_t index;

        make_null_packet(secondary_nulls[i], (uint8_t)(0x50 + i));
        push_packet(&engine, &stats, 1, secondary_nulls[i]);
        index = (engine.history[1].start + engine.history[1].count - 1U) %
                engine.history[1].capacity;
        engine.history[1].records[index].arrival_time_ns = base_ns + ((uint64_t)(i + 1U) * 1000000ULL);
    }

    make_packet(primary_after, 0x123, 1, 0x70);
    push_packet(&engine, &stats, 0, primary_after);
    assert(engine.primary_queue.count == 1);
    engine.primary_queue.records[engine.primary_queue.start].arrival_time_ns =
        base_ns + 50000000ULL;
    engine.next_stream_index[1] = engine.next_stream_index[0] + GAP_PACKETS;

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 2 + GAP_PACKETS);
    assert(memcmp(capture.packets[0], primary_a, TS_PACKET_SIZE) == 0);
    for (i = 0; i < GAP_PACKETS; i++) {
        assert(memcmp(capture.packets[1 + i], secondary_nulls[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[1 + GAP_PACKETS], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.continuity_errors[0] == 0);
    assert(stats.recovered_packets == GAP_PACKETS);
    assert(stats.recovered_null_packets == GAP_PACKETS);
    assert(stats.recovered_content_packets == 0);
    assert(stats.unrecoverable_loss == 0);
    recovery_engine_free(&engine);
}

static void test_primary_index_gap_recovers_with_stale_alignment_confidence(void)
{
    enum {
        OFFSET = 8,
        WARMUP_PACKETS = 12,
        GAP_PACKETS = 20
    };
    const uint64_t base_ns = 5000000000ULL;
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t secondary_gap[GAP_PACKETS][TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.min_alignment_confidence = 80;
    engine.config.max_content_burst_packets = GAP_PACKETS;

    for (i = 0; i < OFFSET; i++) {
        uint8_t prelude[TS_PACKET_SIZE];

        make_packet(prelude, (uint16_t)(0x0700 + i), (uint8_t)i, (uint8_t)(0x20 + i));
        push_packet(&engine, &stats, 1, prelude);
    }
    for (i = 0; i < WARMUP_PACKETS; i++) {
        uint8_t packet[TS_PACKET_SIZE];

        make_packet(packet, 0x124, (uint8_t)i, (uint8_t)(0x50 + i));
        push_packet(&engine, &stats, 0, packet);
        push_packet(&engine, &stats, 1, packet);
    }
    assert(recovery_engine_flush(&engine) == 0);
    assert(engine.last_primary_anchor.valid);
    engine.primary_gap.arrival_time_ns = base_ns;

    engine.alignment.confidence = 0;
    for (i = 0; i < GAP_PACKETS; i++) {
        make_packet(secondary_gap[i], 0x124, (uint8_t)((WARMUP_PACKETS + i) & 0x0fU),
                    (uint8_t)(0x70 + i));
        push_packet(&engine, &stats, 1, secondary_gap[i]);
    }

    make_packet(primary_after, 0x124, (uint8_t)((WARMUP_PACKETS + GAP_PACKETS) & 0x0fU), 0xa0);
    make_packet(secondary_after, 0x124, (uint8_t)((WARMUP_PACKETS + GAP_PACKETS) & 0x0fU), 0xa0);
    push_packet(&engine, &stats, 1, secondary_after);
    push_packet(&engine, &stats, 0, primary_after);
    assert(engine.primary_queue.count == 1);
    engine.primary_queue.records[engine.primary_queue.start].arrival_time_ns =
        base_ns + 2000000000ULL;
    engine.next_stream_index[1] = engine.next_stream_index[0] + 300ULL;

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == WARMUP_PACKETS + GAP_PACKETS + 1);
    for (i = 0; i < GAP_PACKETS; i++) {
        assert(memcmp(capture.packets[WARMUP_PACKETS + i], secondary_gap[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[WARMUP_PACKETS + GAP_PACKETS], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == GAP_PACKETS);
    assert(stats.recovered_content_packets == GAP_PACKETS);
    assert(stats.unrecoverable_loss == 0);
    assert(stats.output_continuity_errors == 0);
    recovery_engine_free(&engine);
}

static void test_null_only_gap_recovered_separately(void)
{
    enum {
        GAP_PACKETS = 6
    };
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t secondary_nulls[GAP_PACKETS][TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x160, (uint8_t)i, (uint8_t)(0x40 + i));
    }

    for (i = 0; i < GAP_PACKETS; i++) {
        make_null_packet(secondary_nulls[i], (uint8_t)(0x70 + i));
        push_packet(&engine, &stats, 1, secondary_nulls[i]);
    }
    make_packet(primary_after, 0x160, 12, 0x90);
    make_packet(secondary_after, 0x160, 12, 0x90);
    push_packet(&engine, &stats, 0, primary_after);
    push_packet(&engine, &stats, 1, secondary_after);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 13 + GAP_PACKETS);
    for (i = 0; i < GAP_PACKETS; i++) {
        assert(memcmp(capture.packets[12 + i], secondary_nulls[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[12 + GAP_PACKETS], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == GAP_PACKETS);
    assert(stats.recovered_null_packets == GAP_PACKETS);
    assert(stats.recovered_content_packets == 0);
    assert(stats.recovered_content_bursts == 0);
    assert(stats.output_continuity_errors == 0);

    recovery_engine_free(&engine);
}

static void test_null_only_regions_do_not_establish_alignment(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t null_packet[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 16; i++) {
        make_null_packet(null_packet, (uint8_t)(0x20 + i));
        push_packet(&engine, &stats, 1, null_packet);
        push_packet(&engine, &stats, 0, null_packet);
    }

    assert(recovery_engine_flush(&engine) == 0);
    assert(!engine.alignment.has_alignment);
    assert(!engine.last_primary_anchor.valid);
    assert(stats.alignment_confidence == 0);
    assert(stats.recovered_packets == 0);

    recovery_engine_free(&engine);
}

static void test_pid_specific_loss_does_not_log_mixed_rejects_for_nulls(void)
{
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t secondary_gap[2][TS_PACKET_SIZE];
    uint8_t null_packet[TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    for (i = 0; i < 12; i++) {
        push_pair(&engine, &stats, 0x031, (uint8_t)i, (uint8_t)(0x50 + i));
    }

    make_packet(secondary_gap[0], 0x031, 12, 0x70);
    make_packet(secondary_gap[1], 0x031, 13, 0x71);
    push_packet(&engine, &stats, 1, secondary_gap[0]);
    push_packet(&engine, &stats, 1, secondary_gap[1]);

    for (i = 0; i < 8; i++) {
        make_null_packet(null_packet, (uint8_t)(0x80 + i));
        push_packet(&engine, &stats, 0, null_packet);
        push_packet(&engine, &stats, 1, null_packet);
    }

    make_packet(primary_after, 0x031, 14, 0x90);
    make_packet(secondary_after, 0x031, 14, 0x90);
    push_packet(&engine, &stats, 0, primary_after);
    push_packet(&engine, &stats, 1, secondary_after);

    assert(recovery_engine_flush(&engine) == 0);
    assert(stats.recovered_content_packets == 2);
    assert(stats.recovered_packets == 2);
    assert(stats.recovered_content_bursts == 1);
    assert(stats.output_continuity_errors == 0);
    assert(stats.recovery_rejects[RECOVERY_REJECT_AMBIGUOUS] == 0);

    recovery_engine_free(&engine);
}

static void test_anchor_gap_detection_only_with_counter_wrap(void)
{
    enum {
        WARMUP_PACKETS = 8,
        GAP_PACKETS = 16
    };
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t secondary_gap[GAP_PACKETS][TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_after[TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.max_content_burst_packets = GAP_PACKETS;

    for (i = 0; i < WARMUP_PACKETS; i++) {
        uint8_t packet[TS_PACKET_SIZE];

        make_packet(packet, 0x125, (uint8_t)(i & 0x0fU), (uint8_t)(0x30 + i));
        push_packet(&engine, &stats, 1, packet);
        push_packet(&engine, &stats, 0, packet);
    }
    assert(recovery_engine_flush(&engine) == 0);
    assert(engine.last_primary_anchor.valid);

    for (i = 0; i < GAP_PACKETS; i++) {
        make_packet(secondary_gap[i], 0x125, (uint8_t)((WARMUP_PACKETS + i) & 0x0fU),
                    (uint8_t)(0x60 + i));
        push_packet(&engine, &stats, 1, secondary_gap[i]);
    }

    make_packet(primary_after, 0x125, (uint8_t)((WARMUP_PACKETS + GAP_PACKETS) & 0x0fU), 0xa0);
    make_packet(secondary_after, 0x125, (uint8_t)((WARMUP_PACKETS + GAP_PACKETS) & 0x0fU), 0xa0);
    push_packet(&engine, &stats, 1, secondary_after);
    push_packet(&engine, &stats, 0, primary_after);

    assert(stats.continuity_errors[0] == 0);
    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == WARMUP_PACKETS + GAP_PACKETS + 1);
    for (i = 0; i < GAP_PACKETS; i++) {
        assert(memcmp(capture.packets[WARMUP_PACKETS + i], secondary_gap[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[WARMUP_PACKETS + GAP_PACKETS], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == GAP_PACKETS);
    assert(stats.recovered_content_packets == GAP_PACKETS);
    assert(stats.recovered_content_bursts == 1);
    assert(stats.output_continuity_errors == 0);
    assert(stats.unrecoverable_loss == 0);

    recovery_engine_free(&engine);
}

static void test_pid_time_range_recovery_wrap_gap(void)
{
    enum {
        PID = 49,
        GAP_PACKETS = 40
    };
    const uint64_t base_ns = 4000000000ULL;
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    uint8_t primary_a[TS_PACKET_SIZE];
    uint8_t primary_after[TS_PACKET_SIZE];
    uint8_t secondary_gap[GAP_PACKETS][TS_PACKET_SIZE];
    size_t i;

    init_engine(&engine, &stats, &capture, &sink);
    engine.config.min_alignment_confidence = 0;
    stats.pcr_timing_confidence[0] = 100;
    stats.pcr_timing_confidence[1] = 100;
    stats.pcr_delay_ns = -1900000000.0;

    make_packet(primary_a, PID, 0, 0x10);
    push_packet(&engine, &stats, 0, primary_a);
    assert(recovery_engine_flush(&engine) == 0);
    engine.output_pid_state[PID].last_arrival_time_ns = base_ns;

    for (i = 0; i < GAP_PACKETS; i++) {
        size_t index;

        make_packet(secondary_gap[i], PID, (uint8_t)((i + 1U) & 0x0fU),
                    (uint8_t)(0x40 + i));
        push_packet(&engine, &stats, 1, secondary_gap[i]);
        index = (engine.history[1].start + engine.history[1].count - 1U) %
                engine.history[1].capacity;
        engine.history[1].records[index].arrival_time_ns =
            base_ns + 1900000000ULL + ((uint64_t)(i + 1U) * 100000000ULL);
    }

    make_packet(primary_after, PID, (uint8_t)((GAP_PACKETS + 1U) & 0x0fU), 0x70);
    push_packet(&engine, &stats, 0, primary_after);
    assert(engine.primary_queue.count == 1);
    engine.primary_queue.records[engine.primary_queue.start].arrival_time_ns =
        base_ns + 5000000000ULL;

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == 2 + GAP_PACKETS);
    assert(memcmp(capture.packets[0], primary_a, TS_PACKET_SIZE) == 0);
    for (i = 0; i < GAP_PACKETS; i++) {
        assert(memcmp(capture.packets[1 + i], secondary_gap[i], TS_PACKET_SIZE) == 0);
    }
    assert(memcmp(capture.packets[1 + GAP_PACKETS], primary_after, TS_PACKET_SIZE) == 0);
    assert(stats.recovered_packets == GAP_PACKETS);
    assert(stats.recovered_content_packets == GAP_PACKETS);
    assert(stats.recovered_content_bursts == 1);
    assert(stats.output_continuity_errors == 0);
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

static void test_generated_content_recovery_sweep(void)
{
    uint16_t pids[] = {0x0100, 0x0101, 0x0110, 0x0120, 0x0130, 0x0140, 0x01f0, 0x0200};
    size_t pid_index;
    unsigned gap;
    unsigned scenarios = 0;

    for (pid_index = 0; pid_index < sizeof(pids) / sizeof(pids[0]); pid_index++) {
        for (gap = 1; gap <= 255; gap++) {
            recovery_engine_t engine;
            report_stats_t stats;
            capture_sink_t capture;
            packet_sink_t sink;
            uint8_t primary_anchor[TS_PACKET_SIZE];
            uint8_t secondary_anchor[TS_PACKET_SIZE];
            uint8_t expected_gap[255][TS_PACKET_SIZE];
            size_t i;
            uint16_t scenario = (uint16_t)((pid_index << 8U) | gap);

            init_engine(&engine, &stats, &capture, &sink);
            engine.config.max_content_burst_packets = 255;
            for (i = 0; i < 12; i++) {
                push_pair(&engine, &stats, pids[pid_index], (uint8_t)i,
                          (uint8_t)(0x20 + pid_index + i));
            }
            assert(recovery_engine_flush(&engine) == 0);

            for (i = 0; i < gap; i++) {
                make_packet(expected_gap[i], pids[pid_index], (uint8_t)(12 + i),
                            (uint8_t)(0x50 + gap + i));
                stamp_packet_unique(expected_gap[i], scenario, (uint16_t)i);
                push_packet(&engine, &stats, 1, expected_gap[i]);
            }

            make_packet(primary_anchor, pids[pid_index], (uint8_t)(12 + gap),
                        (uint8_t)(0x80 + gap));
            make_packet(secondary_anchor, pids[pid_index], (uint8_t)(12 + gap),
                        (uint8_t)(0x80 + gap));
            stamp_packet_unique(primary_anchor, scenario, (uint16_t)gap);
            stamp_packet_unique(secondary_anchor, scenario, (uint16_t)gap);
            push_packet(&engine, &stats, 0, primary_anchor);
            push_packet(&engine, &stats, 1, secondary_anchor);

            assert(recovery_engine_flush(&engine) == 0);
            assert(capture.packet_count == 13 + gap);
            assert(stats.recovered_content_packets == gap);
            assert(stats.recovered_packets == gap);
            assert(stats.output_continuity_errors == 0);
            assert(stats.unrecoverable_loss == 0);
            if (gap > 1) {
                assert(stats.recovered_content_bursts == 1);
            }
            for (i = 0; i < gap; i++) {
                assert(memcmp(capture.packets[12 + i], expected_gap[i], TS_PACKET_SIZE) == 0);
            }
            assert(memcmp(capture.packets[12 + gap], primary_anchor, TS_PACKET_SIZE) == 0);
            recovery_engine_free(&engine);
            scenarios++;
        }
    }

    assert(scenarios == 2040);
}

static void test_generated_ambiguous_recovery_sweep(void)
{
    enum {
        BAD_TEI = 0,
        BAD_DISCONTINUITY,
        BAD_PID,
        BAD_COUNTER,
        BAD_EXTRA_CONTENT,
        BAD_CASES
    };
    uint16_t pids[] = {0x0100, 0x0101, 0x0121, 0x0200, 0x0330};
    size_t pid_index;
    int bad_case;
    unsigned scenarios = 0;

    for (pid_index = 0; pid_index < sizeof(pids) / sizeof(pids[0]); pid_index++) {
        for (bad_case = 0; bad_case < BAD_CASES; bad_case++) {
            recovery_engine_t engine;
            report_stats_t stats;
            capture_sink_t capture;
            packet_sink_t sink;
            uint8_t candidate[TS_PACKET_SIZE];
            uint8_t primary_anchor[TS_PACKET_SIZE];
            uint8_t secondary_anchor[TS_PACKET_SIZE];
            size_t i;

            init_engine(&engine, &stats, &capture, &sink);
            for (i = 0; i < 12; i++) {
                push_pair(&engine, &stats, pids[pid_index], (uint8_t)i,
                          (uint8_t)(0x31 + pid_index + i));
            }
            assert(recovery_engine_flush(&engine) == 0);

            make_packet(candidate, pids[pid_index], 12, (uint8_t)(0xa0 + bad_case));
            if (bad_case == BAD_TEI) {
                mark_transport_error(candidate);
            } else if (bad_case == BAD_DISCONTINUITY) {
                mark_discontinuity(candidate);
            } else if (bad_case == BAD_PID) {
                make_packet(candidate, (uint16_t)(pids[pid_index] + 0x20U), 12, 0xa2);
            } else if (bad_case == BAD_COUNTER) {
                make_packet(candidate, pids[pid_index], 14, 0xa3);
            }
            push_packet(&engine, &stats, 1, candidate);

            if (bad_case == BAD_EXTRA_CONTENT) {
                make_packet(candidate, (uint16_t)(pids[pid_index] + 0x40U), 3, 0xa4);
                push_packet(&engine, &stats, 1, candidate);
            }

            make_packet(primary_anchor, pids[pid_index], 13, 0xb0);
            make_packet(secondary_anchor, pids[pid_index], 13, 0xb0);
            push_packet(&engine, &stats, 0, primary_anchor);
            push_packet(&engine, &stats, 1, secondary_anchor);

            assert(recovery_engine_flush(&engine) == 0);
            assert(capture.packet_count == 12);
            assert(stats.recovered_content_packets == 0);
            assert(stats.recovered_packets == 0);
            assert(stats.unrecoverable_loss >= 1);
            assert(stats.output_continuity_errors == 0);
            recovery_engine_free(&engine);
            scenarios++;
        }
    }

    assert(scenarios == 25);
}

static void test_generated_null_recovery_sweep(void)
{
    uint16_t pids[] = {0x0100, 0x0101, 0x0110, 0x0120, 0x0130, 0x0140, 0x01f0, 0x0200};
    size_t pid_index;
    unsigned nulls;
    unsigned scenarios = 0;

    for (pid_index = 0; pid_index < sizeof(pids) / sizeof(pids[0]); pid_index++) {
        for (nulls = 1; nulls <= 12; nulls++) {
            recovery_engine_t engine;
            report_stats_t stats;
            capture_sink_t capture;
            packet_sink_t sink;
            uint8_t primary_anchor[TS_PACKET_SIZE];
            uint8_t secondary_anchor[TS_PACKET_SIZE];
            uint8_t expected_nulls[12][TS_PACKET_SIZE];
            size_t i;

            init_engine(&engine, &stats, &capture, &sink);
            for (i = 0; i < 12; i++) {
                push_pair(&engine, &stats, pids[pid_index], (uint8_t)i,
                          (uint8_t)(0x42 + pid_index + i));
            }
            assert(recovery_engine_flush(&engine) == 0);

            for (i = 0; i < nulls; i++) {
                make_null_packet(expected_nulls[i], (uint8_t)(0x60 + i));
                push_packet(&engine, &stats, 1, expected_nulls[i]);
            }

            make_packet(primary_anchor, pids[pid_index], 12, (uint8_t)(0x90 + nulls));
            make_packet(secondary_anchor, pids[pid_index], 12, (uint8_t)(0x90 + nulls));
            push_packet(&engine, &stats, 0, primary_anchor);
            push_packet(&engine, &stats, 1, secondary_anchor);

            assert(recovery_engine_flush(&engine) == 0);
            assert(capture.packet_count == 13 + nulls);
            assert(stats.recovered_null_packets == nulls);
            assert(stats.recovered_packets == nulls);
            for (i = 0; i < nulls; i++) {
                assert(memcmp(capture.packets[12 + i], expected_nulls[i], TS_PACKET_SIZE) == 0);
            }
            assert(memcmp(capture.packets[12 + nulls], primary_anchor, TS_PACKET_SIZE) == 0);
            recovery_engine_free(&engine);
            scenarios++;
        }
    }

    assert(scenarios == 96);
}

static void test_generated_secondary_loss_sweep(void)
{
    uint16_t pids[] = {0x0100, 0x0101, 0x0110, 0x0120, 0x0130, 0x0140, 0x01f0, 0x0200};
    size_t pid_index;
    unsigned missing;
    unsigned scenarios = 0;

    for (pid_index = 0; pid_index < sizeof(pids) / sizeof(pids[0]); pid_index++) {
        for (missing = 1; missing <= 10; missing++) {
            recovery_engine_t engine;
            report_stats_t stats;
            capture_sink_t capture;
            packet_sink_t sink;
            uint8_t primary_only[10][TS_PACKET_SIZE];
            size_t i;

            init_engine(&engine, &stats, &capture, &sink);
            for (i = 0; i < 12; i++) {
                push_pair(&engine, &stats, pids[pid_index], (uint8_t)i,
                          (uint8_t)(0x18 + pid_index + i));
            }
            assert(recovery_engine_flush(&engine) == 0);

            for (i = 0; i < missing; i++) {
                make_packet(primary_only[i], pids[pid_index], (uint8_t)(12 + i),
                            (uint8_t)(0xc0 + i));
                push_packet(&engine, &stats, 0, primary_only[i]);
            }

            push_pair(&engine, &stats, pids[pid_index], (uint8_t)(12 + missing),
                      (uint8_t)(0xe0 + missing));

            assert(recovery_engine_flush(&engine) == 0);
            assert(capture.packet_count == 13 + missing);
            assert(stats.recovered_packets == 0);
            assert(stats.secondary_loss_events == 1);
            assert(stats.secondary_missing_packets == missing);
            recovery_engine_free(&engine);
            scenarios++;
        }
    }

    assert(scenarios == 80);
}

static void test_generated_offset_recovery_sweep(void)
{
    uint16_t pids[] = {0x0100, 0x0111, 0x0222, 0x0333};
    size_t pid_index;
    unsigned offset;
    unsigned gap;
    unsigned scenarios = 0;

    for (pid_index = 0; pid_index < sizeof(pids) / sizeof(pids[0]); pid_index++) {
        for (offset = 1; offset <= 8; offset++) {
            for (gap = 1; gap <= 5; gap++) {
                recovery_engine_t engine;
                report_stats_t stats;
                capture_sink_t capture;
                packet_sink_t sink;
                uint8_t candidate[5][TS_PACKET_SIZE];
                uint8_t primary_anchor[TS_PACKET_SIZE];
                uint8_t secondary_anchor[TS_PACKET_SIZE];
                size_t i;

                init_engine(&engine, &stats, &capture, &sink);

                for (i = 0; i < offset; i++) {
                    uint8_t prelude[TS_PACKET_SIZE];

                    make_packet(prelude, (uint16_t)(0x0600 + i), (uint8_t)i, (uint8_t)(0x10 + i));
                    push_packet(&engine, &stats, 1, prelude);
                }

                for (i = 0; i < 12; i++) {
                    uint8_t packet[TS_PACKET_SIZE];

                    make_packet(packet, pids[pid_index], (uint8_t)i, (uint8_t)(0x70 + i));
                    push_packet(&engine, &stats, 0, packet);
                    push_packet(&engine, &stats, 1, packet);
                }
                assert(recovery_engine_flush(&engine) == 0);

                for (i = 0; i < gap; i++) {
                    make_packet(candidate[i], pids[pid_index], (uint8_t)(12 + i),
                                (uint8_t)(0x95 + i));
                    push_packet(&engine, &stats, 1, candidate[i]);
                }

                make_packet(primary_anchor, pids[pid_index], (uint8_t)(12 + gap), 0xb8);
                make_packet(secondary_anchor, pids[pid_index], (uint8_t)(12 + gap), 0xb8);
                push_packet(&engine, &stats, 0, primary_anchor);
                push_packet(&engine, &stats, 1, secondary_anchor);

                assert(recovery_engine_flush(&engine) == 0);
                assert(capture.packet_count == 13 + gap);
                assert(stats.recovered_content_packets == gap);
                assert(stats.alignment_offset_packets == (int64_t)(offset + gap));
                for (i = 0; i < gap; i++) {
                    assert(memcmp(capture.packets[12 + i], candidate[i], TS_PACKET_SIZE) == 0);
                }
                assert(memcmp(capture.packets[12 + gap], primary_anchor, TS_PACKET_SIZE) == 0);
                recovery_engine_free(&engine);
                scenarios++;
            }
        }
    }

    assert(scenarios == 160);
}

static void test_generated_parser_sweep(void)
{
    uint16_t pid;
    unsigned continuity_counter;
    unsigned parsed = 0;

    for (pid = 0; pid < 512; pid += 17) {
        for (continuity_counter = 0; continuity_counter < 16; continuity_counter++) {
            uint8_t packet[TS_PACKET_SIZE];
            ts_packet_info_t info;

            make_packet(packet, pid, (uint8_t)continuity_counter,
                        (uint8_t)(pid + continuity_counter));
            assert(ts_packet_parse(packet, &info));
            assert(info.pid == pid);
            assert(info.continuity_counter == continuity_counter);
            assert(info.has_payload);
            assert(!info.transport_error);
            parsed++;
        }
    }

    assert(parsed == 496);
}

static size_t load_real_ts_packets(uint8_t packets[][TS_PACKET_SIZE], size_t max_packets)
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

static bool real_packet_is_recoverable_content(const uint8_t packet[TS_PACKET_SIZE], ts_packet_info_t *info)
{
    return ts_packet_parse(packet, info) && !info->is_null && !info->transport_error &&
           !info->discontinuity_indicator && info->has_payload;
}

static bool find_real_same_pid_run(uint8_t packets[][TS_PACKET_SIZE], size_t packet_count,
                                   size_t min_run, size_t *run_start)
{
    size_t i;

    for (i = 0; i + min_run <= packet_count; i++) {
        ts_packet_info_t first;
        size_t j;

        if (!real_packet_is_recoverable_content(packets[i], &first)) {
            continue;
        }

        for (j = 1; j < min_run; j++) {
            ts_packet_info_t current;
            uint8_t expected_counter = (uint8_t)((first.continuity_counter + j) & 0x0fU);

            if (!real_packet_is_recoverable_content(packets[i + j], &current) ||
                current.pid != first.pid || current.continuity_counter != expected_counter) {
                break;
            }
        }

        if (j == min_run) {
            *run_start = i;
            return true;
        }
    }

    return false;
}

static void test_real_file_parser_window(void)
{
    static uint8_t packets[REAL_TS_MAX_PACKETS][TS_PACKET_SIZE];
    bool seen_pid[REPORT_STATS_PIDS] = {false};
    size_t packet_count = load_real_ts_packets(packets, REAL_TS_MAX_PACKETS);
    size_t parsed_count = 0;
    size_t pid_count = 0;
    size_t pcr_count = 0;
    size_t i;

    if (packet_count == 0) {
        printf("skipping real TS parser test: %s not present\n", REAL_TS_PATH);
        return;
    }

    assert(packet_count == REAL_TS_MAX_PACKETS);
    for (i = 0; i < packet_count; i++) {
        ts_packet_info_t info;

        assert(ts_packet_parse(packets[i], &info));
        parsed_count++;
        if (!seen_pid[info.pid]) {
            seen_pid[info.pid] = true;
            pid_count++;
        }
        if (info.has_pcr) {
            pcr_count++;
        }
    }

    assert(parsed_count == packet_count);
    assert(pid_count >= 3);
    assert(pcr_count > 0);
}

static void test_real_file_alignment_with_offset(void)
{
    enum {
        OFFSET = 7,
        SHARED_PACKETS = 512
    };
    static uint8_t packets[REAL_TS_MAX_PACKETS][TS_PACKET_SIZE];
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    size_t packet_count = load_real_ts_packets(packets, REAL_TS_MAX_PACKETS);
    size_t i;

    if (packet_count == 0) {
        printf("skipping real TS alignment test: %s not present\n", REAL_TS_PATH);
        return;
    }

    assert(packet_count > OFFSET + SHARED_PACKETS);
    init_engine(&engine, &stats, &capture, &sink);

    for (i = 0; i < OFFSET; i++) {
        push_packet(&engine, &stats, 1, packets[i]);
    }
    for (i = 0; i < SHARED_PACKETS; i++) {
        push_packet(&engine, &stats, 0, packets[OFFSET + i]);
        push_packet(&engine, &stats, 1, packets[OFFSET + i]);
    }

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == SHARED_PACKETS);
    assert(stats.alignment_confidence >= 20);
    for (i = 0; i < SHARED_PACKETS; i++) {
        assert(memcmp(capture.packets[i], packets[OFFSET + i], TS_PACKET_SIZE) == 0);
    }

    recovery_engine_free(&engine);
}

static void test_real_file_content_recovery_run(void)
{
    enum {
        WARMUP_PACKETS = 12,
        GAP_PACKETS = 3,
        RUN_PACKETS = WARMUP_PACKETS + GAP_PACKETS + 1
    };
    static uint8_t packets[REAL_TS_MAX_PACKETS][TS_PACKET_SIZE];
    recovery_engine_t engine;
    report_stats_t stats;
    capture_sink_t capture;
    packet_sink_t sink;
    size_t packet_count = load_real_ts_packets(packets, REAL_TS_MAX_PACKETS);
    size_t run_start = 0;
    size_t i;

    if (packet_count == 0) {
        printf("skipping real TS content recovery test: %s not present\n", REAL_TS_PATH);
        return;
    }

    assert(find_real_same_pid_run(packets, packet_count, RUN_PACKETS, &run_start));
    init_engine(&engine, &stats, &capture, &sink);

    for (i = 0; i < WARMUP_PACKETS; i++) {
        push_packet(&engine, &stats, 0, packets[run_start + i]);
        push_packet(&engine, &stats, 1, packets[run_start + i]);
    }
    assert(recovery_engine_flush(&engine) == 0);

    for (i = 0; i < GAP_PACKETS; i++) {
        push_packet(&engine, &stats, 1, packets[run_start + WARMUP_PACKETS + i]);
    }
    push_packet(&engine, &stats, 0, packets[run_start + WARMUP_PACKETS + GAP_PACKETS]);
    push_packet(&engine, &stats, 1, packets[run_start + WARMUP_PACKETS + GAP_PACKETS]);

    assert(recovery_engine_flush(&engine) == 0);
    assert(capture.packet_count == RUN_PACKETS);
    assert(stats.recovered_content_packets == GAP_PACKETS);
    assert(stats.recovered_content_bursts == 1);
    for (i = 0; i < RUN_PACKETS; i++) {
        assert(memcmp(capture.packets[i], packets[run_start + i], TS_PACKET_SIZE) == 0);
    }

    recovery_engine_free(&engine);
}

int main(void)
{
    test_command_line_parse();
    test_udp_url_parsing();
    test_output_udp_pending_batch();
    test_packet_sink_helpers();
    test_recovery_engine_config();
    test_packet_metadata_history_retains_configured_time_window();
    test_ts_packet_parse();
    test_ts_packet_parse_edges();
    test_report_stats_observe_edges();
    test_report_stats_health_recovers_after_quiet_window();
    test_primary_pass_through();
    test_clean_dual_input_baseline_no_recovery();
    test_exact_single_packet_content_recovery();
    test_exact_single_packet_recovery_after_stale_global_anchor();
    test_null_match_does_not_replace_recovery_anchor();
    test_primary_tei_packet_replaced_from_clean_secondary();
    test_primary_tei_packet_not_replaced_from_tei_secondary();
    test_exact_single_packet_content_recovery_negative_cases();
    test_burst_gap_recovery();
    test_bounded_same_pid_burst_recovery_counts();
    test_same_pid_gap_recovers_from_wider_stale_anchor_interval();
    test_bounded_same_pid_burst_respects_configured_max();
    test_generated_content_recovery_sweep();
    test_mixed_pid_burst_recovery_with_null();
    test_mixed_pid_burst_rejects_counter_contradiction();
    test_null_only_gap_recovered_separately();
    test_null_only_regions_do_not_establish_alignment();
    test_pid_specific_loss_does_not_log_mixed_rejects_for_nulls();
    test_anchor_gap_detection_only_with_counter_wrap();
    printf("test_recovery: ok\n");
    return 0;
}
