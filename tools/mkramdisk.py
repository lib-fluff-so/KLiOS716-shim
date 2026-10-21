#!/usr/bin/env python3
"""Build a raw newc CPIO ramdisk containing a CRC-protected test payload.

The bootloader passes initrd bytes to the shim. Keeping the archive uncompressed
lets the freestanding loader inspect it without embedding a gzip/DEFLATE engine.
The payload header is: 8-byte magic, uint32 payload size, uint32 IEEE CRC32.
"""
import gzip  # retained for easy comparison/debugging of produced bytes
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"KL7PAY0\0"
PAYLOAD_NAME = b"ios-kl7-test.bin"
TRAILER_NAME = b"TRAILER!!!"


def pad4(blob: bytes) -> bytes:
    return blob + b"\0" * ((-len(blob)) & 3)


def newc_entry(name: bytes, data: bytes, ino: int, mode: int) -> bytes:
    name_nul = name + b"\0"
    fields = (
        ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name_nul), 0
    )
    header = b"070701" + b"".join(f"{v:08X}".encode("ascii") for v in fields)
    assert len(header) == 110
    return pad4(header + name_nul) + pad4(data)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} OUTPUT.cpio PAYLOAD.bin")
    out_path, payload_path = map(Path, sys.argv[1:])
    payload = payload_path.read_bytes()
    if not payload:
        raise SystemExit("refusing to package an empty payload")
    packed = MAGIC + struct.pack("<II", len(payload), zlib.crc32(payload) & 0xFFFFFFFF) + payload
    archive = newc_entry(PAYLOAD_NAME, packed, 1, 0o100755)
    archive += newc_entry(TRAILER_NAME, b"", 0, 0)
    archive += b"\0" * ((-len(archive)) & 511)
    out_path.write_bytes(archive)
    print(f"{out_path}: raw newc CPIO, {len(archive)} bytes; payload {len(payload)} bytes, CRC32={zlib.crc32(payload) & 0xFFFFFFFF:08x}")


if __name__ == "__main__":
    main()
