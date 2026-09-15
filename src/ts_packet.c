#include "ts_packet.h"

#include <string.h>

bool ts_packet_has_sync(const uint8_t packet[TS_PACKET_SIZE])
{
    return packet[0] == TS_SYNC_BYTE;
}

bool ts_packet_parse(const uint8_t packet[TS_PACKET_SIZE], ts_packet_info_t *info)
{
    uint8_t adaptation_length;

    if (!ts_packet_has_sync(packet)) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->transport_error = (packet[1] & 0x80U) != 0;
    info->payload_unit_start = (packet[1] & 0x40U) != 0;
    info->pid = (uint16_t)(((packet[1] & 0x1fU) << 8U) | packet[2]);
    info->adaptation_field_control = (uint8_t)((packet[3] >> 4U) & 0x03U);
    info->continuity_counter = (uint8_t)(packet[3] & 0x0fU);
    info->has_adaptation = info->adaptation_field_control == 2 || info->adaptation_field_control == 3;
    info->has_payload = info->adaptation_field_control == 1 || info->adaptation_field_control == 3;
    info->is_null = info->pid == TS_NULL_PID;

    if (info->adaptation_field_control == 0) {
        return false;
    }

    if (!info->has_adaptation) {
        return true;
    }

    adaptation_length = packet[4];
    if (adaptation_length == 0) {
        return true;
    }
    if (5U + adaptation_length > TS_PACKET_SIZE) {
        return false;
    }

    info->discontinuity_indicator = (packet[5] & 0x80U) != 0;
    info->has_pcr = (packet[5] & 0x10U) != 0;
    if (info->has_pcr && adaptation_length >= 7) {
        info->pcr_base = ((uint64_t)packet[6] << 25U) |
                         ((uint64_t)packet[7] << 17U) |
                         ((uint64_t)packet[8] << 9U) |
                         ((uint64_t)packet[9] << 1U) |
                         ((uint64_t)packet[10] >> 7U);
        info->pcr_extension = (uint16_t)(((packet[10] & 0x01U) << 8U) | packet[11]);
    } else {
        info->has_pcr = false;
    }

    return true;
}
