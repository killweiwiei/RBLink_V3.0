# RBLink V3 private protocol

RBLink V3 keeps CMSIS-DAP v2 and UART CDC compatible with standard host tools. SPI, I2C, CAN and target power use CMSIS-DAP vendor commands in the same Bulk command transport.

All multi-byte integers are little-endian. A request after the command byte is `[payload_length, action, data...]`; a response is `[command, status, data_length, data...]`. Actions are `0=configure`, `1=transfer/query`, and `2=close`. A close request contains no action data and physically disables/deinitializes the corresponding peripheral where possible. Status values are `0` success, `1` bad length/value, `2` bad action and `3` hardware/timeout error. Maximum returned data is 56 bytes.

| Command | Module | Configure (`action=0`) | Transfer/query (`action=1`) |
|---|---|---|---|
| `0x85` | SPI2 | `speed:u32, mode:u8, lsb_first:u8` | `cs_hold:u8, count:u8, tx[count]`; returns simultaneous RX bytes |
| `0x86` | I2C3 | `speed:u32` (up to 400 kHz) | `address7:u8, write_len:u8, read_len:u8, write_data[]`; returns read bytes |
| `0x87` | CAN1 | `bitrate:u32` | `flags:u8, id:u32, dlc:u8, data[]`; `flags.bit7=1` polls one received frame |
| `0x88` | Wi-Fi | transactional `BEGIN/CHUNK/COMMIT` | returns ESP configuration, STA/AP state, SSID and IPv4 address |
| `0x89` | Storage/Algorithm | reserved for object-level NOR storage and resumable upload | specified in `RBLINK_EXTERNAL_FLASH_AND_ALGORITHM.md`; not implemented yet |
| `0x8E` | Power | `enable:u8, dac_raw:u16` | no data; returns `enabled`, then PA2 VREF, PA1 VOUT and PA0 NTC raw 12-bit ADC values；返回字段顺序仍为VREF、VOUT、NTC |
| `0x8F` | Information | payload length must be zero | returns protocol major/minor, capability bits, firmware major/minor/patch and board revision |

The information payload is `[protocol_major, protocol_minor, capabilities:u16, firmware_major, firmware_minor, firmware_patch, board_revision]`. Hosts must accept the original four-byte prefix so older V3 firmware remains usable.

Wi-Fi `0x88` configure data starts with suboperation `BEGIN=0`, `CHUNK=1`, or
`COMMIT=2`. Transfer returns `[flags, ssid_length, ssid..., ipv4]`; flags bit 0
means configured, bit 1 STA connected, and bit 2 AP active. Close clears saved
router credentials. The password is never returned.

CAN flag bit 0 selects a 29-bit extended ID, bit 1 selects a remote frame, and bit 7 selects reception. A received CAN frame is returned as `flags, id:u32, dlc, data[]`. An empty successful receive response means that no frame is queued.

The power command deliberately accepts a raw DAC code rather than millivolts. The final DAC-to-TLV75901 feedback transfer function must be calibrated on the assembled PCB before firmware exposes a voltage unit. Enabling follows BOOST first, then LDO; disabling follows LDO first, then BOOST.

For an I2C write-then-read with a one- or two-byte register address, firmware uses a repeated START automatically. Two-byte register addresses are transmitted most-significant byte first. Controller/bus faults trigger nine-clock open-drain bus recovery; a normal slave NACK does not.

SPI permits a zero-byte transfer as an explicit chip-select operation: `cs_hold=1` asserts NSS and `cs_hold=0` releases it. Reconfiguration and every failed transfer always release NSS.

Target-facing translator enables remain low after reset. UART is connected only while the USB CDC host asserts DTR; SPI and I2C are connected only after their peripheral configuration succeeds. DAP/JTAG is controlled independently by the CMSIS-DAP connection state.

## STM32–ESP32-C3 wireless transport

The internal wireless link is separate from the target-facing vendor commands. STM32 SPI1 is the master and ESP32-C3 SPI2 is the slave. Both SPI and the ESP TCP server carry the same fixed 512-byte little-endian frame:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `0x4B4C4252` (`RBLK` in byte order) |
| 4 | 1 | protocol version, currently `1` |
| 5 | 1 | channel: `0` control, `1` DAP, `2` UART, `3` vendor, `4` log, `0xFF` idle |
| 6 | 1 | flags: bit0 response, bit1 more fragments, bit2 error |
| 7 | 1 | reserved, zero |
| 8 | 4 | sequence number |
| 12 | 2 | payload length, `0..492` |
| 14 | 2 | reserved, zero |
| 16 | 4 | IEEE CRC-32 over all 512 bytes with this field temporarily zero |
| 20 | 492 | payload; unused bytes are zero-filled before CRC calculation |

ESP GPIO10 READY goes high when an SPI DMA transaction has been queued and goes low when that CS transaction finishes. STM32 must wait for READY before every complete 512-byte mode-0 transaction. READY means that it is electrically safe to clock the transaction, not that the ESP necessarily has a non-idle packet. The production clock goal is 40 MHz, but firmware must start at a conservative clock and only select 40 MHz after board-level signal-integrity testing.

The ESP default development endpoint is a WPA2 SoftAP named `RBlink-xxxxxx`, TCP port `3240`, with one active PC client. Release builds must replace the development password. The ESP remains a transparent bridge: DAP execution and all target-facing peripheral ownership stay on STM32.

Control payload operation `0x01` is PING and echoes the complete payload. Operation `0x02` is GET_INFO and returns `operation, status, transport_version, firmware_major, firmware_minor, firmware_patch, capabilities:u16`. These two operations terminate locally on ESP so the host can diagnose the wireless link even when STM32 target services are not yet running.
