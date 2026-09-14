#ifndef COMMAND_LINE_H
#define COMMAND_LINE_H

#include "recovery_engine.h"

#include <stdint.h>

#define DEFAULT_OUTPUT_URL "udp://127.0.0.1:4500"

typedef struct command_line_options {
    const char *input_primary_url;
    const char *input_secondary_url;
    const char *input_primary_interface;
    const char *input_secondary_interface;
    const char *output_url;
    uint16_t http_port;
    recovery_engine_config_t recovery_config;
} command_line_options_t;

void command_line_print_usage(const char *program_name);
int command_line_parse(int argc, char **argv, command_line_options_t *options);
int command_line_parse_silent(int argc, char **argv, command_line_options_t *options);

#endif
