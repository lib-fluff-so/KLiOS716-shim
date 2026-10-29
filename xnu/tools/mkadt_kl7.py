#!/usr/bin/env python3
"""Build the minimal Apple Device Tree (ADT) for the KL7/MT6789 XNU port.

Usage:
  tools/mkadt_kl7.py OUT.adt [--dtb fdt.dtb] [--product-adt d22ap.adt] [--dump]

The script reads address, memory-boundary, timer, GIC, framebuffer and UFS
facts from the live KL7 FDT. It emits Apple's ADT format, which is a distinct
format from the Linux FDT passed by the bootloader. /product can be imported
from the real D22AP iPhone X IPSW DeviceTree; without it, the output is useful
for early-kernel work but is not sufficient for full iOS userland identity.
"""
import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

PAGE_16K = 0x4000
PANIC_LOG_SIZE = 0x100000
SOC_BASE = 0x01000000
FB_W, FB_H, FB_STRIDE = 1080, 2436, 4352
KL7_ECID = 0x4B4C375F00000001  # synthetic KL7 identity; never a real device ID
D22AP_ADT_DEFAULT = Path(__file__).resolve().parent.parent / 'research/ipsw-x/d22ap.adt'

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


class FdtNode:
    def __init__(self, name):
        self.name = name
        self.props = {}
        self.children = []

    def child(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()

    def strings(self, prop):
        raw = self.props.get(prop, b'')
        return [x.decode(errors='replace') for x in raw.rstrip(b'\0').split(b'\0') if x]

    def compatible(self):
        return self.strings('compatible')


def parse_fdt(blob):
    if len(blob) < 40:
        raise ValueError('DTB is shorter than its 40-byte header')
    magic, total, off_struct, off_strings, off_rsv, version, last_comp, boot_cpu, size_strings, size_struct = struct.unpack_from('>10I', blob)
    if magic != 0xD00DFEED or total > len(blob) or total < 40:
        raise ValueError('invalid FDT header or totalsize')
    if off_struct + size_struct > total or off_strings + size_strings > total:
        raise ValueError('FDT structure/string block exceeds totalsize')
    strings = blob[off_strings:off_strings + size_strings]
    pos = off_struct
    limit = off_struct + size_struct
    stack = []
    root = None

    def string_at(offset):
        if offset >= len(strings):
            raise ValueError(f'FDT property name offset {offset} out of bounds')
        end = strings.find(b'\0', offset)
        if end < 0:
            raise ValueError('unterminated FDT property name')
        return strings[offset:end].decode(errors='strict')

    while pos + 4 <= limit:
        token = struct.unpack_from('>I', blob, pos)[0]
        pos += 4
        if token == 1:  # FDT_BEGIN_NODE
            end = blob.find(b'\0', pos, limit)
            if end < 0:
                raise ValueError('unterminated FDT node name')
            name = blob[pos:end].decode(errors='replace')
            pos = (end + 4) & ~3
            node = FdtNode(name)
            if stack:
                stack[-1].children.append(node)
            elif root is None:
                root = node
            else:
                raise ValueError('multiple FDT roots')
            stack.append(node)
        elif token == 2:  # FDT_END_NODE
            if not stack:
                raise ValueError('unbalanced FDT_END_NODE')
            stack.pop()
        elif token == 3:  # FDT_PROP
            if pos + 8 > limit or not stack:
                raise ValueError('truncated FDT property header')
            length, nameoff = struct.unpack_from('>II', blob, pos)
            pos += 8
            if pos + length > limit:
                raise ValueError('FDT property extends beyond structure block')
            stack[-1].props[string_at(nameoff)] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:  # FDT_NOP
            continue
        elif token == 9:  # FDT_END
            if stack:
                raise ValueError('FDT ended with unclosed nodes')
            if root is None:
                raise ValueError('FDT has no root node')
            return root
        else:
            raise ValueError(f'unknown FDT token {token} at offset 0x{pos - 4:x}')
    raise ValueError('FDT structure block has no FDT_END token')


def get_reg(node, address_cells, size_cells):
    raw = node.props.get('reg')
    if raw is None:
        raise ValueError(f'{node.name}: no reg property')
    stride = (address_cells + size_cells) * 4
    if not stride or len(raw) % stride:
        raise ValueError(f'{node.name}: reg has invalid length {len(raw)}')
    values = []
    off = 0
    while off < len(raw):
        addr = int.from_bytes(raw[off:off + 4 * address_cells], 'big')
        off += 4 * address_cells
        size = int.from_bytes(raw[off:off + 4 * size_cells], 'big')
        off += 4 * size_cells
        values.append((addr, size))
    return values


def prop_u32(node, name):
    raw = node.props.get(name)
    if raw is None or len(raw) != 4:
        raise ValueError(f'{node.name}: {name} must be exactly one FDT cell')
    return struct.unpack('>I', raw)[0]


def find_compatible(root, needle):
    matches = [n for n in root.walk() if needle in n.compatible()]
    if not matches:
        raise ValueError(f'FDT contains no node compatible with {needle!r}')
    return matches


def parse_kl7_facts(path):
    root = parse_fdt(Path(path).read_bytes())
    root_addr_cells = int.from_bytes(root.props.get('#address-cells', b'\0\0\0\x02'), 'big')
    root_size_cells = int.from_bytes(root.props.get('#size-cells', b'\0\0\0\x02'), 'big')
    mem = root.child('memory')
    if mem is None:
        raise ValueError('FDT has no /memory node')
    mem_ranges = get_reg(mem, root_addr_cells, root_size_cells)
    if not any(base <= 0x140000000 < base + size and 0x240000000 <= base + size for base, size in mem_ranges):
        raise ValueError(f'/memory does not cover the expected KL7 upper DRAM bank: {mem_ranges!r}')

    reserved = root.child('reserved-memory')
    if reserved is None:
        raise ValueError('FDT has no /reserved-memory node')
    raddr = prop_u32(reserved, '#address-cells')
    rsize = prop_u32(reserved, '#size-cells')
    r_by_compat = {}
    for node in reserved.children:
        for comp in node.compatible():
            r_by_compat.setdefault(comp, []).append(node)

    def reserved_reg(compat):
        nodes = r_by_compat.get(compat, [])
        if len(nodes) != 1:
            raise ValueError(f'expected one reserved-memory node with compatible={compat}, got {len(nodes)}')
        return get_reg(nodes[0], raddr, rsize)[0]

    rk0_base, rk0_size = reserved_reg('mediatek,dramc-rk0')
    rk1_base, rk1_size = reserved_reg('mediatek,dramc-rk1')
    fb_base, fb_size = reserved_reg('mediatek,framebuffer')
    phys_base = rk0_base + rk0_size
    mem_top = rk1_base  # rk1 DRAMC marker occupies the final 4 KiB page
    mem_size = mem_top - phys_base
    if phys_base != 0x140000000 or mem_top != 0x23FFFF000 or mem_size <= 0:
        raise ValueError(f'unexpected KL7 rank markers: rk0={(hex(rk0_base),hex(rk0_size))}, rk1={(hex(rk1_base),hex(rk1_size))}')
    if fb_base != 0xFDFFF000 or fb_size < FB_STRIDE * FB_H:
        raise ValueError(f'unexpected KL7 framebuffer region: base={fb_base:#x}, size={fb_size:#x}')

    gic_nodes = find_compatible(root, 'arm,gic-v3')
    if len(gic_nodes) != 1:
        raise ValueError(f'expected exactly one arm,gic-v3 node, got {len(gic_nodes)}')
    gicd, gicr = get_reg(gic_nodes[0], root_addr_cells, root_size_cells)[:2]
    if gicd[0] != 0x0C000000 or gicr[0] != 0x0C040000:
        raise ValueError(f'unexpected KL7 GIC ranges: {gicd!r}, {gicr!r}')

    timers = [n for n in root.walk() if n.name == 'timer']
    if len(timers) != 1:
        raise ValueError(f'expected one /timer node, got {len(timers)}')
    timer = timers[0]
    timebase = prop_u32(timer, 'clock-frequency')
    irqs_raw = timer.props.get('interrupts', b'')
    if len(irqs_raw) < 12 * 4 or len(irqs_raw) % 16:
        raise ValueError('/timer/interrupts does not have four-cell interrupt tuples')
    irq_cells = [int.from_bytes(irqs_raw[i:i + 4], 'big') for i in range(0, len(irqs_raw), 4)]
    # FDT tuple order for armv8-timer is secure physical, non-secure physical,
    # virtual, hypervisor. MediaTek stores each tuple as four cells.
    virtual_ppi = irq_cells[9]
    if irq_cells[8] != 1 or virtual_ppi != 11:
        raise ValueError(f'expected KL7 virtual timer PPI 11, got tuple {irq_cells[8:12]!r}')
    virtual_timer_intid = 16 + virtual_ppi

    ufs_nodes = find_compatible(root, 'mediatek,mt8183-ufshci')
    if len(ufs_nodes) != 1:
        raise ValueError(f'expected one MTK UFS host node, got {len(ufs_nodes)}')
    ufs_base, ufs_size = get_reg(ufs_nodes[0], root_addr_cells, root_size_cells)[0]
    if (ufs_base, ufs_size) != (0x11270000, 0x2300):
        raise ValueError(f'unexpected KL7 UFS region: {ufs_base:#x}+{ufs_size:#x}')

    cpus = root.child('cpus')
    if cpus is None:
        raise ValueError('FDT has no /cpus node')
    cpu_nodes = [n for n in cpus.children if n.props.get('device_type') == b'cpu\0']
    cpu_nodes.sort(key=lambda n: int.from_bytes(n.props.get('reg', b'\0' * 4), 'big'))
    cpu_desc = []
    for n in cpu_nodes:
        if 'reg' not in n.props or len(n.props['reg']) != 4:
            raise ValueError(f'{n.name}: expected one-cell MPIDR reg')
        mpidr = int.from_bytes(n.props['reg'], 'big')
        if mpidr & ~0x700:
            raise ValueError(f'{n.name}: MPIDR is not Aff1-only: {mpidr:#x}')
        comps = n.compatible()
        core_type = 'a76' if 'arm,cortex-a76' in comps else 'a55' if 'arm,cortex-a55' in comps else 'unknown'
        cpu_desc.append((mpidr >> 8, core_type))
    if len(cpu_desc) != 8 or [n[0] for n in cpu_desc] != list(range(8)) or [n[1] for n in cpu_desc] != ['a55'] * 6 + ['a76'] * 2:
        raise ValueError(f'expected MT6789 topology 6xA55 + 2xA76 with MPIDR 0x000..0x700, got {cpu_desc!r}')

    return {
        'phys_base': phys_base,
        'mem_size': mem_size,
        'gicd': gicd[0], 'gicd_size': gicd[1],
        'gicr': gicr[0], 'gicr_size': gicr[1],
        'timebase': timebase, 'virtual_timer_intid': virtual_timer_intid,
        'ufs': ufs_base, 'ufs_size': ufs_size,
        'fb': fb_base, 'fb_size': fb_size,
        'cpus': cpu_desc,
    }


def d22ap_product(path):
    """Return real D22AP /product properties, excluding iBoot placeholders."""
    if path is None:
        return None
    if not path or not Path(path).exists():
        print(f'warning: D22AP /product ADT missing ({path}); omitting /product. Early XNU tests only.', file=sys.stderr)
        return None
    blob = adt.load(str(path))
    root, _ = adt.parse(blob)
    for child in root.children:
        if child.prop('name') == b'product\0':
            return [(name, value) for name, flags, value in child.props
                    if not flags and name not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node in D22AP ADT')


def node(name, props=(), children=()):
    result = adt.Node()
    result.props = [('name', 0, cstr(name))] + [(key, 0, value) for key, value in props]
    result.children = list(children)
    return result


def build(facts, product_adt=None, seed=None, include_ufs=False):
    phys_base = facts['phys_base']
    mem_size = facts['mem_size']
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    kernel_mem_size = mem_size - PANIC_LOG_SIZE
    seed = seed if seed is not None else os.urandom(64)
    if len(seed) != 64:
        raise ValueError('random seed must be exactly 64 bytes')
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))
    manifest_hash = hashlib.sha384(b'kl7 experimental boot manifest').digest()

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(kernel_mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('kl7-shim')),
        ('system-firmware-version', cstr('kl7-shim')),
        ('boot-manifest-hash', manifest_hash),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', KL7_ECID)),
        # Keep the expected iPhone X platform silicon ID; the ECID above is synthetic.
        ('chip-id', u32(0x8015)),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [
        node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))]),
    ])

    cpu_nodes = []
    for core, core_type in facts['cpus']:
        compat = ('arm,cortex-a76' if core_type == 'a76' else 'arm,cortex-a55')
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr(compat) + cstr('ARM,v8')),
            ('reg', u32(core << 8)),
            ('cpu-id', u32(core)),
            # Keep the known sweet-style flat topology for the first SMP milestone;
            # target-specific SMP patches still need to validate the A55/A76 mix.
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(facts['timebase'])),
            ('clock-frequency', u32(2304000000 if core >= 6 else 1800000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(facts['timebase'])),
            ('fixed-frequency', u32(facts['timebase'])),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gicd, gicr = facts['gicd'], facts['gicr']
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('kl7-io')),
        ('compatible', cstr('tecno,kl7-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [
        node('gic', [
            ph(),
            ('compatible', cstr('arm,gic-v3')),
            ('interrupt-controller', b''),
            ('virtual-timer-intid', u32(facts['virtual_timer_intid'])),
            ('reg', u64(gicd - SOC_BASE, facts['gicd_size'], gicr - SOC_BASE, facts['gicr_size'])),
        ]),
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('tecno,kl7-display')),
            ('canvas-width', u32(FB_W)),
            ('canvas-height', u32(FB_H)),
        ]),
    ])

    fbcon = node('kl7-fbcon', [
        ph(),
        ('compatible', cstr('tecno,kl7-framebuffer')),
        ('reg', u64(facts['fb'], facts['fb_size'])),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    ufs = node('kl7-ufs', [
        ph(),
        ('compatible', cstr('mediatek,mt8183-ufshci')),
        ('reg', u64(facts['ufs'] - SOC_BASE, facts['ufs_size'])),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])
    product_props = d22ap_product(product_adt)
    products = [node('product', [ph()] + product_props)] if product_props else []
    pram = node('pram', [ph(), ('reg', u64(panic_base, PANIC_LOG_SIZE))])

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('tecno,kl7') + cstr('mediatek,mt6789') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + products + ([ufs] if include_ufs else []))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', help='output raw Apple Device Tree file')
    ap.add_argument('--dtb', default=str(Path(__file__).resolve().parent.parent / 'fdt.dtb'), help='live KL7 Linux FDT used to verify addresses')
    ap.add_argument('--product-adt', default=str(D22AP_ADT_DEFAULT), help='raw or IM4P-wrapped D22AP Apple Device Tree from IPSW')
    ap.add_argument('--seed-hex', help='optional fixed 64-byte random seed (128 hex characters) for reproducible builds')
    ap.add_argument('--dump', action='store_true', help='dump generated ADT')
    ap.add_argument('--with-ufs', action='store_true', help='include the KL7 UFS node (leave off for early single-core XNU tests)')
    args = ap.parse_args()
    facts = parse_kl7_facts(args.dtb)
    seed = bytes.fromhex(args.seed_hex) if args.seed_hex else None
    blob = adt.serialize(build(facts, args.product_adt, seed, include_ufs=args.with_ufs))
    Path(args.out).write_bytes(blob)
    print(f'{args.out}: {len(blob)} bytes')
    print(f"RAM: {facts['phys_base']:#x}+{facts['mem_size']:#x} (chosen dram-size {facts['mem_size'] - PANIC_LOG_SIZE:#x})")
    print(f"GICD: {facts['gicd']:#x}+{facts['gicd_size']:#x}; GICR: {facts['gicr']:#x}+{facts['gicr_size']:#x}")
    print(f"timer: {facts['timebase']} Hz, virtual INTID {facts['virtual_timer_intid']}")
    print(f"UFS: {facts['ufs']:#x}+{facts['ufs_size']:#x}; FB: {facts['fb']:#x}, {FB_W}x{FB_H}, stride {FB_STRIDE}")
    if args.dump:
        adt.dump(adt.parse(blob)[0])


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, struct.error) as exc:
        raise SystemExit(f'mkadt_kl7.py: error: {exc}')
