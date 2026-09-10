# RBLink V3 private protocol

RBLink V3 keeps CMSIS-DAP v2 and UART CDC compatible with standard host tools. SPI, I2C, CAN and target power use CMSIS-DAP vendor commands in the same Bulk command transport.

All multi-byte integers are little-endian. A request after the command byte is `[payload_length, action, data...]`; a response is `[command, status, data_length, data...]`. Actions are `0=configure`, `1=transfer/query`, and `2=close`. A close request contains no action data and physically disables/deinitializes the corresponding peripheral where possible. Power is the exception: its close action is a compatibility no-op because VPOWER must continuously supply the target-side level translators. Status values are `0` success, `1` bad length/value, `2` bad action and `3` hardware/timeout error. Maximum returned data is 56 bytes.

| Command | Module | Configure (`action=0`) | Transfer/query (`action=1`) |
|---|---|---|---|
| `0x85` | SPI2 | `speed:u32, mode:u8, lsb_first:u8` | `cs_hold:u8, count:u8, tx[count]`; returns simultaneous RX bytes |
| `0x86` | I2C3 | `speed:u32` (up to 400 kHz) | `address7:u8, write_len:u8, read_len:u8, write_data[]`; returns read bytes |
| `0x87` | CAN1 | `bitrate:u32` | `flags:u8, id:u32, dlc:u8, data[]`; `flags.bit7=1` polls one received frame |
| `0x88` | Wi-Fi | transactional `BEGIN/CHUNK/COMMIT` | returns ESP configuration, STA/AP state, SSID and IPv4 address |
| `0x89` | Storage/Algorithm | reads W25Q32 identity and fixed layout | bounded read/write/4 KiB erase in the algorithm or offline-firmware region |
| `0x8E` | Power | legacy `enable:u8, dac_raw:u16`; closed loop `enable:u8, target_mV:u16, mode=1`; `enable` is retained for wire compatibility but cannot turn VPOWER off | no data; status returns `enabled`, PA2 VREF, PA1 VOUT, PA0 NTC, live DAC code, two reserved bytes, target mV and closed-loop flag；14字节状态响应向后兼容旧9/11字节主机 |
| `0x8F` | Information | payload length must be zero | returns protocol major/minor, capability bits, firmware major/minor/patch and board revision |

The information payload is `[protocol_major, protocol_minor, capabilities:u16, firmware_major, firmware_minor, firmware_patch, board_revision]`. Hosts must accept the original four-byte prefix so older V3 firmware remains usable.

Storage `0x89` is implemented by protocol `1.5` / firmware `V3.0.05`. Configure
has no data and returns `[layout_version, flags, jedec_mfr, jedec_type,
jedec_density, capacity:u32, page_size:u16, sector_size:u16,
algorithm_base:u32, algorithm_size:u32, offline_base:u32, offline_size:u32,
max_write:u8, max_read:u8]`. `flags.bit0` means a supported 4 MiB density is
ready. Transfer data starts with `operation, region, offset:u32`; operation 1
adds `count:u8` and reads, operation 2 adds `count:u8,data[]` and programs, and
operation 3 erases one aligned 4 KiB sector. Region 1 is the algorithm library
and region 2 is the target offline image. Offset is always relative to the
selected region; metadata and physical addresses cannot be reached through
this command. The current fixed layout reserves the first 192 KiB for RBFS,
assigns region 1 the following 2.8125 MiB and region 2 the final 1 MiB.
Read/write count is 1–48 bytes.

Wi-Fi `0x88` configure data starts with suboperation `BEGIN=0`, `CHUNK=1`, or
`COMMIT=2`. Transfer returns `[flags, ssid_length, ssid..., ipv4]`; flags bit 0
means configured, bit 1 STA connected, and bit 2 AP active. Close clears saved
router credentials. The password is never returned.

CAN flag bit 0 selects a 29-bit extended ID, bit 1 selects a remote frame, and bit 7 selects reception. A received CAN frame is returned as `flags, id:u32, dlc, data[]`. An empty successful receive response means that no frame is queued.

RBLink V3 keeps the active-high JTAG, UART, SPI and I2C translator OE pins asserted after firmware initialization, including after a logical protocol close. Closing a protocol releases/deinitializes its controller pins but does not remove translator power. TXU0304 still enters high impedance whenever either VCCA or VCCB is absent; therefore the target must provide VREF or the adjustable VPOWER output must be enabled before connector-side waveforms can appear. CA-IS2062VW has no software enable input and requires its logic-side supply independently.

SPI `CONFIG` may be sent while SPI is already open. Firmware first releases NSS, deinitializes SPI2, and then atomically applies the new speed and mode. SPI2 exposes the APB1 divider steps from 21 MHz down to 164.063 kHz and rejects requests outside that range. When the power loop is within its ADC deadband, its feedback interval changes from 20 ms to 100 ms and immediately returns to 20 ms whenever correction is required. Every ADC operation is single-shot and calls `HAL_ADC_Stop()` after reading.

The legacy power command accepts a raw 12-bit DAC code. Closed-loop targets are restricted to 550–5000 mV in firmware and host software. Six board measurements on 2026-09-01 (258=4.8 V, 486=4.5 V, 1398=3.5 V, 2006=2.85 V, 2538=2.25 V and 3526=1.15 V) produce the feed-forward fit `VOUT = 5.0574519904 - 0.00110686607115 * DAC`. Closed-loop mode starts from that fitted DAC code, then samples PA1 every 20 ms and adjusts PA4 with an approximately 6.4 mV deadband and a maximum 64-code step. At the low-output saturation limit, PA4 automatically changes from buffered DAC code 4095 to GPIO high, recovering the DAC buffer's high-rail headroom and approaching the TLV759P specified 0.55 V minimum. It returns to DAC mode automatically when the requested output rises. PA4 is no longer ADC-sampled; status bytes 9–10 are reserved as `0xFFFF`. Startup follows BOOST first, then LDO. On reset, a blank device enables the factory-default 3.3 V target; a configured device restores and enables its saved voltage target. VPOWER remains on for the lifetime of normal firmware operation and can only be shut down internally after a hardware-initialization or DAC error.

If an internal power fault forces the rail off, a power-status request returns zero for VREF, VOUT and NTC without starting an ADC conversion. There is no user-facing command that requests this state.

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
