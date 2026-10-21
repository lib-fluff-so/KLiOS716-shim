"""Run the EMULATOR_COMPAT AArch64 image and capture its bottom progress strip."""
import sys
from pathlib import Path
from unicorn import *
from unicorn.arm64_const import *
from PIL import Image
import fdt as _fdt
import struct

img = open(sys.argv[1] if len(sys.argv) > 1 else 'out-emu/Image', 'rb').read()
BASE = 0x48000000
FB = 0xFDFFF000
FB_W, FB_H = 1080, 2436
STRIDE_B = 4352
VISIBLE_OFF = 1230 * STRIDE_B
DTB = 0x4F000000
mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
mu.mem_map(0x40000000, 0x10000000)  # RAM 0x40000000..0x50000000
mu.mem_map(0xFD000000, 0x03000000)  # includes framebuffer
mu.mem_map(0x10007000, 0x1000)      # WDT
mu.mem_write(BASE, img)
dtb = open(sys.argv[2] if len(sys.argv) > 2 else 'fdt.dtb', 'rb').read()
fd = _fdt.parse_dtb(dtb)
INITRD = 0x4E000000
bc = open('proc_bootconfig.txt', 'rb').read().replace(b' = ', b'=')
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
raw = bytes(mu.mem_read(FB + VISIBLE_OFF, FB_H * STRIDE_B))
out = Path(sys.argv[3] if len(sys.argv) > 3 else 'out-emu/emu_progress.png')
out.parent.mkdir(parents=True, exist_ok=True)
# The framebuffer's physical pitch is 4352 bytes; only its first 1080 pixels
# per line are visible. This image intentionally captures the current UI once.
im = Image.frombuffer('RGBA', (FB_W, FB_H), raw, 'raw', 'BGRA', STRIDE_B, 1).convert('RGB')
im.save(out)
print('saved progress UI:', out)
