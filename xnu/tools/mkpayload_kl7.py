#!/usr/bin/env python3
"""Pack an XNU kernel Mach-O + KL7 ADT into the payload the KL7 shim boots.

  mkpayload_kl7.py KERNEL ADT OUT [--cmdline "..."] [--ramdisk IMAGE]

The kernel is linked as a PIE with LC_DYLD_CHAINED_FIXUPS. Nobody else
(no kmutil, no iBoot) resolves those for us, and xnu only rebases static
kernelcaches itself, so the chains are walked here and every pointer is
rewritten to its unslid target. The shim then only copies flat segments.

Payload layout (little endian, all offsets from payload start):
  0x000  char  magic[8] = "SWEETXNU"
  0x008  u32   version = 1, nsegs
  0x010  u64   virt_base      kernel VA that maps to phys_base
  0x018  u64   entry_va       LC_UNIXTHREAD pc
  0x020  u64   adt_off, adt_size
  0x030  u64   kernel_end_va  end of the highest segment (page aligned)
  0x038  u64   phys_base      kernel-managed RAM (same values as the ADT's dram-base/size)
  0x040  u64   mem_size
  0x048  char  cmdline[608]
  0x2a8  seg[nsegs] { u64 vmaddr, vmsize, file_off(in payload), file_size }

--ramdisk appends IMAGE as one more segment right after the kernel and moves
kernel_end past it, so the shim needs no changes: the image lands below the
ADT and boot_args, inside topOfKernelData (never handed to the VM), and the
ADT gets /chosen/memory-map "RAMDisk" = <phys, size> for IOFindBSDRoot to
turn into md0 (boot with rd=md0).
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

LC_SEGMENT_64, LC_UNIXTHREAD, LC_DYLD_CHAINED_FIXUPS = 0x19, 0x5, 0x80000034
PAGE = 0x4000
BOOT_LINE_LENGTH = 608
CMDLINE = 0x48
SEG_TABLE = CMDLINE + BOOT_LINE_LENGTH	# 0x2a8


def parse_macho(buf):
    if len(buf) < 32:
        raise ValueError('Mach-O is shorter than its 64-bit header')
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', buf, 0)
    if magic != 0xfeedfacf:
        raise ValueError('not a little-endian 64-bit Mach-O')
    if cputype != 0x0100000c:
        raise ValueError(f'kernel Mach-O must be arm64 (cputype 0x0100000c), got {cputype:#x}')
    # Kernel collections may be MH_EXECUTE, MH_KEXT_BUNDLE, MH_DYLIB or
    # MH_FILESET depending on the iOS generation. The load commands and ARM64
    # entry point are the actual requirements for this packer.
    if sizeofcmds > len(buf) - 32:
        raise ValueError('Mach-O load-command area exceeds file size')
    segs, entry, fixups = [], None, None
    off = 32
    commands_end = 32 + sizeofcmds
    for _ in range(ncmds):
        if off + 8 > commands_end:
            raise ValueError('truncated Mach-O load command header')
        cmd, size = struct.unpack_from('<II', buf, off)
        if size < 8 or off + size > commands_end:
            raise ValueError(f'invalid Mach-O load command size {size} at {off:#x}')
        if cmd == LC_SEGMENT_64:
            if size < 72:
                raise ValueError('LC_SEGMENT_64 is shorter than 72 bytes')
            name = buf[off + 8:off + 24].split(b'\0')[0].decode(errors='replace')
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', buf, off + 24)
            # __CTF is non-mapped debug metadata in some Apple kernel Mach-Os.
            # It can have file bytes with vmsize == 0; keep its load-command slot
            # (the chained-fixups table indexes Mach-O segments), but do not load it.
            if filesize > vmsize and not (name == '__CTF' and vmsize == 0):
                raise ValueError(f'{name}: filesize exceeds vmsize')
            if fileoff > len(buf) or filesize > len(buf) - fileoff:
                raise ValueError(f'{name}: file range extends beyond Mach-O')
            if vmsize and vmaddr + vmsize > 0xffffffffffffffff:
                raise ValueError(f'{name}: virtual range overflows')
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize,
                             fileoff=fileoff, filesize=filesize))
        elif cmd == LC_UNIXTHREAD:
            if size < 16 + 33 * 8:
                raise ValueError('LC_UNIXTHREAD is too short for ARM_THREAD_STATE64')
            flavor, count = struct.unpack_from('<II', buf, off + 8)
            if flavor != 6 or count < 68:
                raise ValueError('LC_UNIXTHREAD does not contain ARM_THREAD_STATE64')
            # arm_thread_state64: x0..x28, fp, lr, sp, pc, cpsr
            entry, = struct.unpack_from('<Q', buf, off + 16 + 32 * 8)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            if size < 16:
                raise ValueError('LC_DYLD_CHAINED_FIXUPS is shorter than 16 bytes')
            fixups = struct.unpack_from('<II', buf, off + 8)  # dataoff, datasize
        off += size
    if off != commands_end:
        raise ValueError('Mach-O ncmds/sizeofcmds mismatch')
    if not segs:
        raise ValueError('Mach-O contains no LC_SEGMENT_64 segments')
    if segs[0]['name'] != '__TEXT' or segs[0]['fileoff'] != 0:
        raise ValueError('first Mach-O segment must be __TEXT with fileoff=0')
    if entry is None:
        raise ValueError('Mach-O has no LC_UNIXTHREAD entry point')
    if not any(s['vmaddr'] <= entry < s['vmaddr'] + s['vmsize'] for s in segs if s['vmsize']):
        raise ValueError(f'Mach-O entry {entry:#x} is outside all segments')
    if len(segs) > 64:
        raise ValueError(f'Mach-O has {len(segs)} segments; SWEETXNU v1 supports at most 64')
    ordered = sorted((s['vmaddr'], s['vmaddr'] + s['vmsize'], s['name']) for s in segs if s['vmsize'])
    for prev, cur in zip(ordered, ordered[1:]):
        if cur[0] < prev[1]:
            raise ValueError(f'Mach-O segments overlap: {prev[2]} and {cur[2]}')
    return dict(cputype=cputype, cpusub=cpusub & 0xff, segs=segs, entry=entry, fixups=fixups)


def apply_chained_fixups(buf, m):
    """Rewrite every chained rebase in buf (the whole file) to its plain target VA."""
    if not m['fixups']:
        return 0
    data_off, data_size = m['fixups']
    if data_off > len(buf) or data_size > len(buf) - data_off or data_size < 28:
        raise ValueError('LC_DYLD_CHAINED_FIXUPS data range is outside the Mach-O')
    _, starts_off, _, _, imports_count, _, _ = struct.unpack_from('<7I', buf, data_off)
    if imports_count != 0:
        raise ValueError('kernel has chained fixup binds; only rebases are supported')
    if starts_off > data_size - 4:
        raise ValueError('chained-fixups starts offset is out of range')
    image_base = m['segs'][0]['vmaddr']		# __TEXT, holds the mach header
    seg_count, = struct.unpack_from('<I', buf, data_off + starts_off)
    if seg_count > len(m['segs']) or 4 + 4 * seg_count > data_size - starts_off:
        raise ValueError('chained-fixups segment starts table is out of range')
    nfix = 0
    for i in range(seg_count):
        seg_info, = struct.unpack_from('<I', buf, data_off + starts_off + 4 + 4 * i)
        if not seg_info:
            continue
        base = data_off + starts_off + seg_info
        if seg_info > data_size - starts_off or base + 22 > data_off + data_size:
            raise ValueError('chained-fixups segment starts info is out of range')
        size, page_size, fmt, seg_offset, max_valid, page_count = struct.unpack_from('<IHHQIH', buf, base)
        if size < 22 or size > data_off + data_size - base or 22 + 2 * page_count > size:
            raise ValueError('chained-fixups page-start table is out of range')
        if i >= len(m['segs']):
            raise ValueError('chained-fixups refers to a nonexistent segment')
        starts = struct.unpack_from('<%dH' % page_count, buf, base + 22)
        seg = m['segs'][i]
        for p, start in enumerate(starts):
            if start == 0xffff:
                continue
            assert not start & 0x8000, 'DYLD_CHAINED_PTR_START_MULTI not supported'
            if page_size == 0 or start >= page_size:
                raise ValueError('invalid chained-fixups page size/start')
            loc = seg['fileoff'] + p * page_size + start
            while True:
                if loc + 8 > seg['fileoff'] + seg['filesize'] or loc + 8 > len(buf):
                    raise ValueError('chained fixup location exceeds segment file data')
                raw, = struct.unpack_from('<Q', buf, loc)
                target, nxt, stride = decode(raw, fmt, image_base)
                struct.pack_into('<Q', buf, loc, target)
                nfix += 1
                if not nxt:
                    break
                step = nxt * stride
                if step <= 0 or loc > 0xffffffffffffffff - step:
                    raise ValueError('chained-fixup next pointer overflows')
                loc += step
    return nfix


def decode(raw, fmt, image_base):
    """Return (unslid target VA, next, stride) for one chained pointer."""
    if fmt in (2, 6):		# DYLD_CHAINED_PTR_64 / _64_OFFSET
        assert not raw >> 63, 'bind in kernel'
        target = raw & ((1 << 36) - 1)
        high8 = (raw >> 36) & 0xff
        nxt = (raw >> 51) & 0xfff
        if fmt == 6:
            target += image_base
        return target | (high8 << 56), nxt, 4
    if fmt == 8:		# DYLD_CHAINED_PTR_64_KERNEL_CACHE
        target = raw & ((1 << 30) - 1)
        nxt = (raw >> 51) & 0xfff
        return image_base + target, nxt, 4
    if fmt in (1, 7, 9, 12):	# arm64e variants (stock VMAPPLE kernel only; PAC is not re-signed)
        auth = raw >> 63
        nxt = (raw >> 51) & 0x7ff
        stride = 4 if fmt in (7, 12) else 8
        if auth:
            target = raw & 0xffffffff
        else:
            target = raw & ((1 << 43) - 1)
            target |= ((raw >> 43) & 0xff) << 56
        if fmt in (7, 9, 12) or auth:
            target += image_base
        return target, nxt, stride
    raise ValueError(f'unsupported chained pointer format {fmt}')


def add_ramdisk_to_adt(adt_blob, phys, size):
    """Return the ADT with /chosen/memory-map RAMDisk = <phys, size> (two uintptr_t)."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    mmap = next(c for c in chosen.children if c.name == 'memory-map')
    assert mmap.prop('RAMDisk') is None, 'ADT already has a RAMDisk'
    mmap.props.append(('RAMDisk', 0, struct.pack('<2Q', phys, size)))
    return adt.serialize(root)


