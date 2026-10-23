#include "rblink_debug.h"
#include "rblink_board.h"
#include "rblink_version.h"

#if (RBLINK_DEBUG_ENABLE != 0U)

#define RB_DEBUG_BAUD        115200U
#define RB_SWD_TRACE_DEPTH   16U
#define RB_SWD_TRACE_MASK    (RB_SWD_TRACE_DEPTH - 1U)
#define RB_DEBUG_TX_BUDGET   8U

typedef struct {
  uint8_t source;
  uint8_t request;
  uint8_t ack;
  uint8_t kind;
  uint32_t value;
  uint32_t value_alt;
} RB_SWD_TraceRecord;

static UART_HandleTypeDef debug_uart;
static RB_SWD_TraceRecord trace_ring[RB_SWD_TRACE_DEPTH];
static volatile uint8_t trace_head;
static volatile uint8_t trace_tail;
static uint8_t tx_line[64];
static uint8_t tx_length;
static uint8_t tx_index;

static uint8_t hex_digit(uint32_t value)
{
  value &= 0x0FU;
  return (uint8_t)((value < 10U) ? ('0' + value) : ('A' + value - 10U));
}

static uint8_t format_trace(uint8_t *out, const RB_SWD_TraceRecord *record)
{
  uint8_t n = 0U;
  int32_t shift;
  if (record->source == 1U) {
    out[n++] = 'J'; out[n++] = 'T'; out[n++] = 'A'; out[n++] = 'G'; out[n++] = ' ';
    out[n++] = 'P'; out[n++] = 'D'; out[n++] = '1'; out[n++] = '0'; out[n++] = '=';
    for (shift = 28; shift >= 0; shift -= 4) {
      out[n++] = hex_digit(record->value >> (uint32_t)shift);
    }
    out[n++] = ' ';
    out[n++] = 'P'; out[n++] = 'D'; out[n++] = '9'; out[n++] = '=';
    for (shift = 28; shift >= 0; shift -= 4) {
      out[n++] = hex_digit(record->value_alt >> (uint32_t)shift);
    }
    out[n++] = ' ';
    out[n++] = 'O'; out[n++] = 'E'; out[n++] = '=';
    out[n++] = (uint8_t)('0' + ((record->request >> 0U) & 1U));
    out[n++] = ' ';
    out[n++] = 'D'; out[n++] = 'I'; out[n++] = 'R'; out[n++] = '=';
    out[n++] = (uint8_t)('0' + ((record->request >> 1U) & 1U));
    out[n++] = '\r'; out[n++] = '\n';
    return n;
  }
  if (record->source == 2U) {
    out[n++] = 'J'; out[n++] = 'T'; out[n++] = 'A'; out[n++] = 'G'; out[n++] = ' ';
    out[n++] = record->kind; out[n++] = ' ';
    out[n++] = 'R'; out[n++] = '=';
    out[n++] = hex_digit(record->request >> 4U);
    out[n++] = hex_digit(record->request);
    out[n++] = ' ';
    out[n++] = 'A'; out[n++] = '=';
    out[n++] = (uint8_t)('0' + ((record->ack >> 0U) & 1U));
    out[n++] = (uint8_t)('0' + ((record->ack >> 1U) & 1U));
    out[n++] = (uint8_t)('0' + ((record->ack >> 2U) & 1U));
    out[n++] = '/'; out[n++] = hex_digit(record->ack);
    out[n++] = ' ';
    out[n++] = 'D'; out[n++] = '=';
    for (shift = 28; shift >= 0; shift -= 4) {
      out[n++] = hex_digit(record->value >> (uint32_t)shift);
    }
    out[n++] = '\r'; out[n++] = '\n';
    return n;
  }
  out[n++] = 'S'; out[n++] = 'W'; out[n++] = 'D'; out[n++] = ' ';
  out[n++] = record->kind; out[n++] = ' ';
  out[n++] = 'R'; out[n++] = '=';
  out[n++] = hex_digit(record->request >> 4U);
  out[n++] = hex_digit(record->request);
  out[n++] = ' ';
  out[n++] = 'A'; out[n++] = '=';
  out[n++] = (uint8_t)('0' + ((record->ack >> 0U) & 1U));
  out[n++] = (uint8_t)('0' + ((record->ack >> 1U) & 1U));
  out[n++] = (uint8_t)('0' + ((record->ack >> 2U) & 1U));
  out[n++] = '/'; out[n++] = hex_digit(record->ack);
  out[n++] = ' ';
  out[n++] = 'D'; out[n++] = '=';
  for (shift = 28; shift >= 0; shift -= 4) {
    out[n++] = hex_digit(record->value >> (uint32_t)shift);
  }
  out[n++] = '\r'; out[n++] = '\n';
  return n;
}

