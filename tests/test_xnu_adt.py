#!/usr/bin/env python3
"""Verify KL7 FDT extraction and the main XNU ADT ABI fields."""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import adt
import mkadt_kl7


def find(node, name):
    for child in node.children:
        if child.prop('name') == name.encode() + b'\0':
            return child
    raise AssertionError(f'ADT child {name!r} not found below {node.name!r}')


def one_u32(node, prop):
    raw = node.prop(prop)
    assert raw is not None and len(raw) == 4, (node.name, prop, raw)
    return struct.unpack('<I', raw)[0]


def one_u64(node, prop):
    raw = node.prop(prop)
    assert raw is not None and len(raw) == 8, (node.name, prop, raw)
    return struct.unpack('<Q', raw)[0]


def main():
    facts = mkadt_kl7.parse_kl7_facts(ROOT / 'fdt.dtb')
    assert facts['phys_base'] == 0x140000000
    assert facts['mem_size'] == 0xfffff000
    assert (facts['gicd'], facts['gicd_size']) == (0x0c000000, 0x40000)
    assert (facts['gicr'], facts['gicr_size']) == (0x0c040000, 0x200000)
    assert facts['timebase'] == 13_000_000
    assert facts['virtual_timer_intid'] == 27
    assert (facts['ufs'], facts['ufs_size']) == (0x11270000, 0x2300)
    assert (facts['fb'], facts['fb_size']) == (0xfdfff000, 0x1ff8000)

    seed = bytes(range(64))
    root = mkadt_kl7.build(facts, product_adt=None, seed=seed)
    blob = adt.serialize(root)
    parsed, end = adt.parse(blob)
    assert end == len(blob)
    assert parsed.name == 'device-tree'
    compat = parsed.prop('compatible').rstrip(b'\0').split(b'\0')
    assert b'D22AP' in compat
    assert b'tecno,kl7' in compat
    assert parsed.prop('model') == b'iPhone10,3\0'

    chosen = find(parsed, 'chosen')
    assert one_u64(chosen, 'dram-base') == 0x140000000
    assert one_u64(chosen, 'dram-size') == 0xffeff000
    assert one_u32(chosen, 'embedded-panic-log-size') == 0x100000
    assert chosen.prop('random-seed') == seed

    cpus = find(parsed, 'cpus')
    cpu_nodes = [n for n in cpus.children if n.name.startswith('cpu')]
    assert len(cpu_nodes) == 8
    assert [one_u32(n, 'reg') for n in cpu_nodes] == [i << 8 for i in range(8)]

    arm_io = find(parsed, 'arm-io')
    gic = find(arm_io, 'gic')
    assert one_u32(gic, 'virtual-timer-intid') == 27
    reg = gic.prop('reg')
    assert reg is not None and len(reg) == 32
    values = struct.unpack('<4Q', reg)
    assert values == (0x0b000000, 0x40000, 0x0b040000, 0x200000)

    ufs = find(parsed, 'kl7-ufs')
    assert struct.unpack('<4Q', ufs.prop('reg')) == (
        0x11270000, 0x2300, 0x10001000, 0x1000)
    pram = find(parsed, 'pram')
    assert struct.unpack('<2Q', pram.prop('reg')) == (0x23feff000, 0x100000)
    print(f'RESULT: KL7 ADT generation checks passed ({len(blob)} bytes)')


if __name__ == '__main__':
    main()
