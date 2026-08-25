# RBlink ESP32-C3 wireless bridge

This ESP-IDF project implements the wireless side of RBlink V3.0. It targets
`ESP32-C3-MINI-1-N4/N4X`; those module variants use the same firmware image.

## Fixed hardware mapping

| ESP32-C3 | Direction | STM32F407 | Function |
|---|---:|---|---|
| GPIO4 | input | PB3 | SPI1 SCLK |
| GPIO5 | input | PB5 | SPI1 MOSI |
| GPIO6 | output | PB4 | SPI1 MISO |
| GPIO7 | input | PB6 | SPI1 CS, active low |
| GPIO10 | output | PB7 | READY handshake |
| EN | input | PE0 | Module enable |
| GPIO0 | output | - | Network-link LED, active high |
| GPIO1 | output | - | Data-activity LED, active high |
| GPIO18/19 | bidirectional | USB connector | ESP native USB reserved |

GPIO10 is high only while an SPI slave transaction is queued and safe for the
STM32 master to clock. It goes low at the end of CS. It does **not** mean that a
network packet is pending. The STM32 must wait for READY before each 512-byte
transaction and may transfer an IDLE frame when it only needs to poll.

## Transport

- ESP32-C3 is an SPI2 slave; STM32 SPI1 is the master, mode 0.
- Both SPI and TCP use one fixed 512-byte frame, including CRC-32.
- Channels are `0 control`, `1 CMSIS-DAP`, `2 UART`, `3 LTLink vendor`, and
  `4 log`; `0xFF` is an idle poll.
- Control operations `0x01 PING` and `0x02 GET_INFO` terminate locally on ESP,
  allowing the PC to verify Wi-Fi and protocol health without STM32 traffic.
- The ESP starts a WPA2 SoftAP named `RBlink-xxxxxx` and listens on TCP port
  `3240`. Only one PC client is allowed, matching one link controlling one
  target at a time.
- Change the default Wi-Fi password in `menuconfig` before production release.

The current implementation is a transparent transport. CMSIS-DAP execution,
UART buffering, and vendor commands remain on STM32, so USB and wireless paths
share the same target-side implementation.

## Build

The verified environment is ESP-IDF v5.5.5 installed at
`D:\Program Files (x86)\Espressif\.espressif`. This project includes a wrapper which sets
all required paths, so no activated terminal is required:

```text
.\build_idf.ps1 build
.\build_idf.ps1 flash COM8
.\build_idf.ps1 monitor COM8
```

The complete ESP-IDF build has been verified on this computer. Build products
are written to `build/`; the application image is `build/rblink_esp32c3.bin`.

The portable frame/CRC unit test can also be run without ESP-IDF:

```text
gcc -std=c11 -Wall -Wextra -Werror test/test_protocol.c \
    main/rblink_link_protocol.c -Imain -o test_protocol
./test_protocol
```

After joining the `RBlink-xxxxxx` SoftAP, verify the ESP independently with:

```text
python tools/rblink_wireless_probe.py
```
