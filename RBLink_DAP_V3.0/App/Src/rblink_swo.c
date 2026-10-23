#include "DAP_config.h"
#include "DAP.h"
#include "rblink_board.h"
#include "rblink_debug.h"

/* CMSIS-DAP command-mode SWO capture for the final V3 board.
   PD9 is USART3_RX (AF7), DMA1 Stream1 Channel4 continuously stores bytes in
   a power-of-two ring. USB and Wi-Fi both reach these standard DAP commands
   through DAP_ExecuteCommand(), so no transport-specific trace path is needed. */

#if (SWO_UART != 0)

#if (RBLINK_DEBUG_ENABLE != 0U)
#error "RBLink SWO and diagnostic printing both use USART3; disable RBLINK_DEBUG_ENABLE"
#endif

#if ((SWO_BUFFER_SIZE & (SWO_BUFFER_SIZE - 1U)) != 0U)
#error "SWO_BUFFER_SIZE must be a power of two"
#endif

#define RB_SWO_DMA_STREAM       DMA1_Stream1
#define RB_SWO_DMA_CHANNEL      DMA_CHANNEL_4
#define RB_SWO_BUFFER_MASK      (SWO_BUFFER_SIZE - 1U)
#define RB_SWO_DEFAULT_BAUD     2000000U

static uint8_t swo_buffer[SWO_BUFFER_SIZE] __attribute__((aligned(4)));
static volatile uint32_t swo_dma_wraps;
static uint32_t swo_read_total;
static uint32_t swo_baudrate = RB_SWO_DEFAULT_BAUD;
static uint8_t swo_transport;
static uint8_t swo_mode;
static uint8_t swo_status;

static void swo_dma_stop(void)
{
  RB_SWO_DMA_STREAM->CR &= ~DMA_SxCR_EN;
  while ((RB_SWO_DMA_STREAM->CR & DMA_SxCR_EN) != 0U) { }
  USART3->CR3 &= ~USART_CR3_DMAR;
  NVIC_DisableIRQ(DMA1_Stream1_IRQn);
  DMA1->LIFCR = DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 |
                DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1;
}

static uint32_t swo_program_baudrate(uint32_t requested)
{
  UART_HandleTypeDef uart = {0};
  uint32_t pclk;
  uint32_t divider;
  uint32_t actual;

  if (requested < 1000U) requested = 1000U;
  if (requested > SWO_UART_MAX_BAUDRATE) requested = SWO_UART_MAX_BAUDRATE;

  uart.Instance = USART3;
  uart.Init.BaudRate = requested;
  uart.Init.WordLength = UART_WORDLENGTH_8B;
  uart.Init.StopBits = UART_STOPBITS_1;
  uart.Init.Parity = UART_PARITY_NONE;
  uart.Init.Mode = UART_MODE_RX;
  uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  uart.Init.OverSampling = (requested > (HAL_RCC_GetPCLK1Freq() / 16U)) ?
                           UART_OVERSAMPLING_8 : UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&uart) != HAL_OK) return 0U;

  pclk = HAL_RCC_GetPCLK1Freq();
  if ((USART3->CR1 & USART_CR1_OVER8) != 0U) {
    divider = 8U * (USART3->BRR >> 4U) + (USART3->BRR & 0x07U);
  } else {
    divider = 16U * (USART3->BRR >> 4U) + (USART3->BRR & 0x0FU);
  }
  actual = (divider != 0U) ? (pclk / divider) : 0U;
  return actual;
}

static uint32_t swo_write_total(void)
{
  uint32_t primask = __get_PRIMASK();
  uint32_t wraps;
  uint32_t position;

  __disable_irq();
  /* Sample NDTR before TCIF. If the circular DMA wraps between the two
     reads, TCIF is then set and NDTR is sampled again in the new cycle.
     Reading TCIF first allowed a wrap immediately before the NDTR read to
     return an old wrap count with a new, small position; unsigned subtraction
     then falsely reported a complete-buffer overrun. */
  position = SWO_BUFFER_SIZE - RB_SWO_DMA_STREAM->NDTR;
  if ((DMA1->LISR & DMA_LISR_TCIF1) != 0U) {
    DMA1->LIFCR = DMA_LIFCR_CTCIF1;
    swo_dma_wraps++;
    position = SWO_BUFFER_SIZE - RB_SWO_DMA_STREAM->NDTR;
  }
  wraps = swo_dma_wraps;
  if (primask == 0U) __enable_irq();

  return wraps * SWO_BUFFER_SIZE + position;
}

