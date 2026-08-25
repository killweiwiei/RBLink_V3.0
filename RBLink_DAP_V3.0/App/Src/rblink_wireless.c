#include "rblink_wireless.h"
#include "rblink_board.h"
#include "rblink_uart.h"
#include "DAP.h"
#include <string.h>

#define FRAME_MAGIC      0x4B4C4252UL
#define FRAME_VERSION    1U
#define FRAME_SIZE       512U
#define PAYLOAD_SIZE     492U
#define CH_DAP           1U
#define CH_UART          2U
#define CH_VENDOR        3U
#define CH_IDLE          0xFFU
#define FLAG_RESPONSE    0x01U
#define FLAG_ERROR       0x04U

typedef __packed struct {
  uint32_t magic;
  uint8_t version, channel, flags, reserved0;
  uint32_t sequence;
  uint16_t payload_length, reserved1;
  uint32_t crc32;
  uint8_t payload[PAYLOAD_SIZE];
} rb_frame_t;

typedef char rb_frame_size_must_be_512[(sizeof(rb_frame_t) == FRAME_SIZE) ? 1 : -1];

static SPI_HandleTypeDef hspi1;
static rb_frame_t tx_frame __attribute__((aligned(4)));
static rb_frame_t rx_frame __attribute__((aligned(4)));
static uint8_t response_pending;
static uint32_t idle_sequence;

static uint32_t crc32(const void *memory, uint32_t length)
{
  const uint8_t *data = memory;
  uint32_t crc = 0xFFFFFFFFUL;
  while (length-- != 0U) {
    uint32_t bit;
    crc ^= *data++;
    for (bit = 0U; bit < 8U; ++bit) {
      uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

static void finalize(rb_frame_t *frame)
{
  frame->magic = FRAME_MAGIC;
  frame->version = FRAME_VERSION;
  frame->reserved0 = 0U;
  frame->reserved1 = 0U;
  frame->crc32 = 0U;
  frame->crc32 = crc32(frame, sizeof(*frame));
}

static uint8_t valid(const rb_frame_t *frame)
{
  rb_frame_t copy;
  uint32_t expected;
  if ((frame->magic != FRAME_MAGIC) || (frame->version != FRAME_VERSION) ||
      (frame->payload_length > PAYLOAD_SIZE)) return 0U;
  memcpy(&copy, frame, sizeof(copy));
  expected = copy.crc32;
  copy.crc32 = 0U;
  return crc32(&copy, sizeof(copy)) == expected;
}

static void make_idle(void)
{
  memset(&tx_frame, 0, sizeof(tx_frame));
  tx_frame.channel = CH_IDLE;
  tx_frame.sequence = idle_sequence++;
  finalize(&tx_frame);
}

static void process_frame(void)
{
  uint32_t result = 0U;
  uint16_t count = 0U;
  if (!valid(&rx_frame) || (rx_frame.channel == CH_IDLE)) return;

  memset(&tx_frame, 0, sizeof(tx_frame));
  tx_frame.channel = rx_frame.channel;
  tx_frame.sequence = rx_frame.sequence;
  tx_frame.flags = FLAG_RESPONSE;

  if (((rx_frame.channel == CH_DAP) || (rx_frame.channel == CH_VENDOR)) &&
      (rx_frame.payload_length >= 1U) && (rx_frame.payload_length <= 64U)) {
    result = DAP_ExecuteCommand(rx_frame.payload, tx_frame.payload);
    tx_frame.payload_length = (uint16_t)(result & 0xFFFFU);
  } else if (rx_frame.channel == CH_UART) {
    (void)RB_UART_Write(rx_frame.payload, rx_frame.payload_length);
    count = RB_UART_Read(tx_frame.payload, 64U);
    tx_frame.payload_length = count;
  } else {
    tx_frame.flags |= FLAG_ERROR;
    tx_frame.payload_length = 0U;
  }
  finalize(&tx_frame);
  response_pending = 1U;
}

void RB_Wireless_Init(void)
{
  GPIO_InitTypeDef io = {0};
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_SPI1_CLK_ENABLE();
  io.Pin = RB_ESP_SPI_PINS;
  io.Mode = GPIO_MODE_AF_PP;
  io.Pull = GPIO_NOPULL;
  io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  io.Alternate = GPIO_AF5_SPI1;
  HAL_GPIO_Init(RB_ESP_SPI_PORT, &io);

  memset(&hspi1, 0, sizeof(hspi1));
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8; /* 10.5 MHz bring-up; raise after SI test. */
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7U;
  (void)HAL_SPI_Init(&hspi1);
  make_idle();
  RB_ESP_EN_PORT->BSRR = RB_ESP_EN_PIN;
}

void RB_Wireless_Task(void)
{
  if (HAL_GPIO_ReadPin(RB_ESP_READY_PORT, RB_ESP_READY_PIN) == GPIO_PIN_RESET) return;
  if (!response_pending) make_idle();
  memset(&rx_frame, 0, sizeof(rx_frame));
  HAL_GPIO_WritePin(RB_ESP_NSS_PORT, RB_ESP_NSS_PIN, GPIO_PIN_RESET);
  if (HAL_SPI_TransmitReceive(&hspi1, (uint8_t *)&tx_frame, (uint8_t *)&rx_frame,
                              FRAME_SIZE, 5U) == HAL_OK) {
    response_pending = 0U;
    process_frame();
  }
  HAL_GPIO_WritePin(RB_ESP_NSS_PORT, RB_ESP_NSS_PIN, GPIO_PIN_SET);
}
