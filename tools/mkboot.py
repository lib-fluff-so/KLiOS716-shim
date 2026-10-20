#!/usr/bin/env python3
"""Pack an Android boot image, header v4 (matches the stock boot_a layout: page 4096, hdr size 1584)."""
import struct, sys, gzip, io
def align(n, a=4096): return (n + a - 1) // a * a
def main(kernel, ramdisk, out, os_version=0x200001a9):
    k = open(kernel, 'rb').read(); r = open(ramdisk, 'rb').read()
    hdr = b'ANDROID!' + struct.pack('<IIII', len(k), len(r), os_version, 1584)
    hdr += b'\0' * 16 + struct.pack('<I', 4) + b'\0' * 1536 + struct.pack('<I', 0)
    assert len(hdr) == 1584, len(hdr)
    img = hdr.ljust(4096, b'\0') + k.ljust(align(len(k)), b'\0') + r.ljust(align(len(r)), b'\0')
    open(out, 'wb').write(img)
    print(out, len(img), 'bytes; kernel', len(k), 'ramdisk', len(r))
if __name__ == '__main__':
    main(*sys.argv[1:4])
