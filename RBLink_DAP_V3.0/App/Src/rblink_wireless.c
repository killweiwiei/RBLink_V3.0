#include "rblink_wireless.h"
#include "rblink_board.h"
#include "rblink_uart.h"
#include "DAP.h"
#include <string.h>

#define FRAME_MAGIC      0x4B4C4252UL
#define FRAME_VERSION    1U
#define FRAME_SIZE       512U
#define PAYLOAD_SIZE     492U
#define CH_CONTROL       0U
#define CH_DAP           1U
#define CH_UART          2U
#define CH_VENDOR        3U
#define CH_LOG           4U
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
static uint32_t control_sequence = 0x80000000UL;
static uint8_t wifi_staging[95];
static uint8_t wifi_ssid_length;
static uint8_t wifi_password_length;
static uint8_t wifi_staged_length;

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
  if (frame->payload_length <= PAYLOAD_SIZE) {
    memset(&frame->payload[frame->payload_length], 0,
           PAYLOAD_SIZE - frame->payload_length);
  }
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
      (frame->payload_length > PAYLOAD_SIZE) ||
      (frame->reserved0 != 0U) || (frame->reserved1 != 0U) ||
      ((frame->flags & ~(FLAG_RESPONSE | 0x02U | FLAG_ERROR)) != 0U) ||
      !((frame->channel <= CH_LOG) || (frame->channel == CH_IDLE))) return 0U;
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
  /* Late responses to a timed-out local control request must not be reflected
   * back to ESP as a new request. */
  if ((rx_frame.channel == CH_CONTROL) &&
      ((rx_frame.flags & FLAG_RESPONSE) != 0U)) return;

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
    count = RB_UART_Read(tx_frame.payload, PAYLOAD_SIZE);
    tx_frame.payload_length = count;
  } else {
    tx_frame.flags |= FLAG_ERROR;
    tx_frame.payload_length = 0U;
  }
  finalize(&tx_frame);
  response_pending = 1U;
}

uint8_t RB_Wireless_Control(const uint8_t *request, uint16_t request_length,
                            uint8_t *response, uint16_t *response_length)
{
  uint32_t sequence;
  uint32_t deadline;
  uint8_t request_sent = 0U;

  if ((request == NULL) || (response == NULL) || (response_length == NULL) ||
      (request_length == 0U) || (request_length > PAYLOAD_SIZE)) return 0U;
  sequence = control_sequence++;
  deadline = HAL_GetTick() + 500U;
  response_pending = 0U;

  while ((int32_t)(deadline - HAL_GetTick()) > 0) {
    if (HAL_GPIO_ReadPin(RB_ESP_READY_PORT, RB_ESP_READY_PIN) == GPIO_PIN_RESET) {
      HAL_Delay(1U);
      continue;
    }
    if (!request_sent) {
      memset(&tx_frame, 0, sizeof(tx_frame));
      tx_frame.channel = CH_CONTROL;
      tx_frame.sequence = sequence;
      tx_frame.payload_length = request_length;
      memcpy(tx_frame.payload, request, request_length);
      finalize(&tx_frame);
      request_sent = 1U;
    } else {
      make_idle();
    }
    memset(&rx_frame, 0, sizeof(rx_frame));
    HAL_GPIO_WritePin(RB_ESP_NSS_PORT, RB_ESP_NSS_PIN, GPIO_PIN_RESET);
    if (HAL_SPI_TransmitReceive(&hspi1, (uint8_t *)&tx_frame,
                                (uint8_t *)&rx_frame, FRAME_SIZE, 20U) == HAL_OK) {
      HAL_GPIO_WritePin(RB_ESP_NSS_PORT, RB_ESP_NSS_PIN, GPIO_PIN_SET);
      if (valid(&rx_frame) && (rx_frame.channel == CH_CONTROL) &&
          ((rx_frame.flags & FLAG_RESPONSE) != 0U) &&
          (rx_frame.sequence == sequence)) {
        memcpy(response, rx_frame.payload, rx_frame.payload_length);
        *response_length = rx_frame.payload_length;
        make_idle();
        return 1U;
      }
    } else {
      HAL_GPIO_WritePin(RB_ESP_NSS_PORT, RB_ESP_NSS_PIN, GPIO_PIN_SET);
    }
  }
  make_idle();
  return 0U;
}

uint8_t rblink_platform_wifi(const uint8_t *p, uint8_t n,
                             uint8_t *response, uint8_t *response_len)
{
  enum { ACTION_CONFIG = 0U, ACTION_TRANSFER = 1U, ACTION_CLOSE = 2U };
  enum { STAGE_BEGIN = 0U, STAGE_CHUNK = 1U, STAGE_COMMIT = 2U };
  uint8_t request[98];
  uint8_t esp_response[48];
  uint16_t esp_response_len = 0U;

  *response_len = 0U;
  if ((n == 1U) && (p[0] == ACTION_TRANSFER)) {
    request[0] = 0x10U;
  } else if ((n == 1U) && (p[0] == ACTION_CLOSE)) {
    request[0] = 0x12U;
  } else if ((n == 4U) && (p[0] == ACTION_CONFIG) &&
             (p[1] == STAGE_BEGIN)) {
    if ((p[2] == 0U) || (p[2] > 32U) ||
        !((p[3] == 0U) || ((p[3] >= 8U) && (p[3] <= 63U)))) return 1U;
    wifi_ssid_length = p[2];
    wifi_password_length = p[3];
    wifi_staged_length = 0U;
    memset(wifi_staging, 0, sizeof(wifi_staging));
    return 0U;
  } else if ((n >= 3U) && (p[0] == ACTION_CONFIG) &&
             (p[1] == STAGE_CHUNK)) {
    uint8_t chunk_length = (uint8_t)(n - 3U);
    uint8_t total = (uint8_t)(wifi_ssid_length + wifi_password_length);
    if ((p[2] != wifi_staged_length) ||
        ((uint16_t)p[2] + chunk_length > total)) return 1U;
    memcpy(&wifi_staging[p[2]], &p[3], chunk_length);
    wifi_staged_length = (uint8_t)(wifi_staged_length + chunk_length);
    return 0U;
  } else if ((n == 2U) && (p[0] == ACTION_CONFIG) &&
             (p[1] == STAGE_COMMIT)) {
    uint8_t total = (uint8_t)(wifi_ssid_length + wifi_password_length);
    if ((wifi_ssid_length == 0U) || (wifi_staged_length != total)) return 1U;
    request[0] = 0x11U;
    request[1] = wifi_ssid_length;
    request[2] = wifi_password_length;
    memcpy(&request[3], wifi_staging, total);
  } else {
    return 2U;
  }

  uint16_t request_length = request[0] == 0x11U ?
                            (uint16_t)(3U + wifi_ssid_length +
                                       wifi_password_length) : 1U;
  if (!RB_Wireless_Control(request, request_length, esp_response,
                           &esp_response_len) || (esp_response_len < 2U) ||
      (esp_response[0] != request[0]) || (esp_response[1] != 0U)) return 3U;
  if (request[0] == 0x10U) {
    if ((esp_response_len - 2U) > 56U) return 1U;
    *response_len = (uint8_t)(esp_response_len - 2U);
    memcpy(response, &esp_response[2], *response_len);
  } else if (request[0] == 0x11U) {
    wifi_ssid_length = 0U;
    wifi_password_length = 0U;
    wifi_staged_length = 0U;
  }
  return 0U;
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
