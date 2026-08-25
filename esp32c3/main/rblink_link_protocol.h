#ifndef RBLINK_LINK_PROTOCOL_H
#define RBLINK_LINK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RBLINK_FRAME_MAGIC        UINT32_C(0x4B4C4252) /* "RBLK" on the wire */
#define RBLINK_PROTOCOL_VERSION   1U
#define RBLINK_FRAME_SIZE         512U
#define RBLINK_HEADER_SIZE        20U
#define RBLINK_PAYLOAD_SIZE       (RBLINK_FRAME_SIZE - RBLINK_HEADER_SIZE)

typedef enum {
    RBLINK_CHANNEL_CONTROL = 0,
    RBLINK_CHANNEL_DAP = 1,
    RBLINK_CHANNEL_UART = 2,
    RBLINK_CHANNEL_VENDOR = 3,
    RBLINK_CHANNEL_LOG = 4,
    RBLINK_CHANNEL_IDLE = 0xFF,
} rblink_channel_t;

enum {
    RBLINK_FLAG_RESPONSE = 1U << 0,
    RBLINK_FLAG_MORE = 1U << 1,
    RBLINK_FLAG_ERROR = 1U << 2,
};

enum {
    RBLINK_CONTROL_PING = 0x01,
    RBLINK_CONTROL_GET_INFO = 0x02,
};

/*
 * The exact same fixed-size frame is used over SPI and TCP. All integer fields
 * are little-endian. Keeping the transfer at 512 bytes satisfies ESP32-C3 DMA
 * alignment rules and avoids a second stream-framing protocol on the PC side.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t channel;
    uint8_t flags;
    uint8_t reserved0;
    uint32_t sequence;
    uint16_t payload_length;
    uint16_t reserved1;
    uint32_t crc32;
    uint8_t payload[RBLINK_PAYLOAD_SIZE];
} rblink_frame_t;

_Static_assert(sizeof(rblink_frame_t) == RBLINK_FRAME_SIZE,
               "RBlink frame must be exactly 512 bytes");

uint32_t rblink_crc32(const void *data, size_t length);
void rblink_frame_finalize(rblink_frame_t *frame);
bool rblink_frame_is_valid(const rblink_frame_t *frame);
void rblink_frame_make_idle(rblink_frame_t *frame, uint32_t sequence);

#endif
