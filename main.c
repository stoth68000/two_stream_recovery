#include "command_line.h"
#include "input_udp.h"
#include "output_udp.h"
#include "recovery_engine.h"
#include "report_stats.h"
#include "ts_packet.h"

#include <errno.h>
#include <signal.h>
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

static int process_datagram(recovery_engine_t *engine, report_stats_t *stats, int stream_id,
                            const uint8_t *buffer, size_t bytes)
{
    size_t offset;

    for (offset = 0; offset + TS_PACKET_SIZE <= bytes; offset += TS_PACKET_SIZE) {
        const uint8_t *packet = buffer + offset;
        ts_packet_info_t info;

        if (!ts_packet_parse(packet, &info)) {
            stats->sync_errors[stream_id]++;
            continue;
        }

        report_stats_observe_packet(stats, stream_id, &info);
        if (recovery_engine_push_packet(engine, stream_id, packet, &info) != 0) {
            return -1;
        }
    }

    if (offset != bytes) {
        stats->sync_errors[stream_id]++;
    }

    return 0;
}

static int run_loop(input_udp_t inputs[2], output_udp_t *output,
                    const recovery_engine_config_t *recovery_config)
{
    uint8_t buffer[INPUT_BUFFER_SIZE];
    report_stats_t stats;
    recovery_engine_t engine;
    packet_sink_t sink;

    report_stats_init(&stats);
    sink = output_udp_as_packet_sink(output);
    if (recovery_engine_init_with_config(&engine, &sink, &stats, recovery_config) != 0) {
        fprintf(stderr, "failed to initialize recovery engine\n");
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

        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            recovery_engine_free(&engine);
            return -1;
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
                    return -1;
                }
            }
        }

        if (recovery_engine_drain(&engine, false) != 0) {
            recovery_engine_free(&engine);
            return -1;
        }
        report_stats_maybe_print(&stats, false);
    }

    recovery_engine_flush(&engine);
    report_stats_maybe_print(&stats, true);
    recovery_engine_free(&engine);
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

    result = run_loop(inputs, &output, &options.recovery_config) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    output_udp_close(&output);
    input_udp_close(&inputs[1]);
    input_udp_close(&inputs[0]);
    return result;
}
