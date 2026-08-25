# RBLink DAP V3.0 build notes

## Toolchain

- MCU: STM32F407VGT6, HSE 25 MHz, SYSCLK 168 MHz, USB clock 48 MHz.
- IDE project: `MDK-ARM/RBLink_DAP_V3.0.uvprojx`.
- Verified compiler: Arm Compiler 5.06 update 7 (build 960).
- Output starts at `0x08000000`; V3.0 is a no-bootloader image.

Run `MDK-ARM/build.ps1`. The script returns a non-zero exit code when the Keil
log contains errors and leaves the HEX in `MDK-ARM/RBLink_DAP_V3.0`.

## Firmware architecture

- CubeMX owns `Core` and system clock/USB PCD generation.
- Product code is isolated under `App`; CubeMX regeneration must preserve it.
- USB is one composite device: interface 0 CMSIS-DAP v2 Bulk/WinUSB and
  interfaces 1/2 CDC ACM for target USART1.
- Private CMSIS-DAP commands provide SPI2, I2C3, CAN1 and raw-DAC power control.
- SPI1 exchanges fixed 512-byte CRC frames with ESP32-C3; SPI3 owns external NOR.

## Hardware validation still required

Compilation cannot replace first-board tests. Before enabling a target rail,
verify enable polarity, DAC-to-VOUT calibration, ADC divider ratios and NTC
curve. Start ESP SPI at the current 10.5 MHz and only use the 40 MHz target
after signal-integrity testing. Confirm the fitted NOR JEDEC ID before allowing
algorithm-image erase/program operations.