void RB_Debug_Init(void)
{
  GPIO_InitTypeDef gpio = {0};
  static const uint8_t boot_info[] =
      "\r\n"
      "================================\r\n"
      " RBLink V3 diagnostic console\r\n"
      " Firmware : " RBLINK_FIRMWARE_STRING "\r\n"
      " MCU      : STM32F407VGT6\r\n"
      " UART     : USART3 115200 8N1\r\n"
      " Pins     : PB10=TX PB11=RX\r\n"
      " TX test  : UUUUUUUUUUUUUUUU\r\n"
      " Status   : early boot OK\r\n"
      "================================\r\n";
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_USART3_CLK_ENABLE();

  gpio.Pin = RB_DEBUG_TX_PIN | RB_DEBUG_RX_PIN;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF7_USART3;
  HAL_GPIO_Init(RB_DEBUG_PORT, &gpio);

  debug_uart.Instance = USART3;
  debug_uart.Init.BaudRate = RB_DEBUG_BAUD;
  debug_uart.Init.WordLength = UART_WORDLENGTH_8B;
  debug_uart.Init.StopBits = UART_STOPBITS_1;
  debug_uart.Init.Parity = UART_PARITY_NONE;
  debug_uart.Init.Mode = UART_MODE_TX_RX;
  debug_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  debug_uart.Init.OverSampling = UART_OVERSAMPLING_16;
  (void)HAL_UART_Init(&debug_uart);
  trace_head = trace_tail = 0U;
  /* Send the startup marker immediately. This also proves that USART3 and
     PB10 are alive even if a later subsystem prevents the cooperative task
     loop from draining the diagnostic queue. */
  (void)HAL_UART_Transmit(&debug_uart, (uint8_t *)boot_info,
                          (uint16_t)(sizeof(boot_info) - 1U), 100U);
  tx_length = 0U;
  tx_index = 0U;
}

void RB_Debug_SWDTrace(uint8_t request, uint8_t ack, uint8_t kind, uint32_t value)
{
  uint8_t next = (uint8_t)((trace_head + 1U) & RB_SWD_TRACE_MASK);
  if (next == trace_tail) return;
  trace_ring[trace_head].source = 0U;
  trace_ring[trace_head].request = request;
  trace_ring[trace_head].ack = ack;
  trace_ring[trace_head].kind = kind;
  trace_ring[trace_head].value = value;
  trace_ring[trace_head].value_alt = 0U;
  trace_head = next;
}

void RB_Debug_JTAGTrace(uint32_t primary, uint32_t alternate)
{
  RB_SWD_TraceRecord record;
  uint8_t line[64];
  uint8_t length;
  uint8_t pins = 0U;
  if ((RB_JTAG_EN_PORT->ODR & RB_JTAG_EN_PIN) != 0U) pins |= 1U;
  if ((RB_SWDIO_DIR_PORT->ODR & RB_SWDIO_DIR_PIN) != 0U) pins |= 2U;
  record.source = 1U;
  record.request = pins;
  record.ack = 0U;
  record.kind = 'I';
  record.value = primary;
  record.value_alt = alternate;
  length = format_trace(line, &record);
  /* IDCODE scans are infrequent. Sending only after the TAP has returned to
     Run-Test/Idle cannot disturb JTAG edge timing and guarantees visibility
     even when the normal background queue is busy. */
  (void)HAL_UART_Transmit(&debug_uart, line, length, 20U);
}

void RB_Debug_JTAGTransferTrace(uint8_t request, uint8_t ack, uint8_t phase,
                               uint32_t value)
{
  uint8_t next = (uint8_t)((trace_head + 1U) & RB_SWD_TRACE_MASK);
  if (next == trace_tail) return;
  trace_ring[trace_head].source = 2U;
  trace_ring[trace_head].request = request;
  trace_ring[trace_head].ack = ack;
  trace_ring[trace_head].kind = phase;
  trace_ring[trace_head].value = value;
  trace_ring[trace_head].value_alt = 0U;
  trace_head = next;
}

void RB_Debug_Task(void)
{
  uint8_t budget = RB_DEBUG_TX_BUDGET;
  if ((tx_index == tx_length) && (trace_tail != trace_head)) {
    RB_SWD_TraceRecord record = trace_ring[trace_tail];
    trace_tail = (uint8_t)((trace_tail + 1U) & RB_SWD_TRACE_MASK);
    tx_length = format_trace(tx_line, &record);
    tx_index = 0U;
  }
  while ((budget-- != 0U) && (tx_index < tx_length) &&
         ((USART3->SR & USART_SR_TXE) != 0U)) {
    USART3->DR = tx_line[tx_index++];
  }
}

#endif /* RBLINK_DEBUG_ENABLE */
