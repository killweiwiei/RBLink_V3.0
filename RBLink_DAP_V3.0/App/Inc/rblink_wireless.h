#ifndef RBLINK_WIRELESS_H
#define RBLINK_WIRELESS_H

#include <stdint.h>

void RB_Wireless_Init(void);
void RB_Wireless_Task(void);
uint8_t RB_Wireless_Control(const uint8_t *request, uint16_t request_length,
                            uint8_t *response, uint16_t *response_length);

#endif
