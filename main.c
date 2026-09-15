#include "command_line.h"
#include "input_udp.h"
#include "output_udp.h"
#include "recovery_engine.h"
#include "report_stats.h"
#include "ts_packet.h"
#include "web_server.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define INPUT_BUFFER_SIZE (TS_PACKET_SIZE * 64)

static volatile sig_atomic_t should_stop = 0;

static void handle_signal(int signum)
{
    (void)signum;
    should_stop = 1;
}

static bool has_sync_run(const uint8_t *buffer, size_t bytes, size_t offset)
{
    size_t i;

    for (i = 0; i < 4; i++) {
        size_t sync_offset = offset + (i * TS_PACKET_SIZE);
        if (sync_offset >= bytes || buffer[sync_offset] != TS_SYNC_BYTE) {
            return false;
        }
    }

    return true;
}

static bool find_resync_offset(const uint8_t *buffer, size_t bytes, size_t start_offset,
                               size_t *resync_offset)
{
    size_t offset;

    for (offset = start_offset + 1U; offset < bytes; offset++) {
        if (has_sync_run(buffer, bytes, offset)) {
            *resync_offset = offset;
            return true;
        }
    }

    return false;
}

static int process_datagram(recovery_engine_t *engine, report_stats_t *stats, int stream_id,
                            const uint8_t *buffer, size_t bytes)
{
    size_t offset;

    report_stats_observe_datagram(stats, stream_id, bytes);

    for (offset = 0; offset + TS_PACKET_SIZE <= bytes; offset += TS_PACKET_SIZE) {
        const uint8_t *packet = buffer + offset;
        ts_packet_info_t info;

        if (!ts_packet_parse(packet, &info)) {
            size_t resync_offset;

            stats->sync_errors[stream_id]++;
            stats->malformed_datagrams[stream_id]++;
            if (find_resync_offset(buffer, bytes, offset, &resync_offset)) {
                stats->resync_events[stream_id]++;
                offset = resync_offset - TS_PACKET_SIZE;
                continue;
            }
            continue;
        }

        report_stats_observe_packet(stats, stream_id, &info);
        if (recovery_engine_push_packet(engine, stream_id, packet, &info) != 0) {
            return -1;
        }
    }

    if (offset != bytes) {
        stats->sync_errors[stream_id]++;
        stats->partial_datagrams[stream_id]++;
    }

    return 0;
}

static int run_loop(input_udp_t inputs[2], output_udp_t *output,
                    const recovery_engine_config_t *recovery_config, uint16_t http_port,
                    bool console_report)
{
    uint8_t buffer[INPUT_BUFFER_SIZE];
    report_stats_t stats;
    recovery_engine_t engine;
    packet_sink_t sink;
    web_server_t web_server;
    bool web_enabled = false;
    int web_fd = -1;

    report_stats_init(&stats);
    memset(&web_server, 0, sizeof(web_server));
    web_server.fd = -1;
    if (http_port != 0) {
        if (web_server_open(&web_server, http_port, "webroot") != 0) {
            return -1;
        }
        web_enabled = true;
        web_fd = web_server.fd;
    }

    sink = output_udp_as_packet_sink(output);
    if (recovery_engine_init_with_config(&engine, &sink, &stats, recovery_config) != 0) {
        fprintf(stderr, "failed to initialize recovery engine\n");
        web_server_close(&web_server);
        return -1;
    }

    while (!should_stop) {
        fd_set read_fds;
        int max_fd = -1;
        struct timeval timeout;
        int ready;
        int i;

        FD_ZERO(&read_fds);
        for (i = 0; i < 2; i++) {
            FD_SET(inputs[i].fd, &read_fds);
            if (inputs[i].fd > max_fd) {
                max_fd = inputs[i].fd;
            }
        }
        if (web_enabled) {
            FD_SET(web_fd, &read_fds);
            if (web_fd > max_fd) {
                max_fd = web_fd;
            }
        }

        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            recovery_engine_free(&engine);
            web_server_close(&web_server);
            return -1;
        }

        if (web_enabled && ready > 0 && FD_ISSET(web_fd, &read_fds)) {
            web_server.fd = web_fd;
            web_server_handle_ready(&web_server, &stats);
            web_fd = web_server.fd;
            web_enabled = web_fd >= 0;
        }

        for (i = 0; i < 2; i++) {
            if (ready > 0 && FD_ISSET(inputs[i].fd, &read_fds)) {
                ssize_t received = input_udp_receive(&inputs[i], buffer, sizeof(buffer));
                if (received < 0) {
                    perror("recv");
                    recovery_engine_free(&engine);
                    return -1;
                }
                if (received > 0 &&
                    process_datagram(&engine, &stats, i, buffer, (size_t)received) != 0) {
                    recovery_engine_free(&engine);
                    web_server_close(&web_server);
                    return -1;
                }
            }
        }

        if (recovery_engine_drain(&engine, false) != 0) {
            recovery_engine_free(&engine);
            web_server_close(&web_server);
            return -1;
        }
        report_stats_tick(&stats);
        if (console_report) {
            report_stats_maybe_print(&stats, false);
        }
    }

    recovery_engine_flush(&engine);
    if (console_report) {
        report_stats_maybe_print(&stats, true);
    }
    recovery_engine_free(&engine);
    web_server_close(&web_server);
    return 0;
}

int main(int argc, char **argv)
{
    input_udp_t inputs[2];
    output_udp_t output;
    command_line_options_t options;
    int parse_result;
    int result = EXIT_FAILURE;

    parse_result = command_line_parse(argc, argv, &options);
    if (parse_result > 0) {
        return EXIT_SUCCESS;
    }
    if (parse_result < 0) {
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    if (input_udp_open_with_interface(&inputs[0], options.input_primary_url,
                                      options.input_primary_interface) != 0) {
        return EXIT_FAILURE;
    }
    if (input_udp_open_with_interface(&inputs[1], options.input_secondary_url,
                                      options.input_secondary_interface) != 0) {
        input_udp_close(&inputs[0]);
        return EXIT_FAILURE;
    }
    if (output_udp_open(&output, options.output_url) != 0) {
        input_udp_close(&inputs[1]);
        input_udp_close(&inputs[0]);
        return EXIT_FAILURE;
    }

    result = run_loop(inputs, &output, &options.recovery_config, options.http_port,
                      options.console_report) == 0
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;

    output_udp_close(&output);
    input_udp_close(&inputs[1]);
    input_udp_close(&inputs[0]);
    return result;
}
