# RBLink V3.0 STM32CubeMX工程维护说明

专用CubeMX/Keil工程已经建立在 `RBLink_DAP_V3.0/`。CubeMX只维护启动、系统时钟和USB PCD；产品业务代码固定放在 `App/`，重新生成时不得删除该目录或Keil中的对应分组。

## 工程基础

- MCU：`STM32F407VGT6`，LQFP100。
- 工程名：`RBlink_F407_V3`。
- Toolchain：优先 `MDK-ARM V5`；如果你主要使用STM32CubeIDE，也可以选CubeIDE，但不要同时生成两套工程。
- Firmware Package：使用本机已安装的稳定STM32CubeF4版本，并将库文件复制到工程内，避免依赖用户目录。
- HSE：25 MHz晶振；SYSCLK 168 MHz，AHB 168 MHz，APB1 42 MHz，APB2 84 MHz，USB时钟必须为48 MHz。
- SYS Debug：Serial Wire，保留PA13/PA14用于RBlink自身调试。
- 不启用RTOS。先以中断、DMA和主循环事件队列完成第一版，避免DAP时序被任务切换破坏。
- 生成代码时勾选“为每个外设生成独立 `.c/.h` 文件”，并保留所有 `USER CODE` 区域。

## CubeMX中启用的外设

| 外设 | 引脚 | 初始要求 |
|---|---|---|
| USB_OTG_FS | PA11 DM、PA12 DP | Device Only；禁用VBUS sensing；使用内部D+上拉 |
| ADC1 | PA0 IN0、PA1 IN1、PA2 IN2 | 12位，软件触发；顺序NTC、VPOWER、VREF |
| DAC | PA4 OUT1 | 输出缓冲开启；上电先输出安全码 |
| USART1 | PA9 TX、PA10 RX | 目标UART；启用RX DMA和IDLE中断 |
| USART3 | PB10 TX、PB11 RX | RBlink维护日志口 |
| SPI1 | PB3 SCK、PB4 MISO、PB5 MOSI | STM32 Master，Mode 0；连接ESP；PB6手动CS、PB7 EXTI READY；RX/TX DMA |
| SPI2 | PB13 SCK、PB14 MISO、PB15 MOSI | 目标SPI Master；PB12手动NSS；RX/TX DMA |
| SPI3 | PC10 SCK、PC11 MISO、PC12 MOSI | 外部NOR Flash；PA15手动CS；RX/TX DMA |
| I2C3 | PA8 SCL、PC9 SDA | 目标I2C Master，最高400 kHz；保留GPIO总线恢复能力 |
| CAN1 | PB8 RX、PB9 TX | Classical CAN；具体bit timing运行时配置 |
| TIM8 | PC6 CH1、PC7 CH2 | 两路目标PWM；暂时可只完成引脚和时钟配置 |

USB中间件不要直接选择单一CDC类。最终设备需要CMSIS-DAP v2 Vendor/Bulk与CDC复合设备，后续由我接入复合USB描述符和端点调度；CubeMX只需要生成正确的USB OTG FS底层PCD/HAL。

## 普通GPIO

### 输出且上电默认低

- PA5 `UART_PORTEN`
- PA6 `I2C_PORTEN`
- PA7 `JTAG_PORTEN`
- PE11 `SPI_PORTEN`
- PD0 `LDO_POWEREN`
- PD1 `BOOST_POWEREN`
- PE0 `ESP_CHIPEN`
- PB6 `ESP_SPI_NSS`，空闲应为高；在最早安全初始化阶段先输出高
- PA15 `LT_FLASH_CS`，空闲高
- PC1 `LT_LED_DATA`、PC2 `LT_LED_STATE`，高电平点亮

### 输入

- PB7 `ESP_SPI_RDY`，外部中断上升/下降沿
- PE2 `HARDKEY`，按原理图电平选择上下拉和有效沿
- PE7 `V_TRGT_SWITCH`
- PD9 `SWO_IN`
- PD10 `JTDO_IN`
- PD12 `SWDIO_IN`

### DAP高速GPIO

- PD8 `NRESET_GATE`
- PD11 `SWCLK_OUT`
- PD13 `SWDIO_OUT`
- PD14 `TDI_OUT`
- PD15 `NTRST_OUT`
- PE10 `SWDIO_DIR`

这些DAP引脚先在CubeMX中定义标签，但不要在自动生成的普通HAL循环里实现位操作。后续由我使用直接寄存器/内联函数实现SWD、JTAG和SWO，确保时序稳定。PD12输入与PD13输出是同一目标SWDIO的分离采样/驱动路径，不能合并。

## USB与中断

- USB OTG FS使用最高业务优先级之一，但不要高于所有DAP临界时序中断。
- SPI1 DMA和PB7 READY用于ESP无线桥；READY语义为ESP已排队一个512字节SPI事务。
- USART1 RX使用环形DMA；不要逐字节阻塞接收。
- 禁止在USB、DMA、UART和CAN ISR中执行等待循环或协议解析；ISR只搬运数据并置事件。
- 启用独立看门狗的位置先预留，初始移植阶段不要开启，以免掩盖启动故障。

## 当前工程位置

正式工程已经位于：

`E:\User_Data\Project\Link\LTLink\project\RBLink_V3.0\RBLink_DAP_V3.0`

其中 `.ioc`、`Core/`、`Drivers/`、USB Device Core、`App/` 和 MDK工程均已齐全。
STM32固件已完成CMSIS-DAP v2、Bulk+CDC、UART/SPI/I2C/CAN/电源私有命令、
ESP32-C3 SPI无线桥和NOR基础驱动的完整链接。CubeMX再生成后必须重新确认Keil中的
`Application/RBLink`、`Middleware/CMSIS-DAP`和`Middleware/USB Device`分组仍被保留。
