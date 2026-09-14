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
    stats->observed_secondary_latency_min_ns = 0.0;
    stats->stream_health[0] = STREAM_HEALTH_HEALTHY;
    stats->stream_health[1] = STREAM_HEALTH_HEALTHY;
}

void report_stats_observe_datagram(report_stats_t *stats, int stream_id, size_t bytes)
{
    if (stream_id < 0 || stream_id >= REPORT_STATS_STREAMS) {
        return;
    }

    stats->input_datagrams[stream_id]++;
    if (bytes == 0 || bytes % TS_PACKET_SIZE != 0) {
        stats->partial_datagrams[stream_id]++;
    }
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

void report_stats_observe_output_datagram(report_stats_t *stats, bool short_flush)
{
    stats->output_datagrams++;
    if (short_flush) {
        stats->output_short_flushes++;
    }
}

static void report_stats_observe_latency_sample(report_stats_t *stats, double sample_ns)
{
    if (stats->observed_secondary_latency_samples == 0 ||
        sample_ns < stats->observed_secondary_latency_min_ns) {
        stats->observed_secondary_latency_min_ns = sample_ns;
    }
    if (sample_ns > stats->observed_secondary_latency_max_ns) {
        stats->observed_secondary_latency_max_ns = sample_ns;
    }

    stats->observed_secondary_latency_samples++;
    if (stats->observed_secondary_latency_samples == 1) {
        stats->observed_secondary_latency_avg_ns = sample_ns;
        stats->observed_secondary_latency_jitter_ns = 0.0;
    } else {
        double previous_avg = stats->observed_secondary_latency_avg_ns;
        double diff;

        stats->observed_secondary_latency_avg_ns =
            (previous_avg * 0.875) + (sample_ns * 0.125);
        diff = sample_ns > previous_avg ? sample_ns - previous_avg : previous_avg - sample_ns;
        stats->observed_secondary_latency_jitter_ns =
            (stats->observed_secondary_latency_jitter_ns * 0.875) + (diff * 0.125);
    }
}

void report_stats_observe_pcr_delay(report_stats_t *stats, double sample_ns)
{
    stats->pcr_delay_samples++;
    if (stats->pcr_delay_samples == 1) {
        stats->pcr_delay_ns = sample_ns;
    } else {
        stats->pcr_delay_ns = (stats->pcr_delay_ns * 0.875) + (sample_ns * 0.125);
    }

    report_stats_observe_latency_sample(stats, sample_ns < 0.0 ? -sample_ns : sample_ns);
}

void report_stats_observe_latency(report_stats_t *stats, uint64_t primary_arrival_ns,
                                  uint64_t secondary_arrival_ns, uint64_t max_latency_ns)
{
    double sample_ns;

    if (secondary_arrival_ns < primary_arrival_ns) {
        sample_ns = 0.0;
    } else {
        sample_ns = (double)(secondary_arrival_ns - primary_arrival_ns);
    }

    report_stats_observe_latency_sample(stats, sample_ns);

    if (max_latency_ns > 0 && sample_ns > (double)max_latency_ns) {
        stats->secondary_packets_too_late++;
    }
}

void report_stats_reject_recovery(report_stats_t *stats, recovery_reject_reason_t reason)
{
    if (reason >= 0 && reason < RECOVERY_REJECT_COUNT) {
        stats->recovery_rejects[reason]++;
    }
}

static const char *stream_health_name(stream_health_t health)
{
    switch (health) {
    case STREAM_HEALTH_HEALTHY:
        return "healthy";
    case STREAM_HEALTH_DEGRADED:
        return "degraded";
    case STREAM_HEALTH_LOSSY:
        return "lossy";
    case STREAM_HEALTH_UNTRUSTED:
        return "untrusted";
    }

    return "unknown";
}

static void report_stats_timestamp(char *timestamp, size_t timestamp_size)
{
    time_t wall_time;
    struct tm local_tm;

    wall_time = time(NULL);
    localtime_r(&wall_time, &local_tm);
    strftime(timestamp, timestamp_size, "%Y-%m-%d %H:%M:%S %z", &local_tm);
}

static void update_stream_health(report_stats_t *stats)
{
    int stream_id;

    for (stream_id = 0; stream_id < REPORT_STATS_STREAMS; stream_id++) {
        uint64_t errors = stats->sync_errors[stream_id] +
                          stats->transport_errors[stream_id] +
                          stats->continuity_errors[stream_id];
        uint64_t packets = stats->packets_received[stream_id];

        if (packets == 0) {
            stats->stream_health[stream_id] = STREAM_HEALTH_DEGRADED;
        } else if (errors * 1000ULL >= packets * 50ULL) {
            stats->stream_health[stream_id] = STREAM_HEALTH_UNTRUSTED;
        } else if (errors * 1000ULL >= packets * 10ULL) {
            stats->stream_health[stream_id] = STREAM_HEALTH_LOSSY;
        } else if (errors > 0) {
            stats->stream_health[stream_id] = STREAM_HEALTH_DEGRADED;
        } else {
            stats->stream_health[stream_id] = STREAM_HEALTH_HEALTHY;
        }
    }

    if (stats->secondary_packets_too_late > 0 &&
        stats->stream_health[1] == STREAM_HEALTH_HEALTHY) {
        stats->stream_health[1] = STREAM_HEALTH_DEGRADED;
    }
}

static void capture_rolling_sample(report_stats_t *stats)
{
    report_stats_sample_t *sample = &stats->rolling_samples[stats->rolling_index];

    memset(sample, 0, sizeof(*sample));
    sample->packets_received[0] = stats->packets_received[0];
    sample->packets_received[1] = stats->packets_received[1];
    sample->continuity_errors[0] = stats->continuity_errors[0];
    sample->continuity_errors[1] = stats->continuity_errors[1];
    sample->recovered_packets = stats->recovered_packets;
    sample->unrecoverable_loss = stats->unrecoverable_loss;
    sample->output_packets = stats->output_packets;
    sample->alignment_confidence = stats->alignment_confidence;

    stats->rolling_index = (stats->rolling_index + 1U) % REPORT_STATS_ROLLING_SECONDS;
    if (stats->rolling_count < REPORT_STATS_ROLLING_SECONDS) {
        stats->rolling_count++;
    }
}

static const report_stats_sample_t *oldest_rolling_sample(const report_stats_t *stats)
{
    if (stats->rolling_count == 0) {
        return NULL;
    }
    if (stats->rolling_count < REPORT_STATS_ROLLING_SECONDS) {
        return &stats->rolling_samples[0];
    }

    return &stats->rolling_samples[stats->rolling_index];
}

static void compute_rolling_window(const report_stats_t *stats,
                                   uint64_t win_packets[REPORT_STATS_STREAMS],
                                   uint64_t win_cc_errors[REPORT_STATS_STREAMS],
                                   uint64_t *win_recovered,
                                   uint64_t *win_unrecoverable,
                                   uint64_t *win_output)
{
    const report_stats_sample_t *oldest = oldest_rolling_sample(stats);

    win_packets[0] = 0;
    win_packets[1] = 0;
    win_cc_errors[0] = 0;
    win_cc_errors[1] = 0;
    *win_recovered = 0;
    *win_unrecoverable = 0;
    *win_output = 0;

    if (oldest == NULL) {
        return;
    }

    win_packets[0] = stats->packets_received[0] - oldest->packets_received[0];
    win_packets[1] = stats->packets_received[1] - oldest->packets_received[1];
    win_cc_errors[0] = stats->continuity_errors[0] - oldest->continuity_errors[0];
    win_cc_errors[1] = stats->continuity_errors[1] - oldest->continuity_errors[1];
    *win_recovered = stats->recovered_packets - oldest->recovered_packets;
    *win_unrecoverable = stats->unrecoverable_loss - oldest->unrecoverable_loss;
    *win_output = stats->output_packets - oldest->output_packets;
}

int report_stats_format_json(report_stats_t *stats, char *buffer, size_t buffer_size)
{
    char timestamp[64];
    uint64_t win_packets[REPORT_STATS_STREAMS];
    uint64_t win_cc_errors[REPORT_STATS_STREAMS];
    uint64_t win_recovered;
    uint64_t win_unrecoverable;
    uint64_t win_output;
    int written;

    update_stream_health(stats);
    report_stats_timestamp(timestamp, sizeof(timestamp));
    compute_rolling_window(stats, win_packets, win_cc_errors,
                           &win_recovered, &win_unrecoverable, &win_output);

    written = snprintf(
        buffer, buffer_size,
        "{"
        "\"timestamp\":\"%s\","
        "\"packets_received\":[%" PRIu64 ",%" PRIu64 "],"
        "\"win60_packets\":[%" PRIu64 ",%" PRIu64 "],"
        "\"input_datagrams\":[%" PRIu64 ",%" PRIu64 "],"
        "\"malformed_datagrams\":[%" PRIu64 ",%" PRIu64 "],"
        "\"partial_datagrams\":[%" PRIu64 ",%" PRIu64 "],"
        "\"resync_events\":[%" PRIu64 ",%" PRIu64 "],"
        "\"sync_errors\":[%" PRIu64 ",%" PRIu64 "],"
        "\"transport_errors\":[%" PRIu64 ",%" PRIu64 "],"
        "\"continuity_errors\":[%" PRIu64 ",%" PRIu64 "],"
        "\"win60_continuity_errors\":[%" PRIu64 ",%" PRIu64 "],"
        "\"health\":[\"%s\",\"%s\"],"
        "\"duplicate_counters\":[%" PRIu64 ",%" PRIu64 "],"
        "\"null_packets\":[%" PRIu64 ",%" PRIu64 "],"
        "\"pcr_packets\":[%" PRIu64 ",%" PRIu64 "],"
        "\"alignment_offset_packets\":%" PRId64 ","
        "\"alignment_confidence\":%u,"
        "\"pcr_timing_confidence\":[%u,%u],"
        "\"pcr_bitrate_bps\":[%.0f,%.0f],"
        "\"pcr_delay_ns\":%.0f,"
        "\"pcr_delay_samples\":%" PRIu64 ","
        "\"latency_ms\":{\"min\":%.3f,\"avg\":%.3f,\"max\":%.3f,\"jitter\":%.3f,\"samples\":%" PRIu64 "},"
        "\"stream_disagreements\":%" PRIu64 ","
        "\"primary_delay_overflows\":%" PRIu64 ","
        "\"recovered_null_packets\":%" PRIu64 ","
        "\"recovered_content_packets\":%" PRIu64 ","
        "\"recovered_content_bursts\":%" PRIu64 ","
        "\"unrecoverable_null_regions\":%" PRIu64 ","
        "\"secondary_loss_events\":%" PRIu64 ","
        "\"secondary_missing_packets\":%" PRIu64 ","
        "\"secondary_missing_anchors\":%" PRIu64 ","
        "\"secondary_packets_too_late\":%" PRIu64 ","
        "\"primary_delay_insufficient\":%" PRIu64 ","
        "\"recovery_rejects\":{\"low_alignment\":%" PRIu64 ",\"secondary_late\":%" PRIu64
        ",\"tei\":%" PRIu64 ",\"discontinuity\":%" PRIu64 ",\"wrong_pid\":%" PRIu64
        ",\"wrong_counter\":%" PRIu64 ",\"ambiguous\":%" PRIu64 ",\"burst_too_large\":%" PRIu64
        ",\"missing_candidate\":%" PRIu64 "},"
        "\"recovery_exact_cc_gap\":%" PRIu64 ","
        "\"recovered_packets\":%" PRIu64 ","
        "\"unrecoverable_loss\":%" PRIu64 ","
        "\"win60_recovered\":%" PRIu64 ","
        "\"win60_unrecoverable\":%" PRIu64 ","
        "\"output_packets\":%" PRIu64 ","
        "\"win60_output\":%" PRIu64 ","
        "\"output_continuity_errors\":%" PRIu64 ","
        "\"output_duplicate_counters\":%" PRIu64 ","
        "\"output_datagrams\":%" PRIu64 ","
        "\"output_short_flushes\":%" PRIu64
        "}",
        timestamp,
        stats->packets_received[0], stats->packets_received[1],
        win_packets[0], win_packets[1],
        stats->input_datagrams[0], stats->input_datagrams[1],
        stats->malformed_datagrams[0], stats->malformed_datagrams[1],
        stats->partial_datagrams[0], stats->partial_datagrams[1],
        stats->resync_events[0], stats->resync_events[1],
        stats->sync_errors[0], stats->sync_errors[1],
        stats->transport_errors[0], stats->transport_errors[1],
        stats->continuity_errors[0], stats->continuity_errors[1],
        win_cc_errors[0], win_cc_errors[1],
        stream_health_name(stats->stream_health[0]), stream_health_name(stats->stream_health[1]),
        stats->duplicate_counters[0], stats->duplicate_counters[1],
        stats->null_packets[0], stats->null_packets[1],
        stats->pcr_packets[0], stats->pcr_packets[1],
        stats->alignment_offset_packets, stats->alignment_confidence,
        stats->pcr_timing_confidence[0], stats->pcr_timing_confidence[1],
        stats->pcr_bitrate_bps[0], stats->pcr_bitrate_bps[1],
        stats->pcr_delay_ns,
        stats->pcr_delay_samples,
        stats->observed_secondary_latency_min_ns / 1000000.0,
        stats->observed_secondary_latency_avg_ns / 1000000.0,
        stats->observed_secondary_latency_max_ns / 1000000.0,
        stats->observed_secondary_latency_jitter_ns / 1000000.0,
        stats->observed_secondary_latency_samples,
        stats->stream_disagreements,
        stats->primary_delay_overflows,
        stats->recovered_null_packets,
        stats->recovered_content_packets,
        stats->recovered_content_bursts,
        stats->unrecoverable_null_regions,
        stats->secondary_loss_events,
        stats->secondary_missing_packets,
        stats->secondary_missing_anchors,
        stats->secondary_packets_too_late,
        stats->primary_delay_insufficient,
        stats->recovery_rejects[RECOVERY_REJECT_LOW_ALIGNMENT],
        stats->recovery_rejects[RECOVERY_REJECT_SECONDARY_LATE],
        stats->recovery_rejects[RECOVERY_REJECT_TEI],
        stats->recovery_rejects[RECOVERY_REJECT_DISCONTINUITY],
        stats->recovery_rejects[RECOVERY_REJECT_WRONG_PID],
        stats->recovery_rejects[RECOVERY_REJECT_WRONG_COUNTER],
        stats->recovery_rejects[RECOVERY_REJECT_AMBIGUOUS],
        stats->recovery_rejects[RECOVERY_REJECT_BURST_TOO_LARGE],
        stats->recovery_rejects[RECOVERY_REJECT_MISSING_CANDIDATE],
        stats->recovery_exact_cc_gap,
        stats->recovered_packets,
        stats->unrecoverable_loss,
        win_recovered,
        win_unrecoverable,
        stats->output_packets,
        win_output,
        stats->output_continuity_errors,
        stats->output_duplicate_counters,
        stats->output_datagrams,
        stats->output_short_flushes);

    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }
    return written;
}

