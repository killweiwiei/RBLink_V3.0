#!/usr/bin/env python3
"""Probe the ESP32-C3 RBlink transport without requiring STM32 services."""

from __future__ import annotations

import argparse
import socket
import struct
import zlib

FRAME_SIZE = 512
MAGIC = 0x4B4C4252
VERSION = 1
CHANNEL_CONTROL = 0
FLAG_RESPONSE = 1
OP_GET_INFO = 2
HEADER = struct.Struct("<IBBBBIHHI")


def make_frame(sequence: int, payload: bytes) -> bytes:
    if len(payload) > FRAME_SIZE - HEADER.size:
        raise ValueError("payload is too long")
    frame = bytearray(FRAME_SIZE)
    HEADER.pack_into(
        frame, 0, MAGIC, VERSION, CHANNEL_CONTROL, 0, 0, sequence,
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=3240)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), args.timeout) as link:
        link.sendall(make_frame(1, bytes([OP_GET_INFO])))
        channel, flags, sequence, payload = validate_frame(
            receive_exact(link, FRAME_SIZE)
        )

    if channel != CHANNEL_CONTROL or not flags & FLAG_RESPONSE or sequence != 1:
        raise RuntimeError("unexpected GET_INFO response routing")
    if len(payload) != 8 or payload[0] != OP_GET_INFO or payload[1] != 0:
        raise RuntimeError(f"unexpected GET_INFO payload: {payload.hex(' ')}")
    capabilities = int.from_bytes(payload[6:8], "little")
    print(
        f"RBlink ESP online: transport v{payload[2]}, "
        f"firmware {payload[3]}.{payload[4]}.{payload[5]}, "
        f"capabilities=0x{capabilities:04X}"
    )


if __name__ == "__main__":
    main()
