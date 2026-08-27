# RBLink V3.0 firmware

This directory contains only the two production firmware projects and the
hardware/protocol documents needed to maintain them.

| Directory | Purpose |
|---|---|
| `RBLink_DAP_V3.0` | STM32F407VGT6 Keil/CubeMX firmware |
| `esp32c3` | ESP32-C3-MINI-1-N4/N4X ESP-IDF wireless bridge firmware |
| `docs` | Hardware specification, pin map, CubeMX notes and private protocol documentation |

The STM32 project is the USB-facing owner of CMSIS-DAP, target UART, SPI, I2C,
isolated CAN, adjustable power control and external NOR. The ESP32 remains a
transparent TCP-to-SPI transport; it does not directly drive target pins.

Open `RBLink_DAP_V3.0/MDK-ARM/RBLink_DAP_V3.0.uvprojx` for STM32 development.
See `RBLink_DAP_V3.0/BUILD.md` for the verified build and hardware bring-up
constraints.

Key documents:

- `docs/RBLINK_V3.0_HARDWARE_SPEC.md`
- `docs/RBLINK_V3.0_PINMAP.md`
- `docs/RBLINK_V3.0_CUBEMX.md`
- `docs/RBLINK_V3_VENDOR_PROTOCOL.md`
- `docs/RBLINK_V3_ROADMAP.md`
