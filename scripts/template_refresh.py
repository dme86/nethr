#!/usr/bin/env python3
"""
Capture a reusable pool of Notchian 1.21.11 level_chunk_with_light packets.

The output files are written to:
  assets/chunks/chunk_template_00.bin
  assets/chunks/chunk_template_01.bin
  ...

Each file contains one packet body (packet id + payload), without the outer
frame length varint. The server reuses these packets and patches only chunk x/z.
"""

import argparse
import os
import shutil
import socket
import struct
import time
import zlib

PROTOCOL_VERSION = 774
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 25566
DEFAULT_TARGET = 64
DEFAULT_TIMEOUT = 30.0
DEFAULT_OUTDIR = "assets/chunks"


def encode_varint(value: int) -> bytes:
    value &= 0xFFFFFFFF
    out = bytearray()
    while True:
        if value & ~0x7F == 0:
            out.append(value)
            return bytes(out)
        out.append((value & 0x7F) | 0x80)
        value >>= 7


def encode_string(text: str) -> bytes:
    raw = text.encode("utf-8")
    return encode_varint(len(raw)) + raw


def encode_packet(packet_id: int, payload: bytes = b"") -> bytes:
    body = encode_varint(packet_id) + payload
    return encode_varint(len(body)) + body


def decode_varint(buf: bytes, offset: int = 0):
    value = 0
    shift = 0
    idx = offset
    for _ in range(5):
        if idx >= len(buf):
            return None
        current = buf[idx]
        idx += 1
        value |= (current & 0x7F) << shift
        if (current & 0x80) == 0:
            return value, idx
        shift += 7
    raise ValueError("varint too long")


def recv_exact(sock: socket.socket, length: int) -> bytes:
    out = bytearray()
    while len(out) < length:
        chunk = sock.recv(length - len(out))
        if not chunk:
            return bytes(out)
        out.extend(chunk)
    return bytes(out)


def recv_framed_body(sock: socket.socket):
    header = bytearray()
    while True:
        part = sock.recv(1)
        if not part:
            return None
        header.extend(part)
        parsed = decode_varint(header, 0)
        if parsed is None:
            continue
        body_len, _ = parsed
        body = recv_exact(sock, body_len)
        if len(body) != body_len:
            return None
        return body


def decode_packet(raw_body: bytes, compression_enabled: bool):
    if not compression_enabled:
        parsed = decode_varint(raw_body, 0)
        if parsed is None:
            return None
        packet_id, offset = parsed
        return packet_id, raw_body[offset:], raw_body

    parsed = decode_varint(raw_body, 0)
    if parsed is None:
        return None
    uncompressed_len, offset = parsed
    packet_data = raw_body[offset:]
    if uncompressed_len == 0:
        data = packet_data
    else:
        data = zlib.decompress(packet_data)
        if len(data) != uncompressed_len:
            raise ValueError("invalid decompression length")

    parsed = decode_varint(data, 0)
    if parsed is None:
        return None
    packet_id, packet_offset = parsed
    return packet_id, data[packet_offset:], data


def i32_be(raw: bytes) -> int:
    return struct.unpack(">i", raw)[0]


def ensure_dir(path: str):
    os.makedirs(path, exist_ok=True)


def main():
    parser = argparse.ArgumentParser(description="Capture Notchian chunk templates.")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--target", type=int, default=DEFAULT_TARGET)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--outdir", default=DEFAULT_OUTDIR)
    args = parser.parse_args()

    ensure_dir(args.outdir)
    tmp_outdir = f"{args.outdir}.tmp_refresh"
    if os.path.isdir(tmp_outdir):
        shutil.rmtree(tmp_outdir)
    os.makedirs(tmp_outdir, exist_ok=True)

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
        sock.settimeout(args.timeout)
    except OSError as exc:
        print(f"error: cannot connect to {args.host}:{args.port}: {exc}")
        print("keeping existing templates unchanged.")
        return

    compression_enabled = False

    # Handshake -> Login.
    sock.sendall(
        encode_packet(
            0x00,
            encode_varint(PROTOCOL_VERSION)
            + encode_string("localhost")
            + struct.pack(">H", args.port)
            + encode_varint(2),
        )
    )
    sock.sendall(
        encode_packet(
            0x00,
            encode_string("TemplateProbe")
            + bytes.fromhex("00112233445566778899aabbccddeeff"),
        )
    )

    state = "login"
    chunks = []
    seen_coords = set()
    start = time.time()

    try:
        while time.time() - start < args.timeout and len(chunks) < args.target:
            raw = recv_framed_body(sock)
            if raw is None:
                break
            decoded = decode_packet(raw, compression_enabled)
            if decoded is None:
                break
            packet_id, payload, full = decoded

            if state == "login":
                # Login compression
                if packet_id == 0x03:
                    compression_enabled = True
                    continue
                # Login success -> send Login Acknowledged
                if packet_id == 0x02:
                    sock.sendall(encode_packet(0x03))
                    state = "configuration"
                    continue
                continue

            if state == "configuration":
                # Clientbound known packs -> reply with client info + known packs
                if packet_id == 0x0E:
                    client_info = (
                        encode_string("en_us")
                        + bytes([16])
                        + encode_varint(0)
                        + bytes([1])
                        + bytes([0x7F])
                        + encode_varint(1)
                        + bytes([1])
                        + bytes([1])
                        + encode_varint(0)
                    )
                    sock.sendall(encode_packet(0x00, client_info))
                    known_packs = (
                        encode_varint(1)
                        + encode_string("minecraft")
                        + encode_string("core")
                        + encode_string("1.21.11")
                    )
                    sock.sendall(encode_packet(0x07, known_packs))
                    continue
                # Finish configuration -> acknowledge and enter play
                if packet_id == 0x03:
                    sock.sendall(encode_packet(0x03))
                    state = "play"
                    continue
                continue

            if state == "play" and packet_id == 0x2C and len(full) >= 9:
                # Packet body layout starts with 0x2C + x(i32) + z(i32).
                chunk_x = i32_be(full[1:5])
                chunk_z = i32_be(full[5:9])
                key = (chunk_x, chunk_z)
                if key in seen_coords:
                    continue
                seen_coords.add(key)
                chunks.append((chunk_x, chunk_z, full))
    except OSError as exc:
        print(f"warning: capture interrupted: {exc}")
    finally:
        try:
            sock.close()
        except OSError:
            pass

    for idx, (chunk_x, chunk_z, body) in enumerate(chunks):
        path = os.path.join(tmp_outdir, f"chunk_template_{idx:02d}.bin")
        with open(path, "wb") as file:
            file.write(body)
        print(f"[{idx:02d}] x={chunk_x:4d} z={chunk_z:4d} bytes={len(body):6d} -> {path}")

    print(f"captured {len(chunks)} templates")
    if len(chunks) == 0:
        shutil.rmtree(tmp_outdir, ignore_errors=True)
        print("error: no templates captured; keeping existing templates unchanged.")
        return

    # Atomic-ish swap: keep existing templates until we have a valid new set.
    for name in os.listdir(args.outdir):
        if name.startswith("chunk_template_") and name.endswith(".bin"):
            os.remove(os.path.join(args.outdir, name))
    for name in sorted(os.listdir(tmp_outdir)):
        shutil.move(os.path.join(tmp_outdir, name), os.path.join(args.outdir, name))
    shutil.rmtree(tmp_outdir, ignore_errors=True)

    if len(chunks) < args.target:
        print("warning: captured less than target; walk/fly further on the Notchian probe world and rerun.")


if __name__ == "__main__":
    main()
