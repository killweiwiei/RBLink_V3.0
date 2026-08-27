#ifndef RBLINK_BOARD_H
#define RBLINK_BOARD_H

#include "stm32f4xx_hal.h"

/* Final RBLink V3.0 pin contract (schematic/netlist dated 2026-08-25). */
#define RB_LED_DATA_PORT       GPIOC
#define RB_LED_DATA_PIN        GPIO_PIN_1
#define RB_LED_STATE_PORT      GPIOC
#define RB_LED_STATE_PIN       GPIO_PIN_2

#define RB_ADC_NTC_PIN         GPIO_PIN_0
#define RB_ADC_VOUT_PIN        GPIO_PIN_1
#define RB_ADC_VREF_PIN        GPIO_PIN_2
#define RB_DAC_VOUT_PIN        GPIO_PIN_4

#define RB_UART_TX_PIN         GPIO_PIN_9       /* USART1_TX */
#define RB_UART_RX_PIN         GPIO_PIN_10      /* USART1_RX */
#define RB_DEBUG_TX_PIN        GPIO_PIN_10      /* USART3_TX */
#define RB_DEBUG_RX_PIN        GPIO_PIN_11      /* USART3_RX */

/* DAP: separate SWDIO output/input removes GPIO mode-switch delay. */
#define RB_NRESET_PORT         GPIOD
#define RB_NRESET_PIN          GPIO_PIN_8
#define RB_SWO_PORT            GPIOD
#define RB_SWO_PIN             GPIO_PIN_9
#define RB_TDO_PORT            GPIOD
#define RB_TDO_PIN             GPIO_PIN_10
#define RB_SWCLK_PORT          GPIOD
#define RB_SWCLK_PIN           GPIO_PIN_11
#define RB_SWDIO_IN_PORT       GPIOD
#define RB_SWDIO_IN_PIN        GPIO_PIN_12
#define RB_SWDIO_OUT_PORT      GPIOD
#define RB_SWDIO_OUT_PIN       GPIO_PIN_13
#define RB_TDI_PORT            GPIOD
#define RB_TDI_PIN             GPIO_PIN_14
#define RB_NTRST_PORT          GPIOD
#define RB_NTRST_PIN           GPIO_PIN_15
#define RB_SWDIO_DIR_PORT      GPIOE
#define RB_SWDIO_DIR_PIN       GPIO_PIN_10

/* Target buses. */
#define RB_SPI_NSS_PORT        GPIOB
#define RB_SPI_NSS_PIN         GPIO_PIN_12
#define RB_SPI_SCK_PIN         GPIO_PIN_13
#define RB_SPI_MISO_PIN        GPIO_PIN_14
#define RB_SPI_MOSI_PIN        GPIO_PIN_15
#define RB_SPI_PINS            (RB_SPI_SCK_PIN | RB_SPI_MISO_PIN | RB_SPI_MOSI_PIN) /* SPI2 */
#define RB_I2C_SCL_PORT        GPIOA
#define RB_I2C_SCL_PIN         GPIO_PIN_8
#define RB_I2C_SDA_PORT        GPIOC
#define RB_I2C_SDA_PIN         GPIO_PIN_9
#define RB_CAN_PORT            GPIOB
#define RB_CAN_RX_PIN          GPIO_PIN_8
#define RB_CAN_TX_PIN          GPIO_PIN_9
#define RB_CAN_PINS            (RB_CAN_RX_PIN | RB_CAN_TX_PIN)

/* ESP32-C3 SPI1 bridge. */
#define RB_ESP_EN_PORT         GPIOE
#define RB_ESP_EN_PIN          GPIO_PIN_0
#define RB_ESP_SPI_PORT        GPIOB
#define RB_ESP_SPI_PINS        (GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5)
#define RB_ESP_NSS_PORT        GPIOB
#define RB_ESP_NSS_PIN         GPIO_PIN_6
#define RB_ESP_READY_PORT      GPIOB
#define RB_ESP_READY_PIN       GPIO_PIN_7

/* External NOR Flash on SPI3, CS is PA15 in the final netlist. */
#define RB_FLASH_NSS_PORT      GPIOA
#define RB_FLASH_NSS_PIN       GPIO_PIN_15
#define RB_FLASH_SPI_PORT      GPIOC
#define RB_FLASH_SPI_PINS      (GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12)

/* Target/power interface enables are active high and have 100k pull-downs. */
#define RB_BOOST_EN_PORT       GPIOD
#define RB_BOOST_EN_PIN        GPIO_PIN_1
#define RB_LDO_EN_PORT         GPIOD
#define RB_LDO_EN_PIN          GPIO_PIN_0
#define RB_JTAG_EN_PORT        GPIOA
#define RB_JTAG_EN_PIN         GPIO_PIN_7
#define RB_UART_EN_PORT        GPIOA
#define RB_UART_EN_PIN         GPIO_PIN_5
#define RB_SPI_EN_PORT         GPIOE
#define RB_SPI_EN_PIN          GPIO_PIN_11
#define RB_I2C_EN_PORT         GPIOA
#define RB_I2C_EN_PIN          GPIO_PIN_6
#define RB_VTRGT_STATUS_PORT   GPIOE
#define RB_VTRGT_STATUS_PIN    GPIO_PIN_7
#define RB_HARDKEY_PORT        GPIOE
#define RB_HARDKEY_PIN         GPIO_PIN_2

void RB_Board_Init(void);
void RB_DAP_Enable(uint8_t enable);
void RB_UART_Enable(uint8_t enable);
void RB_SPI_Enable(uint8_t enable);
void RB_I2C_Enable(uint8_t enable);
void RB_LED_State(uint8_t on);
void RB_LED_Data(uint8_t on);

#endif
