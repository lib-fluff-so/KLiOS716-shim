#!/usr/bin/env python3
"""Independent packaging assertions for the v0.7.0 raw newc CPIO ramdisk."""
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"KL7PAY0\0"
TARGET = b"ios-kl7-test.bin"


def parse_archive(data: bytes) -> dict[bytes, bytes]:
    files = {}
    off = 0
    while off + 110 <= len(data):
        h = data[off:off + 110]
        if h[:6] not in (b"070701", b"070702"):
            if not any(data[off:]):
                break
            raise AssertionError(f"invalid newc header at offset {off}")
        fields = [int(h[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        file_size, name_size = fields[6], fields[11]
        name_off = off + 110
        data_off = (name_off + name_size + 3) & ~3
        end = data_off + file_size
        assert name_size >= 1 and data_off <= len(data) and end <= len(data)
        name = data[name_off:name_off + name_size]
        body = data[data_off:end]
        if name == b"TRAILER!!!\0":
            off = (end + 3) & ~3
            if off >= len(data):
                break
            if not any(data[off:]):
                break
            continue
        files[name[:-1]] = body
        off = (end + 3) & ~3
    return files


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} ramdisk.cpio payload.bin")
    archive_path, payload_path = map(Path, sys.argv[1:])
    archive = archive_path.read_bytes()
    payload = payload_path.read_bytes()
    assert archive[:6] == b"070701", "ramdisk must be raw newc CPIO, not gzip"
    files = parse_archive(archive)
    assert TARGET in files, "test payload missing from CPIO"
    packed = files[TARGET]
    assert packed[:8] == MAGIC, "payload header magic mismatch"
    size, crc = struct.unpack_from("<II", packed, 8)
    assert size == len(payload), "payload header size mismatch"
    assert packed[16:] == payload, "payload body mismatch"
    assert crc == zlib.crc32(payload) & 0xFFFFFFFF, "payload CRC32 mismatch"
    print(f"RESULT: ramdisk packaging ok; archive={len(archive)} bytes, payload={len(payload)} bytes")


if __name__ == "__main__":
    main()
