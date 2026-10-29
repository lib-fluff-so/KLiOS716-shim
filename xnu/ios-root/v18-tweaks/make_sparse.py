#!/usr/bin/env python3
"""Convert a raw disk image to an Android sparse image, skipping zero chunks."""
import os
import struct
import sys

SRC = sys.argv[1]
DST = sys.argv[2]
BLK = 4096
CHUNK = 4 * 1024 * 1024
RAW = 0xCAC1
SKIP = 0xCAC3

size = os.path.getsize(SRC)
total_blks = (size + BLK - 1) // BLK
if total_blks > 0xFFFFFFFF:
    raise SystemExit("image too large for Android sparse format")

with open(SRC, "rb") as fsrc, open(DST, "wb") as fdst:
    fdst.write(struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, BLK,
                           total_blks, 0, 0))
    chunks = 0
    raw_chunks = 0
    skipped_chunks = 0
    copied = 0
    off = 0
    while off < size:
        n = min(CHUNK, size - off)
        data = fsrc.read(n)
        if len(data) != n:
            raise SystemExit(f"short read at {off}: {len(data)} != {n}")
        blocks = (n + BLK - 1) // BLK
        if blocks * BLK > n:
            data += b"\0" * (blocks * BLK - n)
        if data == b"\0" * len(data):
            fdst.write(struct.pack("<HHII", SKIP, 0, blocks, 12))
            skipped_chunks += 1
        else:
            fdst.write(struct.pack("<HHII", RAW, 0, blocks, 12 + len(data)))
            fdst.write(data)
            raw_chunks += 1
            copied += len(data)
        chunks += 1
        off += n
        if chunks % 256 == 0:
            print(f"{off / (1 << 30):.1f} GiB scanned, {copied / (1 << 30):.1f} GiB raw", flush=True)
    fdst.seek(0)
    fdst.write(struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, BLK,
                           total_blks, chunks, 0))
print(f"done: {chunks} chunks, {raw_chunks} raw, {skipped_chunks} skip, "
      f"{copied / (1 << 30):.2f} GiB raw data, size={os.path.getsize(DST)}")
