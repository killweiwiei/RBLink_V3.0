#include "rblink_uart.h"
#include "rblink_board.h"

#define RB_UART_RING_SIZE 2048U
#define RB_UART_RING_MASK (RB_UART_RING_SIZE - 1U)

static volatile uint16_t tx_head, tx_tail, rx_head, rx_tail;
static uint8_t tx_ring[RB_UART_RING_SIZE];
static uint8_t rx_ring[RB_UART_RING_SIZE];
static UART_HandleTypeDef huart1;

static void apply_line_coding(uint32_t baud, uint8_t stop, uint8_t parity, uint8_t bits)
{
  if ((baud == 0U) || ((bits != 7U) && (bits != 8U)) ||
      ((bits == 7U) && (parity == 0U)) || (parity > 2U)) {
    return;
  }
  huart1.Instance = USART1;
  huart1.Init.BaudRate = baud;
  huart1.Init.StopBits = (stop == 2U) ? UART_STOPBITS_2 : UART_STOPBITS_1;
  huart1.Init.Parity = (parity == 1U) ? UART_PARITY_ODD :
                      (parity == 2U) ? UART_PARITY_EVEN : UART_PARITY_NONE;
  /* On STM32F4 the parity bit is included in WordLength: 7 data + parity
     uses 8B, while 8 data + parity uses 9B. */
  huart1.Init.WordLength = ((bits == 8U) && (huart1.Init.Parity != UART_PARITY_NONE)) ?
                          UART_WORDLENGTH_9B : UART_WORDLENGTH_8B;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  USART1->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_TXEIE);
  if (HAL_UART_Init(&huart1) != HAL_OK) return;
  USART1->CR1 |= USART_CR1_RXNEIE;
  /* A SET_LINE_CODING request can race with queued CDC data. Never leave a
     non-empty transmit ring stalled after HAL_UART_Init rewrites CR1. */
  if (tx_head != tx_tail) USART1->CR1 |= USART_CR1_TXEIE;
}

void RB_UART_Init(void)
{
  GPIO_InitTypeDef io = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_USART1_CLK_ENABLE();
  io.Pin = RB_UART_TX_PIN | RB_UART_RX_PIN;
  io.Mode = GPIO_MODE_AF_PP;
  io.Pull = GPIO_PULLUP;
  io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  io.Alternate = GPIO_AF7_USART1;
  HAL_GPIO_Init(GPIOA, &io);
  tx_head = tx_tail = rx_head = rx_tail = 0U;
  huart1.Instance = 0;
  apply_line_coding(115200U, 0U, 0U, 8U);
  HAL_NVIC_SetPriority(USART1_IRQn, 6U, 0U);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  /* The target-side translator is active-high.  Keep it enabled by default:
     Web Serial and some terminal programs do not assert DTR when opening CDC,
     and tying PORTEN to DTR made USART1 transmit internally with no waveform
     reaching PA9/the connector. */
  RB_UART_Enable(1U);
}

void RB_UART_Task(void)
{
  /* Low-cost recovery for a line-coding/USB interrupt race. This also makes
     a stalled transmitter observable and recoverable without resetting the
     whole probe. */
  if ((huart1.Instance == USART1) && (tx_head != tx_tail) &&
      ((USART1->CR1 & USART_CR1_TXEIE) == 0U)) {
    USART1->CR1 |= USART_CR1_TXEIE;
  }
}

void RB_UART_SetLineCoding(uint32_t baud, uint8_t stop, uint8_t parity, uint8_t bits)
{
  apply_line_coding(baud, stop, parity, bits);
}

void RB_UART_SetControlLines(uint16_t state)
{
  (void)state;
  RB_UART_Enable(1U);
}

uint16_t RB_UART_Write(const uint8_t *data, uint16_t length)
{
  uint16_t count = 0U;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  while (count < length) {
    uint16_t next = (tx_head + 1U) & RB_UART_RING_MASK;
    if (next == tx_tail) break;
    tx_ring[tx_head] = data[count++];
    tx_head = next;
  }
  if (count != 0U) USART1->CR1 |= USART_CR1_TXEIE;
  if (primask == 0U) __enable_irq();
  return count;
}

uint16_t RB_UART_Read(uint8_t *data, uint16_t capacity)
{
  uint16_t count = 0U;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  while ((count < capacity) && (rx_tail != rx_head)) {
    data[count++] = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1U) & RB_UART_RING_MASK;
  }
  if (primask == 0U) __enable_irq();
  return count;
}

void USART1_IRQHandler(void)
{
  uint32_t sr = USART1->SR;
  if ((sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_NE | USART_SR_FE)) != 0U) {
    uint8_t value = (uint8_t)USART1->DR;
    uint16_t next = (rx_head + 1U) & RB_UART_RING_MASK;
    if (next != rx_tail) {
      rx_ring[rx_head] = value;
      rx_head = next;
    }
  }
  if (((sr & USART_SR_TXE) != 0U) && ((USART1->CR1 & USART_CR1_TXEIE) != 0U)) {
    if (tx_tail != tx_head) {
      USART1->DR = tx_ring[tx_tail];
      tx_tail = (tx_tail + 1U) & RB_UART_RING_MASK;
    } else {
      USART1->CR1 &= ~USART_CR1_TXEIE;
    }
  }
}
