#!/usr/bin/env python3
"""One-shot boot of slot B: flash the shim to boot_b, make B active with exactly
N tries (default 1), so the bootloader falls back to slot A by itself after the
shim has run once.

Runs on the PC through adb (default) or on the phone itself as root, e.g. in
Termux with python (--local). Needs root and `bootctl` on the phone.

  oneshot_b.py probe                    show what `bootctl` changes in misc (writes nothing
                                        permanent: slot A is made active again afterwards)
  oneshot_b.py arm out/shim_b.img       flash + arm one attempt (add --reboot to reboot now)
  oneshot_b.py status                   show the slot metadata as parsed

Mechanism (AOSP A/B rules): the bootloader decrements a slot's retry count on each
boot while the slot is not marked successful; at zero the slot is marked unbootable
and the other slot is chosen. `bootctl set-active-boot-slot` resets the count (HAL
default), `arm` then rewrites tries_remaining of slot B in the AOSP
`bootloader_control` block at misc+2048 and fixes its CRC. It refuses to write unless
the block parses (magic 'BCAB', CRC matches) - MTK may keep its own layout; in that
case `probe` prints the byte diff so the layout can be worked out.
"""
import argparse, base64, hashlib, struct, subprocess, sys, time, zlib

CTL_OFF = 2048                  # offsetof(bootloader_message_ab, slot_suffix)
MAGIC = 0x42414342              # 'BCAB'
SLOT_INFO = 12                  # slot_suffix[4] magic[4] version[1] bits[1] reserved[2]
CRC_OFF = 28
MISC_PATHS = ["/dev/block/by-name/misc", "/dev/block/bootdevice/by-name/misc",
              "/dev/block/platform/soc/11270000.ufshci/by-name/misc"]


# ---------------------------------------------------------------- backends --
class Backend:
    def run(self, cmd):             # run as root on the phone, return stdout bytes
        raise NotImplementedError

    def part(self, name):
        for root in ("/dev/block/by-name", "/dev/block/bootdevice/by-name",
                     "/dev/block/platform/soc/11270000.ufshci/by-name"):
            out = self.run(f"ls {root}/{name} 2>/dev/null").decode().strip()
            if out:
                return out
        sys.exit(f"partition {name} not found")

    def read(self, dev, off, n):
        return self.run(f"dd if={dev} bs=1 skip={off} count={n} 2>/dev/null")

    def write(self, dev, off, data):
        b64 = base64.b64encode(data).decode()
        self.run(f"echo {b64} | base64 -d | dd of={dev} bs=1 seek={off} conv=notrunc 2>/dev/null; sync")


class Adb(Backend):
    def run(self, cmd):
        r = subprocess.run(["adb", "exec-out", "su", "-c", cmd], capture_output=True)
        if r.returncode:
            sys.exit(f"adb failed: {r.stderr.decode()}")
        return r.stdout


class Local(Backend):
    def run(self, cmd):
        r = subprocess.run(["su", "-c", cmd], capture_output=True)
        return r.stdout


# ------------------------------------------------------------- metadata ----
def parse(blob):
    """Parse the AOSP bootloader_control block; returns dict or None."""
    c = blob[CTL_OFF:CTL_OFF + 32]
    if len(c) < 32 or struct.unpack_from("<I", c, 4)[0] != MAGIC:
        return None
    crc = c[CRC_OFF:CRC_OFF + 4]
    calc = zlib.crc32(c[:CRC_OFF]) & 0xFFFFFFFF
    if crc == struct.pack("<I", calc):
        endian = "<"
    elif crc == struct.pack(">I", calc):
        endian = ">"
    else:
        return {"crc_ok": False, "raw": c}
    slots = []
    for i in range(4):
        b0, b1 = c[SLOT_INFO + 2 * i], c[SLOT_INFO + 2 * i + 1]
        slots.append({"priority": b0 & 15, "tries": (b0 >> 4) & 7, "successful": b0 >> 7, "verity": b1 & 1})
    return {"crc_ok": True, "endian": endian, "version": c[8], "nb_slot": c[9] & 7, "slots": slots, "raw": c}


def set_tries(blob, endian, slot, tries):
    c = bytearray(blob[CTL_OFF:CTL_OFF + 32])
    o = SLOT_INFO + 2 * slot
    c[o] = (c[o] & 0x8F) | (tries << 4)          # keep priority and successful bit
    c[o] &= 0x7F                                  # successful_boot = 0
    c[CRC_OFF:CRC_OFF + 4] = struct.pack(endian + "I", zlib.crc32(bytes(c[:CRC_OFF])) & 0xFFFFFFFF)
    return bytes(c)


