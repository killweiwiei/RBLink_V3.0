#!/usr/bin/env python3
"""Probe the RBlink ESP32-C3 Wi-Fi service and the STM32 SPI bridge."""

from __future__ import annotations

import argparse
import socket
import struct
import time
import zlib

FRAME_SIZE = 512
MAGIC = 0x4B4C4252
VERSION = 1
CHANNEL_CONTROL = 0
CHANNEL_DAP = 1
FLAG_RESPONSE = 1
OP_GET_INFO = 2
DAP_INFO = 0
DAP_ID_FW_VERSION = 4
DAP_CONNECT = 0x02
DAP_DISCONNECT = 0x03
DAP_TRANSFER_CONFIGURE = 0x04
DAP_TRANSFER = 0x05
DAP_SWJ_CLOCK = 0x11
DAP_SWJ_SEQUENCE = 0x12
DAP_SWD_CONFIGURE = 0x13
DAP_PORT_SWD = 1
DP_IDCODE_READ = 0x02
HEADER = struct.Struct("<IBBBBIHHI")


def make_frame(sequence: int, payload: bytes, channel: int = CHANNEL_CONTROL) -> bytes:
    if len(payload) > FRAME_SIZE - HEADER.size:
        raise ValueError("payload is too long")
    frame = bytearray(FRAME_SIZE)
    HEADER.pack_into(
        frame, 0, MAGIC, VERSION, channel, 0, 0, sequence,
        len(payload), 0, 0,
    )
    frame[HEADER.size:HEADER.size + len(payload)] = payload
    crc = zlib.crc32(frame) & 0xFFFFFFFF
    struct.pack_into("<I", frame, 16, crc)
    return bytes(frame)


