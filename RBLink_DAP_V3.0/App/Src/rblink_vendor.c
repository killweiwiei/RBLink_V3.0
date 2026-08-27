/*
 * RBLink V3 private protocol parser.
 *
 * This file is MCU-independent.  The STM32F407 port must provide the three
 * rblink_platform_* functions below. Keeping register access out of this file
 * allows a later MCU migration without changing the USB protocol.
 */
#include "rblink_vendor.h"

#define RBLINK_STATUS_OK          0x00U
#define RBLINK_STATUS_BAD_LENGTH  0x01U
#define RBLINK_STATUS_BAD_ACTION  0x02U
#define RBLINK_STATUS_IO_ERROR    0x03U
#define RBLINK_MAX_DATA           56U

enum {
    RBLINK_ACTION_CONFIG = 0x00U,
    RBLINK_ACTION_TRANSFER = 0x01U,
    RBLINK_ACTION_CLOSE = 0x02U
};

/* Implemented by rblink_bus.c; the parser remains independent of STM32 HAL. */
extern uint8_t rblink_platform_spi(const uint8_t *request, uint8_t request_len,
                                  uint8_t *response, uint8_t *response_len);
extern uint8_t rblink_platform_i2c(const uint8_t *request, uint8_t request_len,
                                  uint8_t *response, uint8_t *response_len);
extern uint8_t rblink_platform_can(const uint8_t *request, uint8_t request_len,
                                  uint8_t *response, uint8_t *response_len);
extern uint8_t rblink_platform_power(const uint8_t *request, uint8_t request_len,
                                    uint8_t *response, uint8_t *response_len);
extern uint8_t rblink_platform_wifi(const uint8_t *request, uint8_t request_len,
                                   uint8_t *response, uint8_t *response_len);

typedef uint8_t (*ltlink_handler_t)(const uint8_t *, uint8_t, uint8_t *, uint8_t *);

uint32_t rblink_vendor_process(uint8_t command,
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
    if (command == ID_RBLINK_INFO) {
        if (request_len != 0U) {
            response[1] = RBLINK_STATUS_BAD_LENGTH;
            response[2] = 0U;
            return (1U << 16) | 3U;
        }
        response[1] = RBLINK_STATUS_OK;
        response[2] = 8U;
        response[3] = RBLINK_PROTOCOL_MAJOR;
        response[4] = RBLINK_PROTOCOL_MINOR;
        response[5] = RBLINK_CAP_SPI | RBLINK_CAP_I2C | RBLINK_CAP_CAN |
                      RBLINK_CAP_POWER | RBLINK_CAP_WIFI;
        response[6] = 0U;
        response[7] = RBLINK_FIRMWARE_MAJOR;
        response[8] = RBLINK_FIRMWARE_MINOR;
        response[9] = RBLINK_FIRMWARE_PATCH;
        response[10] = RBLINK_BOARD_REVISION;
        return (1U << 16) | 11U;
    }
    if ((request_len < 1U) || (request_len > RBLINK_MAX_DATA)) {
        response[1] = RBLINK_STATUS_BAD_LENGTH;
        response[2] = 0U;
        return (1U << 16) | 3U;
    }
    if ((request[1] != RBLINK_ACTION_CONFIG) &&
        (request[1] != RBLINK_ACTION_TRANSFER) &&
        (request[1] != RBLINK_ACTION_CLOSE)) {
        response[1] = RBLINK_STATUS_BAD_ACTION;
        response[2] = 0U;
        return (((uint32_t)request_len + 1U) << 16) | 3U;
    }

    if (command == ID_RBLINK_SPI) {
        handler = rblink_platform_spi;
    } else if (command == ID_RBLINK_I2C) {
        handler = rblink_platform_i2c;
    } else if (command == ID_RBLINK_CAN) {
        handler = rblink_platform_can;
    } else if (command == ID_RBLINK_POWER) {
        handler = rblink_platform_power;
    } else if (command == ID_RBLINK_WIFI) {
        handler = rblink_platform_wifi;
    }

    status = handler ? handler(&request[1], request_len,
                               &response[3], &response_len)
                     : RBLINK_STATUS_IO_ERROR;
    if (response_len > RBLINK_MAX_DATA) {
        status = RBLINK_STATUS_BAD_LENGTH;
        response_len = 0U;
    }
    response[1] = status;
    response[2] = response_len;
    return (((uint32_t)request_len + 1U) << 16) |
           ((uint32_t)response_len + 3U);
}
