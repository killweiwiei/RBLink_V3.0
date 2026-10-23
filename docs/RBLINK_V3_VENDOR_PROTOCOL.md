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

## I2C register-map extension (protocol 1.10, firmware V3.0.34)

Capability bit9 (`0x0200`) enables a simple 8-bit address / 8-bit value slave map.
Command remains `0x86`; action-specific data excludes the action byte below.

| Action | Data | Effect |
|---|---|---|
| 11 Begin | address7, filler | Validate 08–77; fill inactive 256-byte bank; begin new upload |
| 12 Chunk | start:u8, count:u8, values[count] | count 1–48; start+count ≤256; upload only, no live change |
| 13 Commit | none | Require pending upload and idle bus; activate complete bank and listen |
| 8 Poll | none | Existing 24-byte statistics header plus RX bytes |
| 2 Close | none | Disable listener, cancel pending upload |

Successful actions 11–13 return no data. Busy status 4 leaves an uncommitted upload pending.
Commit initializes the listener at 100 kHz configuration with clock stretching enabled;
the external master supplies SCL. Commit resets pointer/statistics. Generic configuration,
close and legacy slave configuration leave map mode. Legacy action7 is rejected while map mode is active.
The first byte of each external write transaction sets the register pointer; further write
bytes are logged only. Reads return map[pointer++] with 8-bit wraparound. Pointer starts at 00
and persists across STOP/repeated START. Address match ACK and final master NACK are normal.
Do not mistake a failed commit for continued listening; on initialization error reopen the slave.
Usage and hardware checks: [I2C_REGISTER_MAP.md](I2C_REGISTER_MAP.md).

## I2C slave extension (protocol 1.8, firmware V3.0.32)

Capability bit 7 (`0x0080`) announces I2C3 slave support. Do not send these
actions unless this capability is present. V2 and older V3 master actions stay
unchanged. Master and slave modes are mutually exclusive on the same connector.
Close (`action=2`) disables both I2C3 IRQs and releases the controller.

| Action | Data following action | Result |
|---|---|---|
| 6, configure slave | `address7:u8, filler:u8, count:u8, preset[count]` | Start listening with preset already loaded; reset queue/counters |
| 7, replace preset | `count:u8, preset[count]` | Replace buffer only when the external bus is idle |
| 8, drain/status | none | 24-byte status header, followed by 0–32 received bytes |

Addresses are `0x08–0x77` and must not be shifted by the host. Preset count is
0–48. An empty preset returns only the filler byte. Each external read address
match resets the preset index to zero; after the preset ends, return filler.
This first stage provides raw byte reception/transmission, not an EEPROM
register-pointer emulator. The firmware never waits for USB/Wi-Fi to supply
bytes during a transaction, and supports SCL stretching between bytes.
Validate initially with a 100 kHz external master. The hardware has 400 kHz
capability, but 400 kHz operation requires separate board-level validation.

Status 4 means BUSY: wait for STOP/idle, then retry configuration/preset update.
Never issue the master's nine-clock bus recovery while operating as a slave.
The master's final read NACK (AF) is normal and does not increment errors.

| Header offset | Type | Meaning |
|---|---|---|
| 0 | u8 | Status format version, currently 1 |
| 1 | u8 | Active 7-bit address |
| 2 | u16 | Bytes still queued **after** this response drains 0–32 bytes |
| 4 | u32 | Total received bytes, including bytes dropped when queue full |
| 8 | u32 | Bytes supplied to TX data register, including filler; not an ACK count |
| 12 | u32 | Queue overflow bytes (drop newest) |
| 16 | u32 | Sticky SR1 error bitmap: BERR, ARLO, OVR, TIMEOUT; AF excluded |
| 20 | u16 | External write address matches (wraps at 65536) |
| 22 | u16 | External read address matches (wraps at 65536) |
| 24… | bytes | Queued RX data in receive order |

The RX ring has 1023 usable bytes. USB drain chunks do not preserve transaction
boundaries. Host polls at 25 ms with at most 16 responses per turn; transport
latency/other debug operations can still overflow the queue, reported explicitly
by the dropped counter. Closing the PC app or losing the network stops host
polling, not necessarily the standalone slave: send close, reconfigure, or
reset the probe to change its bus role. Error counters persist until the next
successful slave configuration.

## TCU one-shot hardware test (protocol 1.9, firmware V3.0.33)

Capability bit 8 (`0x0100`, high capability byte bit0) advertises the strict
TCU write/STOP/gap/read/STOP test. This is separate from generic preset mode.
Only command `0x86` accepts actions 9 and 10:

- Action 9 data: `role:u8, N:u8, speed:u32`. RBLink role 0=master, 1=slave;
  N is 1 or 16; speed is 100000 or 400000. Address is fixed at unshifted 0x50.
  Start returns immediately, without a target-side address scan or probe.
- Action 10: no data; returns the fixed 56-byte status below. This queries
  local state over USB/Wi-Fi and does not emit I2C frames.
- Action 2: releases the controller and exits test ownership. Ordinary I2C
  configure/transfer/preset operations are rejected while test ownership is
  active. Another action 9 is accepted only after the prior group terminates.

Master execution runs in `RB_Bus_Task`: one HAL pure-data transmit, verify
BUSY clears after STOP, wait 101 system ticks (1 ms tick, conservative margin
for at least 100 ms), then one separate pure-data receive. Each HAL call has a
1000 ms timeout; no auto retry, register prefix, memory read, repeated START,
or nine-clock recovery is used. RX/TX counts reflect HAL transfer progress,
not independently measured ACK counts. Fatal API errors stop the group.

Slave execution waits at most 10 seconds for each phase. The first write must
have exactly N bytes and STOP. Only then is the actual received buffer copied
to the local TX buffer. Wrong pattern data is still echoed but latches failure.
A read before the write, insufficient gap, extra/repeated addressing, wrong
length, abnormal final ACK/NACK or bus error fails the group. Normal final
NACK waits for STOP/bus idle before completion. Terminal state disables ACK
and PE, so the test is not a continuous responder. Host issues close after
capturing terminal diagnostics or cancellation. Flash persistence is deferred
while the test is active to avoid Flash stalls during slave servicing.

| Offset | Field |
|---|---|
| 0 | Format version 1 |
| 1 | RBLink role (0 master, 1 slave) |
| 2 | State: 1 first write, 2 gap/await read, 3 done, 4 failed, 5 await STOP/idle |
| 3 | N (1 or 16) |
| 4 | Errors u32: bit0 length, bit1 data pattern, bit2 bus/API, bit3 sequence, bit4 timeout, bit5 gap, bit6 idle |
| 8,9 | RX/TX byte counts u8 |
| 10,11 | Write/read transaction attempt/address-match counts u8 |
| 12 | Raw error u32 (HAL error for master; SR1 error flags except normal AF for slave) |
| 16 | Configured speed u32, not measured frequency |
| 20 | Elapsed milliseconds u32 |
| 24…39 | Actual RX buffer, 16-byte slot; use first N bytes and RX count |
| 40…55 | TX buffer, 16-byte slot; use first N bytes and TX count |

The pattern is `(0xA5 XOR (37*i)) & 0xFF`; for N=16:
`A5 80 EF CA 31 1C 7B A6 8D E8 D7 32 19 44 A3 8E`.
State 3 and matching data/counters are only the RBLink local check. Final
acceptance still requires TCU g_debug, independent START/STOP and ACK/NACK
waveforms, FIFO/idle cleanup, and measured SCL speed. Neither configuration
values nor USB elapsed time establish actual 100/400 kHz performance.

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