void report_stats_maybe_print(report_stats_t *stats, bool force)
{
    uint64_t now_ns = report_stats_now_ns();
    char timestamp[64];
    uint64_t win_packets[REPORT_STATS_STREAMS] = {0, 0};
    uint64_t win_cc_errors[REPORT_STATS_STREAMS] = {0, 0};
    uint64_t win_recovered = 0;
    uint64_t win_unrecoverable = 0;
    uint64_t win_output = 0;

    if (!force && now_ns - stats->last_report_ns < 1000000000ULL) {
        return;
    }

    update_stream_health(stats);
    capture_rolling_sample(stats);
    compute_rolling_window(stats, win_packets, win_cc_errors,
                           &win_recovered, &win_unrecoverable, &win_output);
    report_stats_timestamp(timestamp, sizeof(timestamp));

    printf("%s packets_rx=[%" PRIu64 ",%" PRIu64 "] win60_packets=[%" PRIu64 ",%" PRIu64 "] "
           "input_datagrams=[%" PRIu64 ",%" PRIu64 "] malformed_datagrams=[%" PRIu64 ",%" PRIu64 "] "
           "partial_datagrams=[%" PRIu64 ",%" PRIu64 "] resync_events=[%" PRIu64 ",%" PRIu64 "] "
           "sync_errors=[%" PRIu64 ",%" PRIu64 "] "
           "tei=[%" PRIu64 ",%" PRIu64 "] cc_errors=[%" PRIu64 ",%" PRIu64 "] "
           "win60_cc_errors=[%" PRIu64 ",%" PRIu64 "] health=[%s,%s] "
           "duplicate_cc=[%" PRIu64 ",%" PRIu64 "] null=[%" PRIu64 ",%" PRIu64 "] "
           "pcr=[%" PRIu64 ",%" PRIu64 "] align_offset=%" PRId64 " align_confidence=%u "
           "pcr_confidence=[%u,%u] pcr_bitrate_bps=[%.0f,%.0f] pcr_delay_ns=%.0f "
           "latency_ms=[min=%.3f,avg=%.3f,max=%.3f,jitter=%.3f,samples=%" PRIu64 "] "
           "disagreements=%" PRIu64 " delay_overflows=%" PRIu64 " recovered_null=%" PRIu64 " "
           "recovered_content=%" PRIu64 " recovered_bursts=%" PRIu64
           " unrecoverable_null_regions=%" PRIu64
           " secondary_loss_events=%" PRIu64 " secondary_missing_packets=%" PRIu64
           " secondary_missing_anchors=%" PRIu64
           " secondary_late=%" PRIu64 " primary_delay_insufficient=%" PRIu64
           " recovery_rejects=[low_align=%" PRIu64 ",late=%" PRIu64 ",tei=%" PRIu64
           ",discontinuity=%" PRIu64 ",pid=%" PRIu64 ",cc=%" PRIu64 ",ambiguous=%" PRIu64
           ",burst=%" PRIu64 ",missing=%" PRIu64 "] exact_cc_gap=%" PRIu64
           " recovered=%" PRIu64 " unrecoverable=%" PRIu64
           " win60_recovered=%" PRIu64 " win60_unrecoverable=%" PRIu64
           " output=%" PRIu64 " win60_output=%" PRIu64
           " output_cc_errors=%" PRIu64 " output_duplicate_cc=%" PRIu64
           " output_datagrams=%" PRIu64
           " output_short_flushes=%" PRIu64 "\n",
           timestamp,
           stats->packets_received[0], stats->packets_received[1],
           win_packets[0], win_packets[1],
           stats->input_datagrams[0], stats->input_datagrams[1],
           stats->malformed_datagrams[0], stats->malformed_datagrams[1],
           stats->partial_datagrams[0], stats->partial_datagrams[1],
           stats->resync_events[0], stats->resync_events[1],
           stats->sync_errors[0], stats->sync_errors[1],
           stats->transport_errors[0], stats->transport_errors[1],
           stats->continuity_errors[0], stats->continuity_errors[1],
           win_cc_errors[0], win_cc_errors[1],
           stream_health_name(stats->stream_health[0]), stream_health_name(stats->stream_health[1]),
           stats->duplicate_counters[0], stats->duplicate_counters[1],
           stats->null_packets[0], stats->null_packets[1],
           stats->pcr_packets[0], stats->pcr_packets[1],
           stats->alignment_offset_packets, stats->alignment_confidence,
           stats->pcr_timing_confidence[0], stats->pcr_timing_confidence[1],
           stats->pcr_bitrate_bps[0], stats->pcr_bitrate_bps[1], stats->pcr_delay_ns,
           stats->observed_secondary_latency_min_ns / 1000000.0,
           stats->observed_secondary_latency_avg_ns / 1000000.0,
           stats->observed_secondary_latency_max_ns / 1000000.0,
           stats->observed_secondary_latency_jitter_ns / 1000000.0,
           stats->observed_secondary_latency_samples,
           stats->stream_disagreements, stats->primary_delay_overflows,
           stats->recovered_null_packets, stats->recovered_content_packets,
           stats->recovered_content_bursts, stats->unrecoverable_null_regions,
           stats->secondary_loss_events, stats->secondary_missing_packets,
           stats->secondary_missing_anchors,
           stats->secondary_packets_too_late, stats->primary_delay_insufficient,
           stats->recovery_rejects[RECOVERY_REJECT_LOW_ALIGNMENT],
           stats->recovery_rejects[RECOVERY_REJECT_SECONDARY_LATE],
           stats->recovery_rejects[RECOVERY_REJECT_TEI],
           stats->recovery_rejects[RECOVERY_REJECT_DISCONTINUITY],
           stats->recovery_rejects[RECOVERY_REJECT_WRONG_PID],
           stats->recovery_rejects[RECOVERY_REJECT_WRONG_COUNTER],
           stats->recovery_rejects[RECOVERY_REJECT_AMBIGUOUS],
           stats->recovery_rejects[RECOVERY_REJECT_BURST_TOO_LARGE],
           stats->recovery_rejects[RECOVERY_REJECT_MISSING_CANDIDATE],
           stats->recovery_exact_cc_gap,
           stats->recovered_packets, stats->unrecoverable_loss,
           win_recovered, win_unrecoverable,
           stats->output_packets, win_output,
           stats->output_continuity_errors, stats->output_duplicate_counters,
           stats->output_datagrams, stats->output_short_flushes);
    fflush(stdout);

    stats->last_report_ns = now_ns;
}
