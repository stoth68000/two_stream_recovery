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
