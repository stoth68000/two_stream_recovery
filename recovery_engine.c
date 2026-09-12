#include "recovery_engine.h"

void recovery_engine_init(recovery_engine_t *engine, output_udp_t *output, report_stats_t *stats)
{
    engine->output = output;
    engine->stats = stats;
}

int recovery_engine_push_packet(recovery_engine_t *engine, int stream_id, const uint8_t packet[TS_PACKET_SIZE])
{
    if (stream_id != 0) {
        return 0;
    }

    if (output_udp_send_ts_packet(engine->output, packet) != 0) {
        return -1;
    }

    engine->stats->output_packets++;
    return 0;
}

int recovery_engine_flush(recovery_engine_t *engine)
{
    return output_udp_flush(engine->output);
}
