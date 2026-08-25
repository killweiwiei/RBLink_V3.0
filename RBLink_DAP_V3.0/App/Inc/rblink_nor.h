#ifndef RBLINK_NOR_H
#define RBLINK_NOR_H

#include <stdint.h>

void RB_NOR_Init(void);
uint32_t RB_NOR_ReadJEDEC(void);
uint8_t RB_NOR_Read(uint32_t address, uint8_t *data, uint16_t length);
uint8_t RB_NOR_PageProgram(uint32_t address, const uint8_t *data, uint16_t length);
uint8_t RB_NOR_Erase4K(uint32_t address);

#endif
