#!/usr/bin/env python3
import gzip
import pathlib
import subprocess
import sys

binary = pathlib.Path(sys.argv[1])
out_dir = pathlib.Path(sys.argv[2])
payload = (b"KL7 Android kernel decompression regression; " * 2048) + bytes(range(256))

def run_fixture(name: str, stream: bytes) -> None:
    input_path = out_dir / f"{name}.bin"
    expected_path = out_dir / f"{name}.expected"
    input_path.write_bytes(stream)
    expected_path.write_bytes(payload)
    subprocess.run([str(binary), str(input_path), str(expected_path)], check=True)

run_fixture("android-kernel-gzip-dynamic", gzip.compress(payload, compresslevel=9, mtime=0))
run_fixture("android-kernel-gzip-stored", gzip.compress(payload, compresslevel=0, mtime=0))

# Linux legacy LZ4 stream containing one uncompressed block.
block = b"\x02\x21\x4c\x18" + (0x80000000 | len(payload)).to_bytes(4, "little") + payload
run_fixture("android-kernel-lz4-raw-block", block)
print("android kernel fixtures: PASS")
