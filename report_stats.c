#include "report_stats.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

uint64_t report_stats_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

void report_stats_init(report_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->last_report_ns = report_stats_now_ns();
}

void report_stats_maybe_print(report_stats_t *stats, bool force)
{
    uint64_t now_ns = report_stats_now_ns();
    time_t wall_time;
    struct tm local_tm;
    char timestamp[64];

    if (!force && now_ns - stats->last_report_ns < 1000000000ULL) {
        return;
    }

    wall_time = time(NULL);
    localtime_r(&wall_time, &local_tm);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %z", &local_tm);

    printf("%s packets_rx=[%" PRIu64 ",%" PRIu64 "] sync_errors=[%" PRIu64 ",%" PRIu64 "] "
           "cc_errors=[%" PRIu64 ",%" PRIu64 "] recovered=%" PRIu64 " unrecoverable=%" PRIu64
           " output=%" PRIu64 "\n",
           timestamp,
           stats->packets_received[0], stats->packets_received[1],
           stats->sync_errors[0], stats->sync_errors[1],
           stats->continuity_errors[0], stats->continuity_errors[1],
           stats->recovered_packets, stats->unrecoverable_loss, stats->output_packets);
    fflush(stdout);

    stats->last_report_ns = now_ns;
}
