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
- Interface 0 publishes Arm's standard CMSIS-DAP v2 WinUSB GUID
  `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`. Keep this exact value so Keil and
  other standard debuggers can discover the probe.
- Private CMSIS-DAP protocol 1.5 commands provide SPI2, I2C3, CAN1, raw-DAC
  power control, transactional Wi-Fi provisioning and bounded W25Q32 partition
  access. The information response also reports firmware and board revision.
- SPI1 exchanges fixed 512-byte CRC frames with ESP32-C3; SPI3 owns external NOR.
- V3.0.22 adds an unprimed IR transition used only when collecting an already-posted AP result through DP RDBUFF. This preserves the final word without issuing a duplicate AP read, so TAR advances by exactly the requested block length and multi-block RAM verification remains aligned. V3.0.20 introduced IR priming and V3.0.21 first preserved the final AP result but advanced TAR once too far.
- V3.0.27 enables standard CMSIS-DAP command-mode SWO UART capture on PD9.
  USART3_RX feeds a 4096-byte DMA1 Stream1 circular buffer; both USB and Wi-Fi
  transports expose it through `DAP_SWO_*` commands. The returned baud rate is
  the actual APB1 divider result and is limited to 5.25 MHz on STM32F407.
- V3.0.28 clears USART3 status/data immediately before SWO DMA starts, so line
  activity during mode/baud configuration cannot create a false fatal framing
  error. Stream/overrun flags report one polling interval instead of remaining
  sticky for the full capture session.
- V3.0.28 version metadata is defined once in `App/Inc/rblink_version.h`. CMSIS-DAP `DAP_Info`, private command `0x8F`, and the optional USART3 diagnostic banner report the same value.
- V3.0.29 fixes the SWO circular-DMA wrap/NDTR sampling race that could falsely
  report a 4 KiB buffer overrun. A UART framing/noise error (normally caused by
  target reset) now drops data only through the damaged frame and resumes from
  the current DMA producer boundary.
- V3.0.30 extends the V3 SPI configuration with 8/16-bit data size and
  4-wire/3-wire selection. Three-wire transfers explicitly select transmit or
  receive direction; 16-bit host words are byte-swapped around the HAL call so
  their displayed value matches the MSB-first wire value. The original SPI
  configuration length remains accepted as 8-bit, 4-wire for compatibility.
  The structured vendor protocol revision is therefore raised from 1.6 to 1.7.
- Production firmware defaults `RBLINK_DEBUG_ENABLE` to `0` in `App/Inc/rblink_debug.h`; this removes USART3 initialization, trace buffering, formatting, and output calls at preprocessing time. Set it to `1` only for diagnostic builds.
  USART3 is also the PD9 SWO receiver, so SWO builds deliberately reject
  `RBLINK_DEBUG_ENABLE=1`; use one function at a time.
- Wireless UART responses can use the full 492-byte frame payload; reserved bits,
  channels and unused payload bytes are validated/cleared on both MCUs.

## Hardware validation still required

Compilation cannot replace first-board tests. Before enabling a target rail,
verify enable polarity, DAC-to-VOUT calibration, ADC divider ratios and NTC
curve. Start ESP SPI at the current 10.5 MHz and only use the 40 MHz target
after signal-integrity testing. Confirm the fitted NOR JEDEC ID before allowing
algorithm-image erase/program operations.