def show(p):
    if p is None:
        print("  no AOSP bootloader_control block at misc+2048 (magic 'BCAB' not found)")
    elif not p["crc_ok"]:
        print("  magic found but CRC does not match (either endianness); raw:", p["raw"].hex())
    else:
        print(f"  AOSP bootloader_control ok (version {p['version']}, {p['nb_slot']} slots, crc {'LE' if p['endian'] == '<' else 'BE'})")
        for i in range(p["nb_slot"] or 2):
            s = p["slots"][i]
            print(f"  slot {'ab'[i]}: priority={s['priority']} tries_remaining={s['tries']} successful={s['successful']} verity_corrupted={s['verity']}")


def diff(a, b, limit=64):
    d = [(i, a[i], b[i]) for i in range(min(len(a), len(b))) if a[i] != b[i]]
    for i, x, y in d[:limit]:
        print(f"  misc+{i:#06x}: {x:02x} -> {y:02x}")
    print(f"  {len(d)} byte(s) differ")
    return d


# --------------------------------------------------------------- commands --
SPAN = 64 * 1024


def cmd_status(be):
    misc = be.part("misc")
    print("active:", be.run("bootctl get-active-boot-slot").decode().strip(), " current:", be.run("bootctl get-current-slot").decode().strip())
    show(parse(be.read(misc, 0, SPAN)))


def cmd_probe(be):
    misc = be.part("misc")
    orig = be.run("bootctl get-active-boot-slot").decode().strip() or "0"
    base = be.read(misc, 0, SPAN)
    open(f"misc_probe_{int(time.time())}.bin", "wb").write(base)
    print(f"active slot is {orig}; misc backup saved (first {SPAN // 1024} KiB)")
    show(parse(base))
    other = "1" if orig == "0" else "0"
    be.run(f"bootctl set-active-boot-slot {other}")
    after = be.read(misc, 0, SPAN)
    print(f"after `bootctl set-active-boot-slot {other}`:")
    show(parse(after))
    diff(base, after)
    be.run(f"bootctl set-active-boot-slot {orig}")
    back = be.read(misc, 0, SPAN)
    print(f"after restoring slot {orig}:")
    diff(base, back)


def cmd_arm(be, image, tries, reboot):
    data = open(image, "rb").read()
    misc, boot_b = be.part("misc"), be.part("boot_b")
    backup = be.read(misc, 0, SPAN)
    bname = f"misc_backup_{int(time.time())}.bin"
    open(bname, "wb").write(backup)
    print("misc backup:", bname)
    pre = parse(backup)
    show(pre)
    if not pre or not pre["crc_ok"]:
        # Nothing has been changed yet: leave it that way. Arming with an unknown
        # layout would leave B active with the HAL's default (7) tries.
        print("ABORT before any change: misc is not in the AOSP bootloader_control layout,")
        print("so the retry count cannot be limited to one. Run `probe` and send its output.")
        sys.exit(1)
    if pre["slots"][1]["successful"]:
        print("note: slot B is currently marked successful; set-active will clear that")

    print(f"flashing {image} ({len(data)} bytes) -> {boot_b}")
    be.write(boot_b, 0, data)
    rb = be.read(boot_b, 0, len(data))
    if hashlib.sha256(rb).digest() != hashlib.sha256(data).digest():
        sys.exit("boot_b read-back mismatch - NOT switching slots")
    print("boot_b verified")

    def back_to_a(why):
        print(why, "- restoring misc backup and making slot A active again")
        be.write(misc, 0, backup)
        be.run("bootctl set-active-boot-slot 0")
        be.write(misc, 0, backup)
        sys.exit(1)

    be.run("bootctl set-active-boot-slot 1")
    after = parse(be.read(misc, 0, SPAN))
    show(after)
    if not after or not after["crc_ok"]:
        back_to_a("layout changed unexpectedly after set-active")
    if not after["slots"][1]["priority"]:
        back_to_a("slot B has priority 0 after set-active")
    blob = be.read(misc, 0, SPAN)
    new = set_tries(blob, after["endian"], 1, tries)
    be.write(misc, CTL_OFF, new)
    chk = parse(be.read(misc, 0, SPAN))
    show(chk)
    if not (chk and chk["crc_ok"] and chk["slots"][1]["tries"] == tries and chk["slots"][1]["successful"] == 0):
        back_to_a("read-back check failed")
    print(f"armed: slot B active with {tries} attempt(s); slot A takes over afterwards.")
    if reboot:
        be.run("reboot")
    else:
        print("run `reboot` (or re-run with --reboot) to start the attempt")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["probe", "arm", "status"])
    ap.add_argument("image", nargs="?")
    ap.add_argument("--local", action="store_true", help="run on the phone itself (root) instead of through adb")
    ap.add_argument("--tries", type=int, default=1, choices=range(1, 8))
    ap.add_argument("--reboot", action="store_true")
    a = ap.parse_args()
    be = Local() if a.local else Adb()
    if a.cmd == "arm":
        if not a.image:
            ap.error("arm needs an image")
        cmd_arm(be, a.image, a.tries, a.reboot)
    elif a.cmd == "probe":
        cmd_probe(be)
    else:
        cmd_status(be)


if __name__ == "__main__":
    main()
