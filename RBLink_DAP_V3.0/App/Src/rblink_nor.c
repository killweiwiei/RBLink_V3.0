#include "rblink_nor.h"
#include "rblink_board.h"
#include <string.h>

#define NOR_LAYOUT_MIN_SIZE 0x00400000UL
static SPI_HandleTypeDef hspi3;
static uint8_t nor_initialized;
static uint8_t nor_software_spi;
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

static uint8_t software_transfer_byte(uint8_t output)
{
  uint8_t input = 0U;
  uint8_t mask;
  for (mask = 0x80U; mask != 0U; mask >>= 1U) {
    HAL_GPIO_WritePin(RB_FLASH_SPI_PORT, RB_FLASH_MOSI_PIN,
                      (output & mask) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    __NOP();
    HAL_GPIO_WritePin(RB_FLASH_SPI_PORT, RB_FLASH_SCK_PIN, GPIO_PIN_SET);
    __NOP();
    input <<= 1U;
    if (HAL_GPIO_ReadPin(RB_FLASH_SPI_PORT, RB_FLASH_MISO_PIN) == GPIO_PIN_SET) input |= 1U;
    HAL_GPIO_WritePin(RB_FLASH_SPI_PORT, RB_FLASH_SCK_PIN, GPIO_PIN_RESET);
    __NOP();
  }
  return input;
}

static uint8_t transfer_byte(uint8_t output)
{
  uint8_t input = 0xFFU;
  if (nor_software_spi) return software_transfer_byte(output);
  if (HAL_SPI_TransmitReceive(&hspi3, &output, &input, 1U, 100U) != HAL_OK) return 0xFFU;
  return input;
}

static void transmit(const uint8_t *data, uint16_t length)
{
  while (length-- != 0U) (void)transfer_byte(*data++);
}

static void receive(uint8_t *data, uint16_t length)
{
  while (length-- != 0U) *data++ = transfer_byte(0xFFU);
}

static void command(uint8_t opcode)
{
  (void)transfer_byte(opcode);
}

static uint32_t capacity_from_jedec(uint32_t jedec)
{
  uint8_t manufacturer = (uint8_t)(jedec >> 16U);
  uint8_t memory_type = (uint8_t)(jedec >> 8U);
  uint8_t density = (uint8_t)jedec;
  if ((manufacturer == 0U) || (manufacturer == 0xFFU) ||
      (memory_type == 0U) || (memory_type == 0xFFU) ||
      (density < 16U) || (density > 31U)) return 0U;
  return 1UL << density;
}

static uint8_t configure_hardware_spi(void)
{
  GPIO_InitTypeDef io = {0};
  io.Pin = RB_FLASH_SPI_PINS;
  io.Mode = GPIO_MODE_AF_PP; io.Pull = GPIO_NOPULL;
  io.Speed = GPIO_SPEED_FREQ_VERY_HIGH; io.Alternate = GPIO_AF6_SPI3;
  HAL_GPIO_Init(RB_FLASH_SPI_PORT, &io);
  memset(&hspi3, 0, sizeof(hspi3));
  hspi3.Instance = SPI3; hspi3.Init.Mode = SPI_MODE_MASTER;
  hspi3.Init.Direction = SPI_DIRECTION_2LINES; hspi3.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi3.Init.CLKPolarity = SPI_POLARITY_LOW; hspi3.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi3.Init.NSS = SPI_NSS_SOFT; hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB; hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE; hspi3.Init.CRCPolynomial = 7U;
  nor_software_spi = 0U;
  return HAL_SPI_Init(&hspi3) == HAL_OK ? 1U : 0U;
}

static void configure_software_spi(void)
{
  GPIO_InitTypeDef io = {0};
  (void)HAL_SPI_DeInit(&hspi3);
  io.Pin = RB_FLASH_MOSI_PIN | RB_FLASH_SCK_PIN;
  io.Mode = GPIO_MODE_OUTPUT_PP; io.Pull = GPIO_NOPULL;
  io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(RB_FLASH_SPI_PORT, &io);
  io.Pin = RB_FLASH_MISO_PIN;
  io.Mode = GPIO_MODE_INPUT; io.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(RB_FLASH_SPI_PORT, &io);
  HAL_GPIO_WritePin(RB_FLASH_SPI_PORT, RB_FLASH_SCK_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RB_FLASH_SPI_PORT, RB_FLASH_MOSI_PIN, GPIO_PIN_SET);
  nor_software_spi = 1U;
}

static uint32_t wake_and_read_jedec(void)
{
  select_nor(0U);
  select_nor(1U); command(0xABU); select_nor(0U); HAL_Delay(1U);
  return RB_NOR_ReadJEDEC();
}

static uint8_t write_enable(void)
{
  select_nor(1U); command(0x06U); select_nor(0U);
  return 1U;
}

uint8_t RB_NOR_IsBusy(void)
{
  uint8_t status;
  if (!RB_NOR_IsReady()) return 1U;
  select_nor(1U); command(0x05U); status = transfer_byte(0xFFU); select_nor(0U);
  return (status & 1U) != 0U ? 1U : 0U;
}

static uint8_t wait_ready(uint32_t timeout)
{
  uint32_t start = HAL_GetTick();
  do {
    if (!RB_NOR_IsBusy()) return 1U;
  } while ((HAL_GetTick() - start) < timeout);
  return 0U;
}

void RB_NOR_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_SPI3_CLK_ENABLE();
  nor_initialized = configure_hardware_spi();
  nor_jedec = nor_initialized ? wake_and_read_jedec() : 0U;
  /* Some PCB revisions route PC10=MOSI and PC12=SCK, opposite to SPI3's
     fixed PC10=SCK/PC12=MOSI mapping. Fall back to GPIO bit-banging when the
     native peripheral receives no plausible JEDEC identity. */
  if (capacity_from_jedec(nor_jedec) < NOR_LAYOUT_MIN_SIZE) {
    configure_software_spi();
    nor_initialized = 1U;
    nor_jedec = wake_and_read_jedec();
  }
  /* JESD216-compatible density codes 0x10..0x1F encode capacity in bytes as
     2^N. The fixed RBLink layout needs at least 4 MiB; larger parts work too. */
  nor_capacity = capacity_from_jedec(nor_jedec);
  if (nor_capacity < NOR_LAYOUT_MIN_SIZE) nor_capacity = 0U;
}

