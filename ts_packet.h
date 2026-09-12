#ifndef TS_PACKET_H
#define TS_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#define TS_PACKET_SIZE 188
#define TS_SYNC_BYTE 0x47
#define TS_NULL_PID 0x1fff

typedef struct ts_packet_info {
    uint16_t pid;
    uint8_t continuity_counter;
    uint8_t adaptation_field_control;
    bool transport_error;
    bool payload_unit_start;
    bool has_payload;
    bool has_adaptation;
    bool discontinuity_indicator;
    bool has_pcr;
    uint64_t pcr_base;
    uint16_t pcr_extension;
    bool is_null;
} ts_packet_info_t;

bool ts_packet_has_sync(const uint8_t packet[TS_PACKET_SIZE]);
bool ts_packet_parse(const uint8_t packet[TS_PACKET_SIZE], ts_packet_info_t *info);

#endif
