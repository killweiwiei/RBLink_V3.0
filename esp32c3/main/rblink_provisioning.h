#ifndef RBLINK_PROVISIONING_H
#define RBLINK_PROVISIONING_H

#include <stdbool.h>
#include <stdint.h>

typedef bool (*rblink_web_debug_exchange_t)(const uint8_t *request,
                                            uint8_t *response);

/* Starts AP+STA Wi-Fi, the local provisioning web page, and the USB CDC
 * provisioning console. NVS must be initialized before this call. */
void rblink_provisioning_init(rblink_web_debug_exchange_t debug_exchange);

bool rblink_provisioning_sta_connected(void);
bool rblink_provisioning_ap_active(void);

/* Handles control-channel commands received from STM32 over SPI. */
bool rblink_provisioning_command(const uint8_t *request, uint16_t request_length,
                                 uint8_t *response, uint16_t *response_length);

#endif