uint8_t RB_NOR_IsReady(void) { return (uint8_t)(nor_initialized && (nor_capacity != 0U)); }

uint32_t RB_NOR_ReadJEDEC(void)
{
  uint8_t id[3];
  if (!nor_initialized) return 0U;
  select_nor(1U); command(0x9FU); receive(id, 3U); select_nor(0U);
  return ((uint32_t)id[0] << 16U) | ((uint32_t)id[1] << 8U) | id[2];
}

uint32_t RB_NOR_Capacity(void) { return nor_capacity; }

uint8_t RB_NOR_Read(uint32_t address, uint8_t *data, uint16_t length)
{
  uint8_t header[4] = {0x03U,(uint8_t)(address>>16U),(uint8_t)(address>>8U),(uint8_t)address};
  if (!range_valid(address, length) ||
      ((data == NULL) && (length != 0U))) return 0U;
  if (length == 0U) return 1U;
  select_nor(1U);
  transmit(header, 4U);
  receive(data, length);
  select_nor(0U); return 1U;
}

uint8_t RB_NOR_PageProgram(uint32_t address, const uint8_t *data, uint16_t length)
{
  uint8_t header[4] = {0x02U,(uint8_t)(address>>16U),(uint8_t)(address>>8U),(uint8_t)address};
  if (!range_valid(address, length) || (data == NULL) ||
      (length==0U)||(length>256U)||(((address&0xFFU)+length)>256U)||!write_enable()) return 0U;
  select_nor(1U);
  transmit(header, 4U);
  transmit(data, length);
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

uint8_t RB_NOR_Erase4KStart(uint32_t address)
{
  uint8_t header[4];
  address &= ~0xFFFUL;
  if (!range_valid(address, 4096U) || !write_enable()) return 0U;
  header[0] = 0x20U; header[1] = (uint8_t)(address >> 16U);
  header[2] = (uint8_t)(address >> 8U); header[3] = (uint8_t)address;
  select_nor(1U); transmit(header, 4U);
  select_nor(0U); return 1U;
}

uint8_t RB_NOR_Erase4K(uint32_t address)
{
  return RB_NOR_Erase4KStart(address) && wait_ready(3000U);
}
