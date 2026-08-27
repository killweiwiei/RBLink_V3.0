#include "rblink_link_protocol.h"

#include <string.h>

uint32_t rblink_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;

    while (length-- != 0U) {
        crc ^= *bytes++;
        for (unsigned int bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

void rblink_frame_finalize(rblink_frame_t *frame)
{
    if (frame->payload_length <= RBLINK_PAYLOAD_SIZE) {
        memset(&frame->payload[frame->payload_length], 0,
               RBLINK_PAYLOAD_SIZE - frame->payload_length);
    }
    frame->magic = RBLINK_FRAME_MAGIC;
    frame->version = RBLINK_PROTOCOL_VERSION;
    frame->reserved0 = 0U;
    frame->reserved1 = 0U;
    frame->crc32 = 0U;
    frame->crc32 = rblink_crc32(frame, sizeof(*frame));
}

bool rblink_frame_is_valid(const rblink_frame_t *frame)
{
    rblink_frame_t copy;
    uint32_t expected_crc;

    if ((frame->magic != RBLINK_FRAME_MAGIC) ||
        (frame->version != RBLINK_PROTOCOL_VERSION) ||
        (frame->payload_length > RBLINK_PAYLOAD_SIZE) ||
        (frame->reserved0 != 0U) || (frame->reserved1 != 0U) ||
        ((frame->flags & ~(RBLINK_FLAG_RESPONSE | RBLINK_FLAG_MORE |
                           RBLINK_FLAG_ERROR)) != 0U) ||
        !((frame->channel <= RBLINK_CHANNEL_LOG) ||
          (frame->channel == RBLINK_CHANNEL_IDLE))) {
        return false;
    }

    expected_crc = frame->crc32;
    memcpy(&copy, frame, sizeof(copy));
    copy.crc32 = 0U;
    return rblink_crc32(&copy, sizeof(copy)) == expected_crc;
}

void rblink_frame_make_idle(rblink_frame_t *frame, uint32_t sequence)
{
    memset(frame, 0, sizeof(*frame));
    frame->channel = RBLINK_CHANNEL_IDLE;
    frame->sequence = sequence;
    rblink_frame_finalize(frame);
}
