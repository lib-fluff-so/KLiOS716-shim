#!/usr/bin/env python3
"""Build a raw newc CPIO ramdisk carrying either the test blob or XNU container.

Default mode preserves the v0.7.1 CRC-wrapped test payload:
  mkramdisk.py OUTPUT.cpio PAYLOAD.bin

XNU mode stores the SWEETXNU v1 container unchanged (the C loader validates its
structure before touching high RAM):
  mkramdisk.py OUTPUT.cpio XNU_PAYLOAD.bin --xnu
"""
import argparse
import struct
import zlib
from pathlib import Path

TEST_MAGIC = b"KL7PAY0\0"
TEST_PAYLOAD_NAME = b"ios-kl7-test.bin"
XNU_PAYLOAD_NAME = b"ios-kl7-xnu.bin"
TRAILER_NAME = b"TRAILER!!!"


def pad4(blob: bytes) -> bytes:
    return blob + b"\0" * ((-len(blob)) & 3)


def newc_entry(name: bytes, data: bytes, ino: int, mode: int) -> bytes:
    name_nul = name + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name_nul), 0)
    header = b"070701" + b"".join(f"{v:08X}".encode("ascii") for v in fields)
    assert len(header) == 110
    return pad4(header + name_nul) + pad4(data)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    ap.add_argument('payload', type=Path)
    ap.add_argument('--xnu', action='store_true', help='store an unwrapped SWEETXNU container')
    args = ap.parse_args()

    payload = args.payload.read_bytes()
    if not payload:
        raise SystemExit('refusing to package an empty payload')
    if len(payload) > 0x10000000:
        raise SystemExit('payload exceeds the current shim CPIO limit (256 MiB)')

    if args.xnu:
        name = XNU_PAYLOAD_NAME
        packed = payload
    else:
        name = TEST_PAYLOAD_NAME
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        packed = TEST_MAGIC + struct.pack('<II', len(payload), crc) + payload

    archive = newc_entry(name, packed, 1, 0o100755)
    archive += newc_entry(TRAILER_NAME, b'', 0, 0)
    archive += b'\0' * ((-len(archive)) & 511)
    args.output.write_bytes(archive)
    if args.xnu:
        print(f'{args.output}: raw newc CPIO, {len(archive)} bytes; SWEETXNU container {len(payload)} bytes')
    else:
        print(f'{args.output}: raw newc CPIO, {len(archive)} bytes; test payload {len(payload)} bytes, CRC32={zlib.crc32(payload) & 0xFFFFFFFF:08x}')


if __name__ == '__main__':
    main()
