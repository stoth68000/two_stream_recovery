#include "ts_packet.h"

bool ts_packet_has_sync(const uint8_t packet[TS_PACKET_SIZE])
{
    return packet[0] == TS_SYNC_BYTE;
}
