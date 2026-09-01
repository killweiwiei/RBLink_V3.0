#ifndef RBLINK_UART_H
#define RBLINK_UART_H

#include <stdint.h>

void RB_UART_Init(void);
void RB_UART_Task(void);
void RB_UART_SetLineCoding(uint32_t baud, uint8_t stop, uint8_t parity, uint8_t bits);
void RB_UART_SetControlLines(uint16_t state);
uint16_t RB_UART_Write(const uint8_t *data, uint16_t length);
uint16_t RB_UART_Read(uint8_t *data, uint16_t capacity);

#endif
