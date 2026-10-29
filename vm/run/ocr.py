#!/usr/bin/env python3
"""Screenshot + OCR with coordinates (for blind GUI control).

Usage: ocr.py [out.png] [--psm N]
Prints: x y w h conf text   (coordinates already scaled to screen pixels)
"""
import os
import subprocess
import sys

from PIL import Image, ImageOps

VM_DIR = "/home/fluff_/macOS-VM"
OUT = "/tmp/opencode/ocr.png"
PSM = "11"
args = [a for a in sys.argv[1:]]
if "--psm" in args:
    i = args.index("--psm")
    PSM = args[i + 1]
    del args[i:i + 2]
paths = [a for a in args if not a.startswith("-")]
if paths:
    OUT = paths[0]

os.makedirs(os.path.dirname(OUT), exist_ok=True)
subprocess.run([sys.executable, os.path.join(VM_DIR, "run", "vmctl.py"), "shot", OUT],
               check=True, stdout=subprocess.DEVNULL)

im = Image.open(OUT).convert("L")
w, h = im.size
big = im.resize((w * 2, h * 2), Image.LANCZOS)
big = ImageOps.autocontrast(big, cutoff=1)
tmp = "/tmp/opencode/ocr_proc.png"
big.save(tmp)

tsv = subprocess.run(["tesseract", tmp, "-", "--psm", PSM, "tsv"],
                     capture_output=True, text=True).stdout
print(f"# screen {w}x{h}")
words = []
for line in tsv.splitlines()[1:]:
    f = line.split("\t")
    if len(f) < 12:
        continue
    text = f[11].strip()
    if not text:
        continue
    try:
        conf = float(f[10])
    except ValueError:
        continue
    if conf < 35:
        continue
    x, y, bw, bh = (int(f[6]) // 2, int(f[7]) // 2, int(f[8]) // 2, int(f[9]) // 2)
    words.append((y, x, bw, bh, conf, text))
for y, x, bw, bh, conf, text in sorted(words):
    print(f"{x:4d} {y:4d} {bw:3d} {bh:3d} {conf:3.0f}  {text}")
