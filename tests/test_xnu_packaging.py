#!/usr/bin/env python3
"""Synthetic Mach-O regression test for mkpayload_xnu.py and XNU CPIO mode."""
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import adt
import mkadt_kl7
import mkpayload_xnu
import mkramdisk


def parse_cpio(data):
    result = {}
    off = 0
    while off + 110 <= len(data):
        h = data[off:off + 110]
        if h[:6] != b'070701':
            if not any(data[off:]):
                break
            raise AssertionError(f'bad CPIO header at {off:#x}')
        fields = [int(h[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        name_off = off + 110
        data_off = (name_off + namesize + 3) & ~3
        end = data_off + size
        assert end <= len(data) and namesize > 0
        name = data[name_off:name_off + namesize]
        if name == b'TRAILER!!!\0':
            off = (end + 3) & ~3
            if off >= len(data) or not any(data[off:]):
                break
            continue
        result[name[:-1]] = data[data_off:end]
        off = (end + 3) & ~3
    return result


def make_macho(path):
    vmaddr = 0xfffffff007800000
    text_filesize = 0x200
    text_vmsize = 0x8000
    text = bytearray(72)
    struct.pack_into('<II', text, 0, 0x19, 72)  # LC_SEGMENT_64
    text[8:24] = b'__TEXT\0' + b'\0' * 9
    struct.pack_into('<QQQQiiII', text, 24, vmaddr, text_vmsize, 0, text_filesize, 7, 5, 0, 0)

    data = bytearray(72)
    struct.pack_into('<II', data, 0, 0x19, 72)
    data[8:24] = b'__DATA\0' + b'\0' * 9
    struct.pack_into('<QQQQiiII', data, 24, vmaddr + text_vmsize, 0x4000, 0x200, 0x80, 3, 3, 0, 0)

    thread = bytearray(280)
    struct.pack_into('<II', thread, 0, 0x5, len(thread))  # LC_UNIXTHREAD
    struct.pack_into('<II', thread, 8, 6, 68)  # ARM_THREAD_STATE64 flavor/count
    struct.pack_into('<Q', thread, 16 + 32 * 8, vmaddr + 0x100)  # PC

    # Apple kernelcaches may contain a non-mapped __CTF metadata segment:
    # it has file bytes but vmsize == 0, and must be ignored by the loader.
    ctf = bytearray(72)
    struct.pack_into('<II', ctf, 0, 0x19, 72)  # LC_SEGMENT_64
    ctf[8:24] = b'__CTF\0' + b'\0' * 10
    struct.pack_into('<QQQQiiII', ctf, 24, 0, 0, 0x300, 0x80, 0, 0, 0, 0)

    commands = bytes(text + data + thread + ctf)
    header = struct.pack('<8I', 0xfeedfacf, 0x0100000c, 2, 2, 4, len(commands), 0, 0)
    blob = bytearray(0x400)
    blob[:32] = header
    blob[32:32 + len(commands)] = commands
    for i in range(0x180, len(blob)):
        blob[i] = (i * 13 + 7) & 0xff
    # Re-apply load commands after filling the test body.
    blob[:32] = header
    blob[32:32 + len(commands)] = commands
    path.write_bytes(blob)


def run(output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    facts = mkadt_kl7.parse_kl7_facts(ROOT / 'fdt.dtb')
    root = mkadt_kl7.build(facts, product_adt=None, seed=bytes(range(64)))
    adt_blob = adt.serialize(root)

    kernel = output_dir / 'synthetic-kernel.macho'
    adt_path = output_dir / 'kl7.adt'
    payload_path = output_dir / 'xnu_payload.bin'
    payload_ramdisk_path = output_dir / 'xnu_payload_ramdisk.bin'
    cpio_path = output_dir / 'xnu.cpio'
    make_macho(kernel)
    adt_path.write_bytes(adt_blob)

    # Parser retains the __CTF slot for segment-index compatibility, while the
    # produced payload contains only the two mapped kernel segments.
    parsed = mkpayload_xnu.parse_macho(bytearray(kernel.read_bytes()))
    assert any(s['name'] == '__CTF' and s['vmsize'] == 0 for s in parsed['segs'])

    payload, info = mkpayload_xnu.build(str(kernel), str(adt_path), 'debug=0xc cpus=1 keepsyms=1')
    payload_path.write_bytes(payload)
    assert payload[:8] == b'SWEETXNU'
    magic, version, nsegs = struct.unpack_from('<8sII', payload, 0)
    assert magic == b'SWEETXNU' and version == 1 and nsegs == 2
    virt_base, entry, adt_off, adt_size, kernel_end, phys_base, mem_size = struct.unpack_from('<7Q', payload, 0x10)
    assert entry == 0xfffffff007800100
    assert phys_base == 0x140000000 and mem_size == 0xffeff000
    assert (kernel_end & 0x3fff) == 0
    assert adt_off + adt_size <= len(payload)
    seg = struct.unpack_from('<4Q', payload, 0x2a8)
    seg2 = struct.unpack_from('<4Q', payload, 0x2a8 + 32)
    assert seg[0] == 0xfffffff007800000 and seg[1] == 0x8000 and seg[3] == 0x200
    assert seg2[0] == 0xfffffff007808000 and seg2[1] == 0x4000 and seg2[3] == 0x80
    assert info['fixups'] == 0

    # Exercise the optional md0/root-ramdisk path as a third physical segment.
    root_image = output_dir / 'root.img'
    root_image.write_bytes((b'KL7ROOTFS' * 23) + b'\0')
    with_ramdisk, ramdisk_info = mkpayload_xnu.build(str(kernel), str(adt_path),
                                                       'debug=0xc rd=md0', str(root_image))
    payload_ramdisk_path.write_bytes(with_ramdisk)
    assert ramdisk_info['ramdisk'] is not None
    rmagic, rversion, rnsegs = struct.unpack_from('<8sII', with_ramdisk, 0)
    assert (rmagic, rversion, rnsegs) == (b'SWEETXNU', 1, 3)
    rroot, rend = adt.parse(with_ramdisk[struct.unpack_from('<Q', with_ramdisk, 0x20)[0]:])
    chosen = next(n for n in rroot.children if n.name == 'chosen')
    mmap = next(n for n in chosen.children if n.name == 'memory-map')
    rd_phys, rd_size = struct.unpack('<2Q', mmap.prop('RAMDisk'))
    assert rd_size == 0x4000 and rd_phys >= 0x140000000

    archive = mkramdisk.newc_entry(mkramdisk.XNU_PAYLOAD_NAME, payload, 1, 0o100755)
    archive += mkramdisk.newc_entry(mkramdisk.TRAILER_NAME, b'', 0, 0)
    archive += b'\0' * ((-len(archive)) & 511)
    cpio_path.write_bytes(archive)
    files = parse_cpio(cpio_path.read_bytes())
    assert files[b'ios-kl7-xnu.bin'] == payload
    print(f'RESULT: 2-segment synthetic Mach-O and optional ramdisk -> SWEETXNU/ADT/CPIO passed ({len(payload)} byte container)')


def main():
    if len(sys.argv) == 2:
        run(Path(sys.argv[1]))
    elif len(sys.argv) == 1:
        with tempfile.TemporaryDirectory(prefix='kl7-xnu-test-') as td:
            run(Path(td))
    else:
        raise SystemExit(f'usage: {Path(sys.argv[0]).name} [OUTPUT_DIR]')


if __name__ == '__main__':
    main()