static uint32_t swo_available(void)
{
  uint32_t written;
  uint32_t count;
  uint32_t sr = USART3->SR;
  uint8_t resync = 0U;

  if ((sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) != 0U) {
    volatile uint32_t discard = USART3->DR;
    (void)discard;
    swo_status |= DAP_SWO_STREAM_ERROR;
    resync = 1U;
  }

  written = swo_write_total();
  /* Target reset can leave a partial UART frame in the DMA ring. Drop only
     bytes received up to that hardware error and restart at a known producer
     boundary. Subsequent valid ITM packets remain available normally. */
  if (resync != 0U) {
    swo_read_total = written;
    return 0U;
  }
  count = written - swo_read_total;
  if (count > SWO_BUFFER_SIZE) {
    swo_read_total = written - SWO_BUFFER_SIZE;
    count = SWO_BUFFER_SIZE;
    swo_status |= DAP_SWO_BUFFER_OVERRUN;
  }
  return count;
}

static void swo_capture_start(void)
{
  volatile uint32_t clear;
  swo_dma_stop();
  swo_dma_wraps = 0U;
  swo_read_total = 0U;
  swo_status = DAP_SWO_CAPTURE_ACTIVE;

  /* The target can already be toggling PD9 while Mode/Baudrate commands are
     processed. Clear setup-time FE/NE/ORE immediately before DMA starts. */
  clear = USART3->SR;
  clear = USART3->DR;
  (void)clear;

  RB_SWO_DMA_STREAM->PAR = (uint32_t)&USART3->DR;
  RB_SWO_DMA_STREAM->M0AR = (uint32_t)swo_buffer;
  RB_SWO_DMA_STREAM->NDTR = SWO_BUFFER_SIZE;
  RB_SWO_DMA_STREAM->CR = RB_SWO_DMA_CHANNEL | DMA_SxCR_PL_1 |
                          DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_TCIE;
  RB_SWO_DMA_STREAM->FCR = 0U;
  DMA1->LIFCR = DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 |
                DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1;
  HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 4U, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
  USART3->CR3 |= USART_CR3_DMAR;
  RB_SWO_DMA_STREAM->CR |= DMA_SxCR_EN;
}

static void swo_capture_stop(void)
{
  swo_dma_stop();
  swo_status = 0U;
}

static uint32_t swo_mode_enable(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();
  __HAL_RCC_USART3_CLK_ENABLE();
  gpio.Pin = RB_SWO_PIN;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF7_USART3;
  HAL_GPIO_Init(RB_SWO_PORT, &gpio);
  return swo_program_baudrate(swo_baudrate);
}

static void swo_mode_disable(void)
{
  GPIO_InitTypeDef gpio = {0};
  swo_capture_stop();
  USART3->CR1 &= ~USART_CR1_UE;
  gpio.Pin = RB_SWO_PIN;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(RB_SWO_PORT, &gpio);
}

void DMA1_Stream1_IRQHandler(void)
{
  uint32_t flags = DMA1->LISR;
  if ((flags & DMA_LISR_TCIF1) != 0U) {
    DMA1->LIFCR = DMA_LIFCR_CTCIF1;
    swo_dma_wraps++;
  }
  if ((flags & (DMA_LISR_TEIF1 | DMA_LISR_DMEIF1 | DMA_LISR_FEIF1)) != 0U) {
    DMA1->LIFCR = DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CFEIF1;
    swo_status |= DAP_SWO_STREAM_ERROR;
  }
}

uint32_t SWO_Transport(const uint8_t *request, uint8_t *response)
{
  uint8_t value = request[0];
  if (((swo_status & DAP_SWO_CAPTURE_ACTIVE) == 0U) && (value <= 1U)) {
    swo_transport = value;
    response[0] = DAP_OK;
  } else {
    response[0] = DAP_ERROR;
  }
  return (1U << 16) | 1U;
}

uint32_t SWO_Mode(const uint8_t *request, uint8_t *response)
{
  uint8_t value = request[0];
  uint32_t result = 0U;

  if (swo_mode == DAP_SWO_UART) swo_mode_disable();
  if (value == DAP_SWO_OFF) {
    result = 1U;
  } else if (value == DAP_SWO_UART) {
    result = swo_mode_enable();
  }
  swo_mode = (result != 0U) ? value : DAP_SWO_OFF;
  swo_status = 0U;
  response[0] = (result != 0U) ? DAP_OK : DAP_ERROR;
  return (1U << 16) | 1U;
}

