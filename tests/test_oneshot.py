"""Simulated phone for tools/oneshot_b.py:  python3 tests/test_oneshot.py"""
import os, struct, sys, tempfile, zlib, io, contextlib
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import oneshot_b as o

class Fake(o.Backend):
    def __init__(self, aosp=True, endian="<"):
        self.misc = bytearray(1 << 20); self.boot_b = bytearray(1 << 20); self.active = 0; self.aosp = aosp; self.e = endian
        if aosp: self.ctl(0)
        else: self.misc[5000:5008] = b"MTKSLOT\x00"
    def ctl(self, active):
        c = bytearray(32); c[0:4] = b"_a\0\0"; struct.pack_into("<I", c, 4, o.MAGIC); c[8] = 1; c[9] = 2
        for i in range(2):
            pri = 15 if i == active else 14 - i
            c[12 + 2 * i] = pri | (7 << 4)
        c[28:32] = struct.pack(self.e + "I", zlib.crc32(bytes(c[:28])) & 0xFFFFFFFF)
        self.misc[o.CTL_OFF:o.CTL_OFF + 32] = c
    def part(self, name): return name
    def run(self, cmd):
        if cmd.startswith("bootctl set-active-boot-slot"):
            self.active = int(cmd.split()[-1])
            if self.aosp: self.ctl(self.active)
            else: self.misc[5008] = self.active + 1
            return b""
        if cmd == "bootctl get-active-boot-slot": return str(self.active).encode()
        if cmd == "bootctl get-current-slot": return b"0"
        return b""
    def read(self, dev, off, n): return bytes((self.misc if dev == "misc" else self.boot_b)[off:off + n])
    def write(self, dev, off, data):
        t = self.misc if dev == "misc" else self.boot_b; t[off:off + len(data)] = data

os.chdir(tempfile.mkdtemp())
img = os.urandom(20480); open("shim.img", "wb").write(img)
quiet = lambda f, *a: contextlib.redirect_stdout(io.StringIO()).__enter__() and None

for endian in "<>":
    d = Fake(True, endian)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf): o.cmd_arm(d, "shim.img", 1, False)
    p = o.parse(bytes(d.misc))
    assert d.boot_b[:len(img)] == img, "image written"
    assert d.active == 1, "B active"
    assert p["crc_ok"] and p["endian"] == endian, "crc valid after patch, same endianness"
    assert p["slots"][1]["tries"] == 1 and p["slots"][1]["successful"] == 0 and p["slots"][1]["priority"] == 15, p
    assert p["slots"][0]["tries"] == 7, "slot A untouched"
    print("arm ok, crc endian", endian, "->", p["slots"][1])

d = Fake(False)
before = bytes(d.misc)
buf = io.StringIO()
try:
    with contextlib.redirect_stdout(buf): o.cmd_arm(d, "shim.img", 1, False)
    raise SystemExit("must refuse on a foreign layout")
except SystemExit as e:
    assert e.code == 1, e.code
changed = [i for i in range(len(before)) if before[i] != d.misc[i]]
assert changed == [5008], f"only the HAL's own byte may differ, got {changed}"
print("foreign layout refused, only the HAL's own byte changed; diff shown:", [l for l in buf.getvalue().splitlines() if "misc+" in l])

d = Fake(True); buf = io.StringIO()
with contextlib.redirect_stdout(buf): o.cmd_probe(d)
assert d.active == 0 and "0 byte(s) differ" in buf.getvalue().splitlines()[-1], buf.getvalue()
print("probe restores slot A with a clean diff")
print("ALL OK")
