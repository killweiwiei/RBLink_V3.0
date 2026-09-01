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
| GPIO0 | output | - | Network-link LED, active high; 500 ms blink while waiting, steady when a TCP client is connected |
| GPIO1 | output | - | SPI/TCP data-activity LED, active high; non-blocking 60 ms minimum pulse |
| GPIO18/19 | bidirectional | USB connector | ESP native USB reserved |

GPIO10 is high only while an SPI slave transaction is queued and safe for the
STM32 master to clock. It goes low at the end of CS. It does **not** mean that a
network packet is pending. The STM32 must wait for READY before each 512-byte
transaction and may transfer an IDLE frame when it only needs to poll.

## Transport

- ESP32-C3 is an SPI2 slave; STM32 SPI1 is the master, mode 0.
- Both SPI and TCP use one fixed 512-byte frame, including CRC-32.
- Channels are `0 control`, `1 CMSIS-DAP`, `2 UART`, `3 RBLink vendor`, and
  `4 log`; `0xFF` is an idle poll.
- Control operations `0x01 PING` and `0x02 GET_INFO` terminate locally on ESP,
  allowing the PC to verify Wi-Fi and protocol health without STM32 traffic.
- The ESP runs in AP+STA mode. Its WPA2 SoftAP is named `RBlink-xxxxxx`; saved
  router credentials are loaded from NVS and reconnect automatically. TCP port
  `3240` is reachable through either interface. Only one PC client is allowed.
- Open `http://192.168.4.1` while connected to the RBLink AP for web
  provisioning. USB local provisioning goes through the STM32 CMSIS-DAP
  vendor command and SPI control channel; ESP native USB is not used for it.
- The same page exposes `/ws/debug` as a binary WebSocket and includes a Web
  SWD console. It identifies the STM32 bridge, connects a target Debug Port,
  reads DP IDCODE and target memory, and halts/resumes a Cortex-M core. Web
  debug and raw TCP port `3240` are mutually exclusive during transactions.
  Flash erase/program is intentionally not exposed by the initial Web console.
- Change the default Wi-Fi password in `menuconfig` before production release.

The current implementation is a transparent transport. CMSIS-DAP execution,
UART buffering, and vendor commands remain on STM32, so USB and wireless paths
share the same target-side implementation.

## Build

The configured environment is ESP-IDF v5.5.5 with Python 3.12 installed at
`E:\Esp\.espressif\v5.5.5`. This project includes a wrapper which sets
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

After joining the `RBlink-xxxxxx` SoftAP, verify both the ESP Wi-Fi service and
the complete ESP-to-STM32 SPI bridge with:

```text
python tools/rblink_wireless_probe.py
```

To isolate the ESP Wi-Fi/TCP service without requiring an STM32 response, use
`python tools/rblink_wireless_probe.py --esp-only`.
