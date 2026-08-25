/*
 * LTLink V3 private CMSIS-DAP vendor commands.
 *
 * Commands 0x85..0x87 are unused by upstream DAPLink v0258.  Keeping the
 * protocol behind this small interface makes the STM32F407 peripheral driver
 * replaceable without changing the upstream CMSIS-DAP command dispatcher.
 */
#ifndef RBLINK_VENDOR_H
#define RBLINK_VENDOR_H

#include <stdint.h>

#define ID_LTLINK_SPI 0x85U
#define ID_LTLINK_I2C 0x86U
#define ID_LTLINK_CAN 0x87U
#define ID_LTLINK_POWER 0x8EU
#define ID_LTLINK_INFO 0x8FU

#define LTLINK_PROTOCOL_MAJOR 1U
#define LTLINK_PROTOCOL_MINOR 2U
#define LTLINK_CAP_SPI   (1U << 0)
#define LTLINK_CAP_I2C   (1U << 1)
#define LTLINK_CAP_CAN   (1U << 2)
#define LTLINK_CAP_POWER (1U << 3)

/* DAP_ProcessVendorCommand return format:
 * upper 16 bits = consumed request bytes, lower 16 bits = response bytes.
 */
uint32_t ltlink_vendor_process(uint8_t command,
                               const uint8_t *request,
                               uint8_t *response);

#endif
