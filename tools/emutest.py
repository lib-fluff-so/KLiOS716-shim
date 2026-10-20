#!/usr/bin/env python3
"""Run an EMULATOR_COMPAT AArch64 image in Unicorn and capture the visible framebuffer pages."""
import sys, struct
from pathlib import Path
from unicorn import *
from unicorn.arm64_const import *
from PIL import Image

img = open(sys.argv[1] if len(sys.argv) > 1 else 'out/Image', 'rb').read()
BASE = 0x48000000
FB = 0xFDFFF000
FB_W, FB_H = 1080, 2436
STRIDE_A, STRIDE_B = 4320, 4352
VISIBLE_OFF = 1230 * STRIDE_B  # visible region proved on the physical device
DTB = 0x4F000000
mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
mu.mem_map(0x40000000, 0x10000000)          # RAM 0x40000000..0x50000000
mu.mem_map(0xFD000000, 0x03000000)          # RAM incl. framebuffer
mu.mem_map(0x10007000, 0x1000)              # WDT
mu.mem_write(BASE, img)
dtb = open(sys.argv[2], 'rb').read() if len(sys.argv) > 2 else None
# BOOTCFG: fake initrd with bootconfig tail; patch /chosen/linux,initrd-start/end in the real DTB
import fdt as _fdt
fd = _fdt.parse_dtb(dtb)
INITRD = 0x4E000000
bc = open('proc_bootconfig.txt','rb').read().replace(b' = ', b'=').replace(b'\n', b'\n')
payload = b'\0' * 4096 + bc + struct.pack('<II', len(bc), 0) + b'#BOOTCONFIG\n'
mu.mem_write(INITRD, payload)
ch = fd.get_node('chosen')
for k, v in (('linux,initrd-start', INITRD), ('linux,initrd-end', INITRD + len(payload))):
    ch.remove_property(k) if ch.exist_property(k) else None
    ch.append(_fdt.PropWords(k, v >> 32, v & 0xffffffff))
dtb = fd.to_dtb(version=17)
mu.mem_write(DTB, dtb)

regs = {
 'CURRENTEL': 0x4, 'MPIDR_EL1': 0x80000000, 'MIDR_EL1': 0x412fd050, 'ID_AA64PFR0_EL1': 0x1100,
 'ID_AA64MMFR0_EL1': 0x0000000000101122, 'ID_AA64ISAR0_EL1': 0x0000100010211120, 'ID_AA64ISAR1_EL1': 0x0000000000100001,
 'CNTFRQ_EL0': 13000000, 'SCTLR_EL1': 0x30d00800,
}
tick = [0]
SNAPS = {}
# Four 4-second pages; capture each page near its midpoint.
snap_times = [2, 6, 10, 14]
log = []
def hook_mrs(uc, reg, cp_reg, user_data):
    # cp_reg is a tuple (crn, crm, op0, op1, op2) in unicorn 2.x
    crn, crm, op0, op1, op2 = cp_reg[:5]
    key = (op0, op1, crn, crm, op2)
    table = {
      (3,0,0,0,0): regs['MIDR_EL1'], (3,0,0,0,5): regs['MPIDR_EL1'],
      (3,0,0,4,0): regs['ID_AA64PFR0_EL1'], (3,0,0,7,0): regs['ID_AA64MMFR0_EL1'],
      (3,0,0,6,0): regs['ID_AA64ISAR0_EL1'], (3,0,0,6,1): regs['ID_AA64ISAR1_EL1'],
      (3,3,14,0,0): regs['CNTFRQ_EL0'], (3,0,1,0,0): regs['SCTLR_EL1'], (3,0,4,2,2): regs['CURRENTEL'],
    }
    if key == (3,3,14,0,2):   # CNTVCT_EL0
        tick[0] += 13000000 // 20
        val = tick[0]
        for n, st in enumerate(snap_times):
            if n not in SNAPS and tick[0] >= st * 13000000:
                SNAPS[n] = bytes(uc.mem_read(FB + VISIBLE_OFF, FB_H * STRIDE_B))
    elif key in table:
        val = table[key]
    else:
        log.append(key); val = 0
    uc.reg_write(reg, val)
    return True
mu.hook_add(UC_HOOK_INSN, hook_mrs, None, 1, 0, UC_ARM64_INS_MRS)
mu.reg_write(UC_ARM64_REG_X0, DTB)
try:
    mu.emu_start(BASE, 0, count=400_000_000)
except UcError as e:
    pc = mu.reg_read(UC_ARM64_REG_PC)
    print('stopped:', e, 'pc=', hex(pc), 'off=', hex(pc - BASE), 'x0=', hex(mu.reg_read(UC_ARM64_REG_X0)))
print('unknown sysregs:', set(log))
fb = bytes(mu.mem_read(FB + VISIBLE_OFF, FB_H * STRIDE_B))
from PIL import Image as _I
Path('out').mkdir(parents=True, exist_ok=True)
for n, raw in SNAPS.items():
    # The actual framebuffer stride is 4352, not tightly packed 1080*4=4320.
    im = _I.frombuffer('RGBA', (FB_W, FB_H), raw, 'raw', 'BGRA', STRIDE_B, 1).convert('RGB')
    im.save(f'out/emu_page{n+1}.png'); print('saved page', n+1)
for name, stride in (('A4320', STRIDE_A), ('B4352', STRIDE_B)):
    im = Image.new('RGB', (FB_W, FB_H))
    row = [fb[y*stride:(y*stride)+FB_W*4] for y in range(FB_H)]
    # fast path: build from bytes (little endian XRGB: B,G,R,X)
    data = bytearray()
    for r in row:
        for x in range(FB_W):
            b, g, rr = r[x*4], r[x*4+1], r[x*4+2]
            data += bytes((rr, g, b))
    im = Image.frombytes('RGB', (FB_W, FB_H), bytes(data))
    im.save(f'out/emu_{name}.png')
    print('saved', f'out/emu_{name}.png')
