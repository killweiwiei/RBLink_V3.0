/*
 * RBLink V3 private CMSIS-DAP vendor commands.
 *
 * Commands 0x85..0x87 are unused by upstream DAPLink v0258.  Keeping the
 * protocol behind this small interface makes the STM32F407 peripheral driver
 * replaceable without changing the upstream CMSIS-DAP command dispatcher.
 */
#ifndef RBLINK_VENDOR_H
#define RBLINK_VENDOR_H

#include <stdint.h>

#define ID_RBLINK_SPI 0x85U
#define ID_RBLINK_I2C 0x86U
#define ID_RBLINK_CAN 0x87U
#define ID_RBLINK_WIFI 0x88U
#define ID_RBLINK_STORAGE 0x89U
#define ID_RBLINK_POWER 0x8EU
#define ID_RBLINK_INFO 0x8FU

#define RBLINK_PROTOCOL_MAJOR 1U
#define RBLINK_PROTOCOL_MINOR 5U
#define RBLINK_FIRMWARE_MAJOR 3U
#define RBLINK_FIRMWARE_MINOR 0U
#define RBLINK_FIRMWARE_PATCH 5U
#define RBLINK_BOARD_REVISION 0U /* Set after the first PCB revision is frozen. */
#define RBLINK_CAP_SPI   (1U << 0)
#define RBLINK_CAP_I2C   (1U << 1)
#define RBLINK_CAP_CAN   (1U << 2)
#define RBLINK_CAP_POWER (1U << 3)
#define RBLINK_CAP_WIFI  (1U << 4)
#define RBLINK_CAP_STORAGE (1U << 6)

/* DAP_ProcessVendorCommand return format:
 * upper 16 bits = consumed request bytes, lower 16 bits = response bytes.
 */
uint32_t rblink_vendor_process(uint8_t command,
                               const uint8_t *request,
                               uint8_t *response);

#endif
