#include "rblink_nor.h"
#include "rblink_board.h"
#include <string.h>

#define NOR_TIMEOUT_MS  100U
#define NOR_W25Q32_SIZE 0x00400000UL
static SPI_HandleTypeDef hspi3;
static uint8_t nor_initialized;
static uint32_t nor_jedec;
static uint32_t nor_capacity;

static uint8_t range_valid(uint32_t address, uint32_t length)
{
  return nor_initialized && (nor_capacity != 0U) &&
         (address <= nor_capacity) && (length <= (nor_capacity - address));
}

static void select_nor(uint8_t selected)
{
  HAL_GPIO_WritePin(RB_FLASH_NSS_PORT, RB_FLASH_NSS_PIN,
                    selected ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static uint8_t command(uint8_t opcode)
{
  return HAL_SPI_Transmit(&hspi3, &opcode, 1U, NOR_TIMEOUT_MS) == HAL_OK;
}

static uint8_t write_enable(void)
{
  uint8_t ok;
  select_nor(1U); ok = command(0x06U); select_nor(0U);
  return ok;
}

static uint8_t wait_ready(uint32_t timeout)
{
  uint32_t start = HAL_GetTick();
  uint8_t tx[2] = {0x05U, 0xFFU}, rx[2];
  do {
    select_nor(1U);
    if (HAL_SPI_TransmitReceive(&hspi3, tx, rx, 2U, NOR_TIMEOUT_MS) != HAL_OK) {
      select_nor(0U); return 0U;
    }
    select_nor(0U);
    if ((rx[1] & 1U) == 0U) return 1U;
  } while ((HAL_GetTick() - start) < timeout);
  return 0U;
}

void RB_NOR_Init(void)
{
  GPIO_InitTypeDef io = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_SPI3_CLK_ENABLE();
  io.Pin = RB_FLASH_SPI_PINS;
  io.Mode = GPIO_MODE_AF_PP; io.Pull = GPIO_NOPULL;
  io.Speed = GPIO_SPEED_FREQ_VERY_HIGH; io.Alternate = GPIO_AF6_SPI3;
  HAL_GPIO_Init(RB_FLASH_SPI_PORT, &io);
  memset(&hspi3, 0, sizeof(hspi3));
  hspi3.Instance = SPI3; hspi3.Init.Mode = SPI_MODE_MASTER;
  hspi3.Init.Direction = SPI_DIRECTION_2LINES; hspi3.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi3.Init.CLKPolarity = SPI_POLARITY_LOW; hspi3.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi3.Init.NSS = SPI_NSS_SOFT; hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB; hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE; hspi3.Init.CRCPolynomial = 7U;
  nor_initialized = HAL_SPI_Init(&hspi3) == HAL_OK ? 1U : 0U;
  select_nor(0U);
  nor_jedec = RB_NOR_ReadJEDEC();
  /* W25Q32 uses JEDEC density code 0x16 (2^22 bytes).  Accept compatible
     vendors with the same density, but never expose a guessed capacity. */
  nor_capacity = ((nor_jedec & 0xFFU) == 0x16U) ? NOR_W25Q32_SIZE : 0U;
}

uint8_t RB_NOR_IsReady(void) { return (uint8_t)(nor_initialized && (nor_capacity != 0U)); }

uint32_t RB_NOR_ReadJEDEC(void)
{
  uint8_t tx[4] = {0x9FU,0xFFU,0xFFU,0xFFU}, rx[4] = {0};
  if (!nor_initialized) return 0U;
  select_nor(1U); (void)HAL_SPI_TransmitReceive(&hspi3,tx,rx,4U,NOR_TIMEOUT_MS); select_nor(0U);
  return ((uint32_t)rx[1] << 16U) | ((uint32_t)rx[2] << 8U) | rx[3];
}

uint32_t RB_NOR_Capacity(void) { return nor_capacity; }

uint8_t RB_NOR_Read(uint32_t address, uint8_t *data, uint16_t length)
{
  uint8_t header[4] = {0x03U,(uint8_t)(address>>16U),(uint8_t)(address>>8U),(uint8_t)address};
  if (!range_valid(address, length) ||
      ((data == NULL) && (length != 0U))) return 0U;
  if (length == 0U) return 1U;
  select_nor(1U);
  if ((HAL_SPI_Transmit(&hspi3,header,4U,NOR_TIMEOUT_MS)!=HAL_OK) ||
      (HAL_SPI_Receive(&hspi3,data,length,NOR_TIMEOUT_MS)!=HAL_OK)) {select_nor(0U);return 0U;}
  select_nor(0U); return 1U;
}

uint8_t RB_NOR_PageProgram(uint32_t address, const uint8_t *data, uint16_t length)
{
  uint8_t header[4] = {0x02U,(uint8_t)(address>>16U),(uint8_t)(address>>8U),(uint8_t)address};
  if (!range_valid(address, length) || (data == NULL) ||
      (length==0U)||(length>256U)||(((address&0xFFU)+length)>256U)||!write_enable()) return 0U;
  select_nor(1U);
  if ((HAL_SPI_Transmit(&hspi3,header,4U,NOR_TIMEOUT_MS)!=HAL_OK) ||
      (HAL_SPI_Transmit(&hspi3,(uint8_t *)data,length,NOR_TIMEOUT_MS)!=HAL_OK)) {select_nor(0U);return 0U;}
  select_nor(0U); return wait_ready(500U);
}

uint8_t RB_NOR_Program(uint32_t address, const uint8_t *data, uint16_t length)
{
  uint16_t chunk;
  if ((data == NULL) || (length == 0U) || !range_valid(address, length)) return 0U;
  while (length != 0U) {
    chunk = (uint16_t)(256U - (address & 0xFFU));
    if (chunk > length) chunk = length;
    if (!RB_NOR_PageProgram(address, data, chunk)) return 0U;
    address += chunk; data += chunk; length = (uint16_t)(length - chunk);
  }
  return 1U;
}

uint8_t RB_NOR_Erase4K(uint32_t address)
{
  uint8_t header[4] = {0x20U,(uint8_t)(address>>16U),(uint8_t)(address>>8U),(uint8_t)address};
  address &= ~0xFFFUL;
  if (!range_valid(address, 4096U) || !write_enable()) return 0U;
  select_nor(1U); if(HAL_SPI_Transmit(&hspi3,header,4U,NOR_TIMEOUT_MS)!=HAL_OK){select_nor(0U);return 0U;}
  select_nor(0U); return wait_ready(3000U);
}
