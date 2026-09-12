#ifndef TS_PACKET_H
#define TS_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#define TS_PACKET_SIZE 188
#define TS_SYNC_BYTE 0x47

bool ts_packet_has_sync(const uint8_t packet[TS_PACKET_SIZE]);

#endif