def adt_dram(adt_blob):
    """Kernel-managed RAM <base, size> from the ADT's /chosen dram-base/dram-size."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    return (struct.unpack('<Q', chosen.prop('dram-base'))[0],
            struct.unpack('<Q', chosen.prop('dram-size'))[0])


def build(kernel_path, adt_path, cmdline, ramdisk_path=None):
    buf = bytearray(open(kernel_path, 'rb').read())
    m = parse_macho(buf)
    nfix = apply_chained_fixups(buf, m)
    segs = [s for s in m['segs'] if s['vmsize']]
    if not segs:
        raise ValueError('Mach-O has no non-empty segments')
    text = m['segs'][0]
    # virt_base: 32MB (16K-granule L2 block) below the mach header, so start.s's
    # block mapping that starts at the header's block begins exactly at phys_base.
    virt_base = text['vmaddr'] & ~(0x2000000 - 1)
    kernel_end = max(s['vmaddr'] + s['vmsize'] for s in segs)
    kernel_end = (kernel_end + PAGE - 1) & ~(PAGE - 1)

    adt_blob = open(adt_path, 'rb').read()
    # The ADT decides which part of RAM is managed by XNU.
    PHYS_BASE, MEM_SIZE = adt_dram(adt_blob)
    if not PHYS_BASE or not MEM_SIZE or PHYS_BASE & (PAGE - 1):
        raise ValueError(f'ADT has invalid dram-base/dram-size: {PHYS_BASE:#x}+{MEM_SIZE:#x}')
    if PHYS_BASE + MEM_SIZE > 0xffffffffffffffff:
        raise ValueError('ADT physical RAM range overflows')
    if kernel_end - virt_base > MEM_SIZE:
        raise ValueError('Mach-O segments exceed the ADT-managed RAM size')
    pieces = [(s['vmaddr'], s['vmsize'], buf[s['fileoff']:s['fileoff'] + s['filesize']]) for s in segs]
    ramdisk = None
    if ramdisk_path:
        image = open(ramdisk_path, 'rb').read()
        if not image:
            raise ValueError('root ramdisk is empty')
        rd_size = (len(image) + PAGE - 1) & ~(PAGE - 1)
        rd_va = kernel_end
        kernel_end = rd_va + rd_size
        if kernel_end - virt_base > MEM_SIZE:
            raise ValueError('kernel plus root ramdisk exceed the ADT-managed RAM size')
        rd_pa = rd_va - virt_base + PHYS_BASE
        pieces.append((rd_va, rd_size, image))
        adt_blob = add_ramdisk_to_adt(adt_blob, rd_pa, rd_size)
        ramdisk = (hex(rd_pa), hex(rd_size))

    if len(pieces) > 64:
        raise ValueError(f'payload has {len(pieces)} segments; maximum is 64')
    hdr_size = SEG_TABLE + 32 * len(pieces)
    out = bytearray(hdr_size)
    seg_table = []
    for vmaddr, vmsize, data in pieces:
        off = len(out)
        out += data
        out += b'\0' * (-len(out) & 0xf)
        seg_table.append((vmaddr, vmsize, off, len(data)))
    adt_off = len(out)
    out += adt_blob
    out += b'\0' * (-len(out) & 0xf)

    struct.pack_into('<8sII', out, 0, b'SWEETXNU', 1, len(pieces))
    struct.pack_into('<7Q', out, 0x10, virt_base, m['entry'], adt_off, len(adt_blob), kernel_end,
                     PHYS_BASE, MEM_SIZE)
    cl = cmdline.encode()[:BOOT_LINE_LENGTH - 1]
    out[CMDLINE:CMDLINE + len(cl)] = cl
    for i, t in enumerate(seg_table):
        struct.pack_into('<4Q', out, SEG_TABLE + 32 * i, *t)
    info = dict(cputype=hex(m['cputype']), cpusub=m['cpusub'], fixups=nfix, virt_base=hex(virt_base),
                entry=hex(m['entry']), kernel_end=hex(kernel_end), ramdisk=ramdisk, size=len(out),
                segs=[(s['name'], hex(s['vmaddr']), hex(s['vmsize'])) for s in segs])
    return bytes(out), info


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('kernel'); ap.add_argument('adt'); ap.add_argument('out')
    ap.add_argument('--cmdline', default='debug=0xc serial=3 cpus=1 keepsyms=1')
    ap.add_argument('--ramdisk', help='root image for md0 (boot with rd=md0)')
    a = ap.parse_args()
    blob, info = build(a.kernel, a.adt, a.cmdline, a.ramdisk)
    open(a.out, 'wb').write(blob)
    for k, v in info.items():
        if k != 'segs':
            print(f'{k}: {v}')
    for s in info['segs']:
        print('  ', *s)