def receive_exact(connection: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        block = connection.recv(length - len(data))
        if not block:
            raise ConnectionError("RBlink closed the TCP connection")
        data.extend(block)
    return bytes(data)


def validate_frame(frame: bytes) -> tuple[int, int, int, bytes]:
    if len(frame) != FRAME_SIZE:
        raise ValueError("incorrect frame length")
    magic, version, channel, flags, _, sequence, length, _, expected = (
        HEADER.unpack_from(frame)
    )
    check = bytearray(frame)
    struct.pack_into("<I", check, 16, 0)
    actual = zlib.crc32(check) & 0xFFFFFFFF
    if magic != MAGIC or version != VERSION or length > FRAME_SIZE - HEADER.size:
        raise ValueError("incorrect header")
    if actual != expected:
        raise ValueError(f"CRC mismatch: received {expected:08X}, got {actual:08X}")
    return channel, flags, sequence, frame[HEADER.size:HEADER.size + length]


def parse_info(payload: bytes, announce: bool = True) -> tuple[int, ...] | None:
    if len(payload) < 8 or payload[0] != OP_GET_INFO or payload[1] != 0:
        raise RuntimeError(f"unexpected GET_INFO payload: {payload.hex(' ')}")
    capabilities = int.from_bytes(payload[6:8], "little")
    if announce:
        print(
            f"RBlink ESP online: transport v{payload[2]}, "
            f"firmware {payload[3]}.{payload[4]}.{payload[5]}, "
            f"capabilities=0x{capabilities:04X}"
        )
    if len(payload) >= 36:
        return struct.unpack_from("<IIIIIII", payload, 8)
    if len(payload) >= 24:
        return struct.unpack_from("<IIII", payload, 8)
    return None


def exchange_dap(
    link: socket.socket, sequence: int, request: bytes
) -> bytes:
    link.sendall(make_frame(sequence, request, channel=CHANNEL_DAP))
    channel, flags, response_sequence, payload = validate_frame(
        receive_exact(link, FRAME_SIZE)
    )
    if (
        channel != CHANNEL_DAP
        or not flags & FLAG_RESPONSE
        or response_sequence != sequence
    ):
        raise RuntimeError(
            f"unexpected DAP response routing for sequence {sequence}"
        )
    if len(payload) < 2 or payload[0] != request[0]:
        raise RuntimeError(
            f"unexpected DAP command response: {payload.hex(' ')}"
        )
    return payload


def checked_dap(
    link: socket.socket, sequence: int, request: bytes
) -> None:
    response = exchange_dap(link, sequence, request)
    if response[1] != 0:
        raise RuntimeError(
            f"CMSIS-DAP command 0x{request[0]:02X} failed: "
            f"{response.hex(' ')}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=3240)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument(
        "--swd-test", action="store_true",
        help="connect to the target over SWD and read its DP IDCODE",
    )
    parser.add_argument(
        "--swd-clock", type=int, default=1_000_000,
        help="SWD clock used by --swd-test (default: 1000000)",
    )
    parser.add_argument(
        "--esp-only", action="store_true",
        help="stop after the ESP-local GET_INFO check",
    )
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), args.timeout) as link:
        link.sendall(make_frame(1, bytes([OP_GET_INFO])))
        channel, flags, sequence, payload = validate_frame(
            receive_exact(link, FRAME_SIZE)
        )
        if channel != CHANNEL_CONTROL or not flags & FLAG_RESPONSE or sequence != 1:
            raise RuntimeError("unexpected GET_INFO response routing")
        counters_before = parse_info(payload)
        if args.esp_only:
            return

        # DAP_Info is not handled locally by the ESP.  It crosses the complete
        # TCP -> ESP -> SPI -> STM32 path and the response returns the same way.
        link.sendall(
            make_frame(
                2,
                bytes([DAP_INFO, DAP_ID_FW_VERSION]),
                channel=CHANNEL_DAP,
            )
        )
        try:
            channel, flags, sequence, payload = validate_frame(
                receive_exact(link, FRAME_SIZE)
            )
        except TimeoutError:
            link.sendall(make_frame(3, bytes([OP_GET_INFO])))
            diag_channel, diag_flags, diag_sequence, diag_payload = validate_frame(
                receive_exact(link, FRAME_SIZE)
            )
            if (
                diag_channel != CHANNEL_CONTROL
                or not diag_flags & FLAG_RESPONSE
                or diag_sequence != 3
            ):
                raise RuntimeError("unexpected diagnostic GET_INFO response routing")
            counters_after = parse_info(diag_payload, announce=False)
            if counters_before is not None and counters_after is not None:
                delta = tuple((after - before) & 0xFFFFFFFF for before, after in zip(counters_before, counters_after))
                if len(delta) >= 7:
                    print(
                        "Link counters after timeout: "
                        f"queued={delta[0]}, transmitted={delta[1]}, "
                        f"transactions={delta[2]}, STM32 idle={delta[3]}, "
                        f"STM32 response={delta[4]}, invalid={delta[5]}, "
                        f"SPI->TCP={delta[6]}"
                    )
                else:
                    print(
                        "Link counters after timeout: "
                        f"TCP->SPI={delta[0]}, SPI transactions={delta[1]}, "
                        f"valid STM32 frames={delta[2]}, SPI->TCP={delta[3]}"
                    )
            raise SystemExit("STM32 bridge response timed out; use the counters above to locate the break")

    if channel != CHANNEL_DAP or not flags & FLAG_RESPONSE or sequence != 2:
        raise RuntimeError("unexpected STM32 DAP_Info response routing")
    if len(payload) < 3 or payload[0] != DAP_INFO or payload[1] == 0:
        raise RuntimeError(f"unexpected STM32 DAP_Info payload: {payload.hex(' ')}")
    if len(payload) != payload[1] + 2:
        raise RuntimeError("incorrect STM32 DAP_Info response length")
    firmware = payload[2:].rstrip(b"\0").decode("ascii", errors="replace")
    print(f"RBlink STM32 bridge online: CMSIS-DAP firmware {firmware}")

    if not args.swd_test:
        return
    if args.swd_clock < 10_000 or args.swd_clock > 10_000_000:
        raise SystemExit("--swd-clock must be between 10000 and 10000000")

    # Give the ESP TCP task time to observe EOF from the bridge-health session;
    # it intentionally permits only one active client.
    time.sleep(0.1)
    swd_link = socket.create_connection((args.host, args.port), args.timeout)
    sequence = 10
    connected = False
    try:
        response = exchange_dap(
            swd_link, sequence, bytes([DAP_CONNECT, DAP_PORT_SWD])
        )
        sequence += 1
        if response[1] != DAP_PORT_SWD:
            raise RuntimeError(f"target SWD connect failed: {response.hex(' ')}")
        connected = True

        checked_dap(
            swd_link,
            sequence,
            bytes([DAP_TRANSFER_CONFIGURE, 0, 100, 0, 0, 0]),
        )
        sequence += 1
        checked_dap(
            swd_link,
            sequence,
            bytes([DAP_SWJ_CLOCK]) + struct.pack("<I", args.swd_clock),
        )
        sequence += 1
        checked_dap(swd_link, sequence, bytes([DAP_SWD_CONFIGURE, 0]))
        sequence += 1

        for bits, data in (
            (64, bytes([0xFF] * 8)),
            (16, bytes([0x9E, 0xE7])),
            (64, bytes([0xFF] * 8)),
            (8, bytes([0x00])),
        ):
            checked_dap(
                swd_link,
                sequence,
                bytes([DAP_SWJ_SEQUENCE, bits]) + data,
            )
            sequence += 1

        response = exchange_dap(
            swd_link,
            sequence,
            bytes([DAP_TRANSFER, 0, 1, DP_IDCODE_READ]),
        )
        sequence += 1
        if len(response) < 7 or response[1] != 1 or response[2] & 7 != 1:
            ack = response[2] & 7 if len(response) > 2 else 0
            ack_detail = {
                0: "no ACK bits received",
                2: "WAIT",
                4: "FAULT",
                7: "invalid ACK / SWDIO stayed high (check target power, VTREF, GND, and SWDIO direction)",
            }.get(ack, "unknown ACK")
            raise RuntimeError(
                f"SWD DP IDCODE read failed: ACK=0x{ack:X} ({ack_detail})"
            )
        dp_idcode = struct.unpack_from("<I", response, 3)[0]
        if dp_idcode in (0, 0xFFFFFFFF):
            raise RuntimeError(f"invalid SWD DP IDCODE: 0x{dp_idcode:08X}")
        print(
            f"RBlink wireless SWD online: DP IDCODE=0x{dp_idcode:08X}, "
            f"clock={args.swd_clock} Hz"
        )
    finally:
        if connected:
            try:
                exchange_dap(swd_link, sequence, bytes([DAP_DISCONNECT]))
            except Exception:
                pass
        swd_link.close()


if __name__ == "__main__":
    main()
