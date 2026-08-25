# RBLink V3.0 STM32F407VGT6 定版引脚表

> 依据 `Netlist_ltLink_Official_V3.0_2026-08-25.enet` 与
> `SCH_ltLink_Official V3.0_2026-08-25.pdf` 复核。连接关系以网表为准；本文件不再是早期建议草案。

## STM32F407VGT6 已连接 GPIO

| 功能 | MCU引脚 | 外设/方向 | 原理图网络 |
|---|---|---|---|
| NTC采样 | PA0 | ADC123_IN0 | `ADC_NTCIN` |
| 输出电压采样 | PA1 | ADC123_IN1 | `ADC_VPOWERIN` |
| VTREF采样 | PA2 | ADC123_IN2 | `ADC_VREFIN` |
| DAC设定 | PA4 | DAC1_OUT | `DAC_OUT` |
| UART端口使能 | PA5 | GPIO输出 | `UART_PORTEN` |
| I2C端口使能 | PA6 | GPIO输出 | `I2C_PORTEN` |
| JTAG端口使能 | PA7 | GPIO输出 | `JTAG_PORTEN` |
| 目标I2C SCL | PA8 | I2C3_SCL/AF4 | `T_I2C_SCL` |
| 目标UART TX/RX | PA9/PA10 | USART1_TX/RX, AF7 | `T_UTXD` / `T_URXD` |
| USB FS | PA11/PA12 | USB_DM/DP, AF10 | `LT_USB_DM` / `LT_USB_DP` |
| 本机SWD | PA13/PA14 | SWDIO/SWCLK | `LT_SWDIO` / `LT_SWCLK` |
| NOR Flash CS | PA15 | GPIO输出 | `LT_FLASH_CS` |
| ESP SPI | PB3/PB4/PB5 | SPI1 SCK/MISO/MOSI, AF5 | `ESP_SPI_*` |
| ESP SPI NSS/RDY | PB6/PB7 | GPIO输出/EXTI输入 | `ESP_SPI_NSS` / `ESP_SPI_RDY` |
| CAN1 RX/TX | PB8/PB9 | CAN1, AF9 | `T_CAN_RXD` / `T_CAN_TXD` |
| 本机调试UART | PB10/PB11 | USART3_TX/RX, AF7 | `LT_DBG_TX` / `LT_DBG_RX` |
| 目标SPI | PB12/PB13/PB14/PB15 | SPI2 NSS/SCK/MISO/MOSI, AF5 | `T_SPI_*` |
| RGB LED数据/状态 | PC1/PC2 | GPIO输出，高有效 | `LT_LED_DATA` / `LT_LED_STATE` |
| 目标PWM1/PWM2 | PC6/PC7 | TIM8_CH1/CH2, AF3 | `T_PWM1` / `T_PWM2` |
| 目标I2C SDA | PC9 | I2C3_SDA, AF4 | `T_I2C_SDA` |
| NOR Flash | PC10/PC11/PC12 | SPI3 MOSI/MISO/SCK, AF6 | `LT_FLASH_*` |
| LDO/Boost使能 | PD0/PD1 | GPIO输出，高有效 | `LDO_POWEREN` / `BOOST_POWEREN` |
| 目标nRESET | PD8 | GPIO输出，经2N7002K开漏 | `T_NRST` |
| SWO输入/JTAG TDO输入 | PD9/PD10 | GPIO/定时采样输入 | `T_JTDO_SWO-2/-1` |
| SWCLK/TCK | PD11 | GPIO输出 | `T_JTCK_SWCLK` |
| SWDIO输入/输出 | PD12/PD13 | GPIO输入/GPIO输出 | `T_JTMS_SWDIO-2/-1` |
| JTAG TDI/nTRST | PD14/PD15 | GPIO输出 | `T_JTDI` / `T_JNTRST` |
| ESP使能 | PE0 | GPIO输出 | `ESP_CHIPEN` |
| 侧按键 | PE2 | GPIO输入 | `HARDKEY` |
| 目标电压选择状态 | PE7 | GPIO输入 | `V_TRGT_SWITCH` |
| SWDIO方向 | PE10 | GPIO输出 | `DIR_SPI_JTMS_SWDIO` |
| SPI端口使能 | PE11 | GPIO输出 | `SPI_PORTEN` |

未列出的GPIO在本版网表中未连接。`PC0/PC3/PC4/PC5/PB0`虽生成了局部自动网络名，实际没有接入功能网络。

## 关键实现说明

- SWDIO由`PD13`输出、`PD12`输入，`PE10`控制SN74LVC1T45方向。SWD turnaround周期仍须保留。
- 连接器Pin 13经TXU0304同时送到`PD10`（JTAG TDO）及经0Ω可选路径送到`PD9`（SWO）；两脚均不得配置为输出。
- NOR Flash使用SPI3，CS已经改为`PA15`，不是旧文档中的PC9。
- 目标I2C使用I2C3：`PA8=SCL`、`PC9=SDA`；本机调试UART改为USART3：`PB10=TX`、`PB11=RX`。
- `PC6/PC7`不再作为调试UART，已经作为两路目标PWM输出并经过U12电平转换。
- ADC顺序为`PA0=NTC`、`PA1=VPOWER`、`PA2=VREF`，与早期分配不同。
- U11定版继续使用`PI4ULS5V202UEX`，R73/R74目标侧上拉均为4.7kΩ。

## 外部接口

- 自定义20针接口：Pin 1 VREF、Pin 2 VPOWER、Pin 3 nTRST、Pin 4 SPI_NSS、Pin 5 TDI、
  Pin 6 SPI_SCK、Pin 7 TMS/SWDIO、Pin 8 SPI_MOSI、Pin 9 TCK/SWCLK、Pin 10 SPI_MISO、
  Pin 11 GND、Pin 12 I2C_SCL、Pin 13 TDO/SWO、Pin 14 I2C_SDA、Pin 15 nRESET、
  Pin 16 PWM1、Pin 17 UART_TX、Pin 18 PWM2、Pin 19 UART_RX、Pin 20 GND。
- 隔离CAN已经移到独立3针连接器CN1：Pin 1 CANH、Pin 2隔离地`VSIO_GND`、Pin 3 CANL。
- 本机维护口H1：3.3V、SWDIO、SWCLK、GND、USART3_TX、USART3_RX。
