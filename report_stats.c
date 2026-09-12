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

void report_stats_observe_packet(report_stats_t *stats, int stream_id, const ts_packet_info_t *info)
{
    uint16_t pid = info->pid;

    if (stream_id < 0 || stream_id >= REPORT_STATS_STREAMS) {
        return;
    }

    stats->packets_received[stream_id]++;
    stats->per_pid_packets[stream_id][pid]++;

    if (info->transport_error) {
        stats->transport_errors[stream_id]++;
    }
    if (info->is_null) {
        stats->null_packets[stream_id]++;
    }
    if (info->has_pcr) {
        stats->pcr_packets[stream_id]++;
    }

    if (info->is_null || info->transport_error || info->discontinuity_indicator) {
        stats->last_continuity_counter[stream_id][pid] = info->continuity_counter;
        stats->has_continuity_counter[stream_id][pid] = true;
        return;
    }

    if (!stats->has_continuity_counter[stream_id][pid]) {
        stats->last_continuity_counter[stream_id][pid] = info->continuity_counter;
        stats->has_continuity_counter[stream_id][pid] = true;
        return;
    }

    if (info->has_payload) {
        uint8_t expected = (uint8_t)((stats->last_continuity_counter[stream_id][pid] + 1U) & 0x0fU);
        if (info->continuity_counter == stats->last_continuity_counter[stream_id][pid]) {
            stats->duplicate_counters[stream_id]++;
        } else if (info->continuity_counter != expected) {
            stats->continuity_errors[stream_id]++;
        }
    } else if (info->continuity_counter != stats->last_continuity_counter[stream_id][pid]) {
        stats->continuity_errors[stream_id]++;
    }

    stats->last_continuity_counter[stream_id][pid] = info->continuity_counter;
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
           "tei=[%" PRIu64 ",%" PRIu64 "] cc_errors=[%" PRIu64 ",%" PRIu64 "] "
           "duplicate_cc=[%" PRIu64 ",%" PRIu64 "] null=[%" PRIu64 ",%" PRIu64 "] "
           "pcr=[%" PRIu64 ",%" PRIu64 "] align_offset=%" PRId64 " align_confidence=%u "
           "disagreements=%" PRIu64 " recovered=%" PRIu64 " unrecoverable=%" PRIu64
           " output=%" PRIu64 "\n",
           timestamp,
           stats->packets_received[0], stats->packets_received[1],
           stats->sync_errors[0], stats->sync_errors[1],
           stats->transport_errors[0], stats->transport_errors[1],
           stats->continuity_errors[0], stats->continuity_errors[1],
           stats->duplicate_counters[0], stats->duplicate_counters[1],
           stats->null_packets[0], stats->null_packets[1],
           stats->pcr_packets[0], stats->pcr_packets[1],
           stats->alignment_offset_packets, stats->alignment_confidence,
           stats->stream_disagreements,
           stats->recovered_packets, stats->unrecoverable_loss, stats->output_packets);
    fflush(stdout);

    stats->last_report_ns = now_ns;
}
