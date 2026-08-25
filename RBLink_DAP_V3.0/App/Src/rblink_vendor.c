/*
 * LTLink V3 private protocol parser.
 *
 * This file is MCU-independent.  The STM32F407 port must provide the three
 * ltlink_platform_* functions below.  Keeping register access out of this file
 * allows a later MCU migration without changing the USB protocol.
 */
#include "rblink_vendor.h"

#define LTLINK_STATUS_OK          0x00U
#define LTLINK_STATUS_BAD_LENGTH  0x01U
#define LTLINK_STATUS_BAD_ACTION  0x02U
#define LTLINK_STATUS_IO_ERROR    0x03U
#define LTLINK_MAX_DATA           56U

enum {
    LTLINK_ACTION_CONFIG = 0x00U,
    LTLINK_ACTION_TRANSFER = 0x01U,
    LTLINK_ACTION_CLOSE = 0x02U
};

/* Implemented by source/hic_hal/stm32/stm32f407vg/ltlink_platform_stm32f407.c. */
extern uint8_t ltlink_platform_spi(const uint8_t *request, uint8_t request_len,
                                  uint8_t *response, uint8_t *response_len);
extern uint8_t ltlink_platform_i2c(const uint8_t *request, uint8_t request_len,
                                  uint8_t *response, uint8_t *response_len);
extern uint8_t ltlink_platform_can(const uint8_t *request, uint8_t request_len,
                                  uint8_t *response, uint8_t *response_len);
extern uint8_t ltlink_platform_power(const uint8_t *request, uint8_t request_len,
                                    uint8_t *response, uint8_t *response_len);

typedef uint8_t (*ltlink_handler_t)(const uint8_t *, uint8_t, uint8_t *, uint8_t *);

uint32_t ltlink_vendor_process(uint8_t command,
                               const uint8_t *request,
                               uint8_t *response) {
    ltlink_handler_t handler = 0;
    uint8_t request_len;
    uint8_t response_len = 0U;
    uint8_t status;

    /* Payload layout: [length][action][action-specific data...].
     * The command byte itself is handled by DAP_ProcessVendorCommand().
     */
    request_len = request[0];
    response[0] = command;
    /* INFO has no action byte. It lets the host reject incompatible protocol
     * revisions before enabling any target-facing hardware. */
    if (command == ID_LTLINK_INFO) {
        if (request_len != 0U) {
            response[1] = LTLINK_STATUS_BAD_LENGTH;
            response[2] = 0U;
            return (1U << 16) | 3U;
        }
        response[1] = LTLINK_STATUS_OK;
        response[2] = 4U;
        response[3] = LTLINK_PROTOCOL_MAJOR;
        response[4] = LTLINK_PROTOCOL_MINOR;
        response[5] = LTLINK_CAP_SPI | LTLINK_CAP_I2C | LTLINK_CAP_CAN | LTLINK_CAP_POWER;
        response[6] = 0U;
        return (1U << 16) | 7U;
    }
    if ((request_len < 1U) || (request_len > LTLINK_MAX_DATA)) {
        response[1] = LTLINK_STATUS_BAD_LENGTH;
        response[2] = 0U;
        return (1U << 16) | 3U;
    }
    if ((request[1] != LTLINK_ACTION_CONFIG) &&
        (request[1] != LTLINK_ACTION_TRANSFER) &&
        (request[1] != LTLINK_ACTION_CLOSE)) {
        response[1] = LTLINK_STATUS_BAD_ACTION;
        response[2] = 0U;
        return (((uint32_t)request_len + 1U) << 16) | 3U;
    }

    if (command == ID_LTLINK_SPI) {
        handler = ltlink_platform_spi;
    } else if (command == ID_LTLINK_I2C) {
        handler = ltlink_platform_i2c;
    } else if (command == ID_LTLINK_CAN) {
        handler = ltlink_platform_can;
    } else if (command == ID_LTLINK_POWER) {
        handler = ltlink_platform_power;
    }

    status = handler ? handler(&request[1], request_len,
                               &response[3], &response_len)
                     : LTLINK_STATUS_IO_ERROR;
    if (response_len > LTLINK_MAX_DATA) {
        status = LTLINK_STATUS_BAD_LENGTH;
        response_len = 0U;
    }
    response[1] = status;
    response[2] = response_len;
    return (((uint32_t)request_len + 1U) << 16) |
           ((uint32_t)response_len + 3U);
}
