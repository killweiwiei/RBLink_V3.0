#include "rblink_board.h"

static void output(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState initial)
{
  GPIO_InitTypeDef io = {0};
  HAL_GPIO_WritePin(port, pin, initial);
  io.Pin = pin;
  io.Mode = GPIO_MODE_OUTPUT_PP;
  io.Pull = GPIO_NOPULL;
  io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(port, &io);
}

void RB_Board_Init(void)
{
  GPIO_InitTypeDef io = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

  /* First establish the safe inactive state; external 100k pull-downs cover
     the interval before firmware starts. */
  output(RB_BOOST_EN_PORT, RB_BOOST_EN_PIN, GPIO_PIN_RESET);
  output(RB_LDO_EN_PORT, RB_LDO_EN_PIN, GPIO_PIN_RESET);

  /* Enable every target-interface level translator once firmware owns the
     GPIOs.  External pull-downs still keep all ports disabled during reset
     and before this initialization executes. */
  output(RB_JTAG_EN_PORT, RB_JTAG_EN_PIN, GPIO_PIN_SET);
  output(RB_UART_EN_PORT, RB_UART_EN_PIN, GPIO_PIN_SET);
  output(RB_SPI_EN_PORT, RB_SPI_EN_PIN, GPIO_PIN_SET);
  output(RB_I2C_EN_PORT, RB_I2C_EN_PIN, GPIO_PIN_SET);
  output(RB_SWDIO_DIR_PORT, RB_SWDIO_DIR_PIN, GPIO_PIN_RESET);
  /* ESP32-C3 CHIP_EN is active high.  Hold it low first so every STM32
     power-on/reset also produces a deterministic ESP reset pulse. */
  output(RB_ESP_EN_PORT, RB_ESP_EN_PIN, GPIO_PIN_RESET);

  output(RB_NRESET_PORT, RB_NRESET_PIN, GPIO_PIN_RESET); /* NMOS off */
  output(RB_SWCLK_PORT, RB_SWCLK_PIN, GPIO_PIN_SET);
  output(RB_TDI_PORT, RB_TDI_PIN, GPIO_PIN_SET);
  output(RB_NTRST_PORT, RB_NTRST_PIN, GPIO_PIN_SET);
  output(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
  output(RB_ESP_NSS_PORT, RB_ESP_NSS_PIN, GPIO_PIN_SET);
  output(RB_FLASH_NSS_PORT, RB_FLASH_NSS_PIN, GPIO_PIN_SET);
  output(RB_LED_STATE_PORT, RB_LED_STATE_PIN, GPIO_PIN_RESET);
  output(RB_LED_DATA_PORT, RB_LED_DATA_PIN, GPIO_PIN_RESET);

  /* Both SWDIO MCU pins start released. DAP connect restores PD13 to
     push-pull only after selecting the MCU-to-target translator direction. */
  HAL_GPIO_WritePin(RB_SWDIO_OUT_PORT, RB_SWDIO_OUT_PIN, GPIO_PIN_SET);
  io.Pin = RB_SWDIO_IN_PIN | RB_SWDIO_OUT_PIN | RB_TDO_PIN | RB_SWO_PIN;
  io.Mode = GPIO_MODE_INPUT;
  io.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOD, &io);
  /* Preconfigure PD13's electrical characteristics without enabling its
     output driver; turnaround only needs to change MODER afterwards. */
  RB_SWDIO_OUT_PORT->OTYPER &= ~RB_SWDIO_OUT_PIN;
  RB_SWDIO_OUT_PORT->OSPEEDR =
      (RB_SWDIO_OUT_PORT->OSPEEDR & ~(3UL << GPIO_OSPEEDR_OSPEED13_Pos)) |
      (3UL << GPIO_OSPEEDR_OSPEED13_Pos);
  io.Pin = RB_VTRGT_STATUS_PIN | RB_HARDKEY_PIN;
  HAL_GPIO_Init(GPIOE, &io);
  io.Pin = RB_ESP_READY_PIN;
  HAL_GPIO_Init(GPIOB, &io);

  /* 10 ms comfortably exceeds the ESP32-C3 CHIP_EN reset-low requirement.
     Releasing it here lets the ESP boot while the STM32 initializes USB. */
  HAL_Delay(10U);
  HAL_GPIO_WritePin(RB_ESP_EN_PORT, RB_ESP_EN_PIN, GPIO_PIN_SET);
}

void RB_DAP_Enable(uint8_t enable)  { HAL_GPIO_WritePin(RB_JTAG_EN_PORT, RB_JTAG_EN_PIN, enable ? GPIO_PIN_SET : GPIO_PIN_RESET); }
void RB_UART_Enable(uint8_t enable) { HAL_GPIO_WritePin(RB_UART_EN_PORT, RB_UART_EN_PIN, enable ? GPIO_PIN_SET : GPIO_PIN_RESET); }
void RB_SPI_Enable(uint8_t enable)  { HAL_GPIO_WritePin(RB_SPI_EN_PORT, RB_SPI_EN_PIN, enable ? GPIO_PIN_SET : GPIO_PIN_RESET); }
void RB_I2C_Enable(uint8_t enable)  { HAL_GPIO_WritePin(RB_I2C_EN_PORT, RB_I2C_EN_PIN, enable ? GPIO_PIN_SET : GPIO_PIN_RESET); }
void RB_LED_State(uint8_t on)       { HAL_GPIO_WritePin(RB_LED_STATE_PORT, RB_LED_STATE_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET); }
void RB_LED_Data(uint8_t on)        { HAL_GPIO_WritePin(RB_LED_DATA_PORT, RB_LED_DATA_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET); }
