#ifndef REPORT_STATS_H
#define REPORT_STATS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct report_stats {
    uint64_t packets_received[2];
    uint64_t sync_errors[2];
    uint64_t continuity_errors[2];
    uint64_t recovered_packets;
    uint64_t unrecoverable_loss;
    uint64_t output_packets;
    uint64_t last_report_ns;
} report_stats_t;

uint64_t report_stats_now_ns(void);
void report_stats_init(report_stats_t *stats);
void report_stats_maybe_print(report_stats_t *stats, bool force);

#endif