uint32_t SWO_Baudrate(const uint8_t *request, uint8_t *response)
{
  uint32_t requested = (uint32_t)request[0] | ((uint32_t)request[1] << 8U) |
                       ((uint32_t)request[2] << 16U) | ((uint32_t)request[3] << 24U);
  uint32_t actual = 0U;
  if (swo_mode == DAP_SWO_UART) {
    uint8_t active = swo_status & DAP_SWO_CAPTURE_ACTIVE;
    if (active != 0U) swo_capture_stop();
    actual = swo_program_baudrate(requested);
    if (actual != 0U) swo_baudrate = actual;
    if ((active != 0U) && (actual != 0U)) swo_capture_start();
  }
  response[0] = (uint8_t)actual;
  response[1] = (uint8_t)(actual >> 8U);
  response[2] = (uint8_t)(actual >> 16U);
  response[3] = (uint8_t)(actual >> 24U);
  return (4U << 16) | 4U;
}

uint32_t SWO_Control(const uint8_t *request, uint8_t *response)
{
  uint8_t active = request[0] & DAP_SWO_CAPTURE_ACTIVE;
  if (swo_mode != DAP_SWO_UART) {
    response[0] = DAP_ERROR;
  } else {
    if ((active != 0U) && ((swo_status & DAP_SWO_CAPTURE_ACTIVE) == 0U)) {
      swo_capture_start();
    } else if ((active == 0U) && ((swo_status & DAP_SWO_CAPTURE_ACTIVE) != 0U)) {
      swo_capture_stop();
    }
    response[0] = DAP_OK;
  }
  return (1U << 16) | 1U;
}

uint32_t SWO_Status(uint8_t *response)
{
  uint32_t count = swo_available();
  response[0] = swo_status;
  response[1] = (uint8_t)count;
  response[2] = (uint8_t)(count >> 8U);
  response[3] = (uint8_t)(count >> 16U);
  response[4] = (uint8_t)(count >> 24U);
  return 5U;
}

uint32_t SWO_ExtendedStatus(const uint8_t *request, uint8_t *response)
{
  uint8_t command = request[0];
  uint32_t count = swo_available();
  uint32_t length = 0U;
  if ((command & 0x01U) != 0U) response[length++] = swo_status;
  if ((command & 0x02U) != 0U) {
    response[length++] = (uint8_t)count;
    response[length++] = (uint8_t)(count >> 8U);
    response[length++] = (uint8_t)(count >> 16U);
    response[length++] = (uint8_t)(count >> 24U);
  }
  if ((command & 0x04U) != 0U) {
    uint32_t tick = TIMESTAMP_GET();
    uint32_t index = swo_write_total();
    response[length++] = (uint8_t)index;
    response[length++] = (uint8_t)(index >> 8U);
    response[length++] = (uint8_t)(index >> 16U);
    response[length++] = (uint8_t)(index >> 24U);
    response[length++] = (uint8_t)tick;
    response[length++] = (uint8_t)(tick >> 8U);
    response[length++] = (uint8_t)(tick >> 16U);
    response[length++] = (uint8_t)(tick >> 24U);
  }
  return (1U << 16) | length;
}

uint32_t SWO_Data(const uint8_t *request, uint8_t *response)
{
  uint32_t count = swo_available();
  uint32_t requested = (uint32_t)request[0] | ((uint32_t)request[1] << 8U);
  uint32_t index;
  uint32_t i;

  if ((swo_transport != 1U) || ((swo_status & DAP_SWO_CAPTURE_ACTIVE) == 0U)) count = 0U;
  if (requested > (DAP_PACKET_SIZE - 4U)) requested = DAP_PACKET_SIZE - 4U;
  if (count > requested) count = requested;
  response[0] = swo_status;
  response[1] = (uint8_t)count;
  response[2] = (uint8_t)(count >> 8U);
  index = swo_read_total;
  for (i = 0U; i < count; ++i) response[3U + i] = swo_buffer[(index + i) & RB_SWO_BUFFER_MASK];
  swo_read_total = index + count;
  /* Report errors for one polling interval instead of keeping one transient
     framing error sticky for the rest of the capture session. */
  swo_status &= (uint8_t)~(DAP_SWO_STREAM_ERROR | DAP_SWO_BUFFER_OVERRUN);
  return (2U << 16) | (3U + count);
}

#endif /* SWO_UART */
