#!/usr/bin/env python3
"""Minimal gzip'd cpio (newc) with only the TRAILER, so LK has a non-empty ramdisk."""
import gzip, sys
name = b'TRAILER!!!\0'
hdr = b'070701' + b''.join(b'%08X' % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
blob = hdr + name
blob += b'\0' * ((4 - len(blob) % 4) % 4)
blob += b'\0' * ((512 - len(blob) % 512) % 512)
open(sys.argv[1], 'wb').write(gzip.compress(blob, 9, mtime=0))
