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
#define DEFAULT_OUTPUT_URL "udp://127.0.0.1:4500"

static volatile sig_atomic_t should_stop = 0;

typedef struct command_line_options {
    const char *input_primary_url;
    const char *input_secondary_url;
    const char *input_primary_interface;
    const char *input_secondary_interface;
    const char *output_url;
    recovery_engine_config_t recovery_config;
} command_line_options_t;

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

static void print_usage(const char *program_name)
{
    recovery_engine_config_t defaults = recovery_engine_default_config();

    fprintf(stderr,
            "Usage:\n"
            "  %s --input-primary-url <url> --input-secondary-url <url> [options]\n"
            "\n"
            "Options:\n"
            "  --input-primary-url <url>\n"
            "      Required. UDP URL for stream #1, the preferred source of truth.\n"
            "      Multicast group URLs automatically join the group with IGMP.\n"
            "\n"
            "  --input-secondary-url <url>\n"
            "      Required. UDP URL for stream #2, the delayed recovery witness.\n"
            "      Multicast group URLs automatically join the group with IGMP.\n"
            "\n"
            "  --input-primary-interface <ipv4>\n"
            "      Local interface IPv4 address used when joining the primary multicast group.\n"
            "      Default: 0.0.0.0\n"
            "\n"
            "  --input-secondary-interface <ipv4>\n"
            "      Local interface IPv4 address used when joining the secondary multicast group.\n"
            "      Default: 0.0.0.0\n"
            "\n"
            "  --output-url <url>\n"
            "      UDP destination for recovered output. Default: %s\n"
            "\n"
            "  --primary-delay-ms <ms>\n"
            "      Milliseconds to delay stream #1 before output, allowing stream #2 time to arrive.\n"
            "      Default: %llu\n"
            "\n"
            "  --max-secondary-latency-ms <ms>\n"
            "      Maximum trusted arrival latency from stream #1 to stream #2 for recovery matches.\n"
            "      Default: %llu\n"
            "\n"
            "  --alignment-window-ms <ms>\n"
            "      Arrival-time search window used when comparing packets for stream alignment.\n"
            "      Default: %llu\n"
            "\n"
            "  -h, --help\n"
            "      Show this help page.\n",
            program_name,
            DEFAULT_OUTPUT_URL,
            (unsigned long long)(defaults.primary_delay_ns / 1000000ULL),
            (unsigned long long)(defaults.max_secondary_latency_ns / 1000000ULL),
            (unsigned long long)(defaults.alignment_window_ns / 1000000ULL));
}

static int parse_u64_ms_option(const char *name, const char *value, uint64_t *target_ns)
{
    unsigned long long parsed;
    char *end = NULL;

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed > UINT64_MAX / 1000000ULL) {
        fprintf(stderr, "invalid millisecond value for %s: %s\n", name, value);
        return -1;
    }

    *target_ns = (uint64_t)parsed * 1000000ULL;
    return 0;
}

static int parse_command_line(int argc, char **argv, command_line_options_t *options)
{
    int i;

    memset(options, 0, sizeof(*options));
    options->output_url = DEFAULT_OUTPUT_URL;
    options->recovery_config = recovery_engine_default_config();

    for (i = 1; i < argc; i++) {
        const char *name = argv[i];
        const char **target = NULL;
        uint64_t *ms_target_ns = NULL;

        if (strcmp(name, "--input-primary-url") == 0) {
            target = &options->input_primary_url;
        } else if (strcmp(name, "--input-secondary-url") == 0) {
            target = &options->input_secondary_url;
        } else if (strcmp(name, "--input-primary-interface") == 0) {
            target = &options->input_primary_interface;
        } else if (strcmp(name, "--input-secondary-interface") == 0) {
            target = &options->input_secondary_interface;
        } else if (strcmp(name, "--output-url") == 0) {
            target = &options->output_url;
        } else if (strcmp(name, "--primary-delay-ms") == 0) {
            ms_target_ns = &options->recovery_config.primary_delay_ns;
        } else if (strcmp(name, "--max-secondary-latency-ms") == 0) {
            ms_target_ns = &options->recovery_config.max_secondary_latency_ns;
        } else if (strcmp(name, "--alignment-window-ms") == 0) {
            ms_target_ns = &options->recovery_config.alignment_window_ns;
        } else if (strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) {
            print_usage(argv[0]);
            return 1;
        } else {
            fprintf(stderr, "unknown option: %s\n", name);
            print_usage(argv[0]);
            return -1;
        }

        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for option: %s\n", name);
            print_usage(argv[0]);
            return -1;
        }

        if (ms_target_ns != NULL) {
            if (parse_u64_ms_option(name, argv[++i], ms_target_ns) != 0) {
                print_usage(argv[0]);
                return -1;
            }
            continue;
        }

        if (*target != NULL && target != &options->output_url) {
            fprintf(stderr, "duplicate option: %s\n", name);
            print_usage(argv[0]);
            return -1;
        }

        *target = argv[++i];
    }

    if (options->input_primary_url == NULL || options->input_secondary_url == NULL) {
        fprintf(stderr, "both input URLs are required\n");
        print_usage(argv[0]);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    input_udp_t inputs[2];
    output_udp_t output;
    command_line_options_t options;
    int parse_result;
    int result = EXIT_FAILURE;

    parse_result = parse_command_line(argc, argv, &options);
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
