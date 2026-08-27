#include <assert.h>
#include <string.h>

#include "rblink_link_protocol.h"

int main(void)
{
    static const char check[] = "123456789";
    rblink_frame_t frame = {0};

    /* Standard IEEE CRC-32 check vector. */
    assert(rblink_crc32(check, sizeof(check) - 1U) == UINT32_C(0xCBF43926));

    frame.channel = RBLINK_CHANNEL_CONTROL;
    frame.sequence = UINT32_C(0x12345678);
    frame.payload_length = 4U;
    memcpy(frame.payload, "PING", 4U);
    rblink_frame_finalize(&frame);
    assert(rblink_frame_is_valid(&frame));

    frame.payload[0] ^= 1U;
    assert(!rblink_frame_is_valid(&frame));
    frame.payload[0] ^= 1U;
    assert(rblink_frame_is_valid(&frame));

    frame.reserved0 = 1U;
    frame.crc32 = 0U;
    frame.crc32 = rblink_crc32(&frame, sizeof(frame));
    assert(!rblink_frame_is_valid(&frame));
    frame.reserved0 = 0U;

    memset(frame.payload, 0xA5, sizeof(frame.payload));
    frame.channel = RBLINK_CHANNEL_CONTROL;
    frame.payload_length = 1U;
    rblink_frame_finalize(&frame);
    assert(frame.payload[0] == 0xA5U);
    assert(frame.payload[1] == 0U);
    assert(rblink_frame_is_valid(&frame));

    rblink_frame_make_idle(&frame, 9U);
    assert(frame.channel == RBLINK_CHANNEL_IDLE);
    assert(frame.sequence == 9U);
    assert(rblink_frame_is_valid(&frame));
    return 0;
}
