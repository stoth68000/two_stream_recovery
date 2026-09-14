#include "command_line.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void command_line_print_usage(const char *program_name)
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
            "  --http-port <number>\n"
            "      Enable the REST stats API and web UI on 127.0.0.1:<number>.\n"
            "      Default: disabled\n"
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
            "  --history-ms <ms>\n"
            "      Rolling packet history target, used to size primary and secondary buffers.\n"
            "      Default: %llu\n"
            "\n"
            "  --max-content-burst-packets <count>\n"
            "      Maximum content packets to recover in one conservative burst.\n"
            "      Default: %u\n"
            "\n"
            "  --min-alignment-confidence <0-100>\n"
            "      Minimum alignment confidence required before content recovery.\n"
            "      Default: %u\n"
            "\n"
            "  -h, --help\n"
            "      Show this help page.\n",
            program_name,
            DEFAULT_OUTPUT_URL,
            (unsigned long long)(defaults.primary_delay_ns / 1000000ULL),
            (unsigned long long)(defaults.max_secondary_latency_ns / 1000000ULL),
            (unsigned long long)(defaults.alignment_window_ns / 1000000ULL),
            (unsigned long long)defaults.history_ms,
            defaults.max_content_burst_packets,
            defaults.min_alignment_confidence);
}

static int parse_u64_ms_option(const char *name, const char *value, uint64_t *target_ns,
                               bool print_errors)
{
    unsigned long long parsed;
    char *end = NULL;

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed > UINT64_MAX / 1000000ULL) {
        if (print_errors) {
            fprintf(stderr, "invalid millisecond value for %s: %s\n", name, value);
        }
        return -1;
    }

    *target_ns = (uint64_t)parsed * 1000000ULL;
    return 0;
}

static int parse_u64_option(const char *name, const char *value, uint64_t *target,
                            bool print_errors)
{
    unsigned long long parsed;
    char *end = NULL;

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        if (print_errors) {
            fprintf(stderr, "invalid integer value for %s: %s\n", name, value);
        }
        return -1;
    }

    *target = (uint64_t)parsed;
    return 0;
}

static int parse_u32_option(const char *name, const char *value, uint32_t *target,
                            uint32_t max_value, bool print_errors)
{
    uint64_t parsed;

    if (parse_u64_option(name, value, &parsed, print_errors) != 0 || parsed > max_value) {
        if (print_errors) {
            fprintf(stderr, "invalid bounded integer value for %s: %s\n", name, value);
        }
        return -1;
    }

    *target = (uint32_t)parsed;
    return 0;
}

static int command_line_parse_internal(int argc, char **argv, command_line_options_t *options,
                                       bool print_errors)
{
    int i;

    memset(options, 0, sizeof(*options));
    options->output_url = DEFAULT_OUTPUT_URL;
    options->recovery_config = recovery_engine_default_config();

    for (i = 1; i < argc; i++) {
        const char *name = argv[i];
        const char **target = NULL;
        uint64_t *ms_target_ns = NULL;
        uint64_t *u64_target = NULL;
        uint32_t *u32_target = NULL;
        uint32_t u32_max = UINT32_MAX;
        bool is_http_port = false;

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
        } else if (strcmp(name, "--http-port") == 0) {
            if (options->http_port != 0) {
                if (print_errors) {
                    fprintf(stderr, "duplicate option: %s\n", name);
                    command_line_print_usage(argv[0]);
                }
                return -1;
            }
            u32_target = &u32_max;
            u32_max = 65535U;
            is_http_port = true;
        } else if (strcmp(name, "--primary-delay-ms") == 0) {
            ms_target_ns = &options->recovery_config.primary_delay_ns;
        } else if (strcmp(name, "--max-secondary-latency-ms") == 0) {
            ms_target_ns = &options->recovery_config.max_secondary_latency_ns;
        } else if (strcmp(name, "--alignment-window-ms") == 0) {
            ms_target_ns = &options->recovery_config.alignment_window_ns;
        } else if (strcmp(name, "--history-ms") == 0) {
            u64_target = &options->recovery_config.history_ms;
        } else if (strcmp(name, "--max-content-burst-packets") == 0) {
            u32_target = &options->recovery_config.max_content_burst_packets;
            u32_max = 255U;
        } else if (strcmp(name, "--min-alignment-confidence") == 0) {
            u32_target = &options->recovery_config.min_alignment_confidence;
            u32_max = 100U;
        } else if (strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) {
            if (print_errors) {
                command_line_print_usage(argv[0]);
            }
            return 1;
        } else {
            if (print_errors) {
                fprintf(stderr, "unknown option: %s\n", name);
                command_line_print_usage(argv[0]);
            }
            return -1;
        }

        if (i + 1 >= argc) {
            if (print_errors) {
                fprintf(stderr, "missing value for option: %s\n", name);
                command_line_print_usage(argv[0]);
            }
            return -1;
        }

        if (ms_target_ns != NULL) {
            if (parse_u64_ms_option(name, argv[++i], ms_target_ns, print_errors) != 0) {
                if (print_errors) {
                    command_line_print_usage(argv[0]);
                }
                return -1;
            }
            continue;
        }
        if (u64_target != NULL) {
            if (parse_u64_option(name, argv[++i], u64_target, print_errors) != 0) {
                if (print_errors) {
                    command_line_print_usage(argv[0]);
                }
                return -1;
            }
            continue;
        }
        if (u32_target != NULL) {
            uint32_t parsed = 0;
            if (parse_u32_option(name, argv[++i], &parsed, u32_max, print_errors) != 0 ||
                (is_http_port && parsed == 0)) {
                if (print_errors) {
                    if (is_http_port && parsed == 0) {
                        fprintf(stderr, "invalid TCP port for %s: %s\n", name, argv[i]);
                    }
                    command_line_print_usage(argv[0]);
                }
                return -1;
            }
            if (is_http_port) {
                options->http_port = (uint16_t)parsed;
            } else {
                *u32_target = parsed;
            }
            continue;
        }

        if (*target != NULL && target != &options->output_url) {
            if (print_errors) {
                fprintf(stderr, "duplicate option: %s\n", name);
                command_line_print_usage(argv[0]);
            }
            return -1;
        }

        *target = argv[++i];
    }

    if (options->input_primary_url == NULL || options->input_secondary_url == NULL) {
        if (print_errors) {
            fprintf(stderr, "both input URLs are required\n");
            command_line_print_usage(argv[0]);
        }
        return -1;
    }

    return 0;
}

int command_line_parse(int argc, char **argv, command_line_options_t *options)
{
    return command_line_parse_internal(argc, argv, options, true);
}

int command_line_parse_silent(int argc, char **argv, command_line_options_t *options)
{
    return command_line_parse_internal(argc, argv, options, false);
}
