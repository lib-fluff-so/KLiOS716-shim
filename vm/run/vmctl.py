#!/usr/bin/env python3
"""Tiny QMP control tool for the headless macOS VM.

Usage:
  vmctl.py shot [out.png]          # screenshot (default screens/latest.png)
  vmctl.py key meta_l+spc          # press key combo
  vmctl.py type "hello world"      # type ASCII text
  vmctl.py click X Y [button]      # absolute click (usb-tablet)
  vmctl.py hmp "info status"       # raw HMP command
  vmctl.py status                  # vm status
"""
import json
import os
import socket
import sys
import time

VM_DIR = "/home/fluff_/macOS-VM"
SOCK = os.path.join(VM_DIR, "run", "qmp.sock")
SHIFTED = {
    "_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right",
    "|": "backslash", ":": "semicolon", '"': "apostrophe", "~": "grave_accent",
    "<": "comma", ">": "dot", "?": "slash", "!": "1", "@": "2", "#": "3",
    "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
}
PLAIN = {
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    "\\": "backslash", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
    ",": "comma", ".": "dot", "/": "slash", " ": "spc", "\n": "ret",
    "\t": "tab",
}


class QMP:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                self.s.connect(SOCK)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        else:
            raise SystemExit("QMP socket not available (VM running?)")
        self.f = self.s.makefile("rwb")
        self._id = 0
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("QMP closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def cmd(self, name, **args):
        self._id += 1
        req = {"execute": name, "arguments": args, "id": self._id}
        self.f.write((json.dumps(req) + "\n").encode())
        self.f.flush()
        r = self._read()
        if "error" in r:
            raise SystemExit(f"QMP error: {r['error']}")
        return r.get("return")


def shot(q, out):
    out = os.path.abspath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = os.path.join("/tmp/opencode", f"shot-{os.getpid()}.ppm")
    q.cmd("screendump", filename=tmp)
    time.sleep(0.3)
    png = os.path.splitext(out)[0] + ".png"
    try:
        from PIL import Image
        with Image.open(tmp) as im:
            im.save(png)
        os.unlink(tmp)
        return png
    except Exception as e:  # noqa
        os.replace(tmp, os.path.splitext(out)[0] + ".ppm")
        return f"{out} (ppm, convert failed: {e})"


def geometry(q):
    tmp = "/tmp/opencode/geom.ppm"
    q.cmd("screendump", filename=tmp)
    time.sleep(0.2)
    from PIL import Image
    with Image.open(tmp) as im:
        w, h = im.size
    os.unlink(tmp)
    return w, h


def send(q, keys, hold=60):
    q.cmd(
        "send-key",
        keys=[{"type": "qcode", "data": k} for k in keys],
        **{"hold-time": hold},
    )


def key_combo(spec):
    keys = [k.strip() for k in spec.split("+") if k.strip()]
    aliases = {"cmd": "meta_l", "command": "meta_l", "enter": "ret",
               "space": "spc", "escape": "esc", "opt": "alt_l",
               "option": "alt_l", "shift": "shift", "ctrl": "ctrl"}
    keys = [aliases.get(k, k) for k in keys]
    return keys


def type_text(q, text):
    for ch in text:
        if ch.isalpha() or ch.isdigit():
            if ch.isupper():
                send(q, ["shift", ch.lower()])
            else:
                send(q, [ch])
        elif ch in PLAIN:
            send(q, [PLAIN[ch]])
        elif ch in SHIFTED:
            send(q, ["shift", SHIFTED[ch]])
        else:
            continue
        time.sleep(0.04)


def click(q, x, y, button="left"):
    w, h = geometry(q)
    ax = int(x * 32767 / max(w - 1, 1))
    ay = int(y * 32767 / max(h - 1, 1))
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ])
    time.sleep(0.1)
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": True, "button": button}},
    ])
    time.sleep(0.08)
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": False, "button": button}},
    ])


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    q = QMP()
    cmd = sys.argv[1]
    if cmd == "shot":
        out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(VM_DIR, "screens", "latest.png")
        print(shot(q, out))
    elif cmd == "key":
        send(q, key_combo(sys.argv[2]))
    elif cmd == "type":
        type_text(q, sys.argv[2])
    elif cmd == "click":
        click(q, int(sys.argv[2]), int(sys.argv[3]),
              sys.argv[4] if len(sys.argv) > 4 else "left")
    elif cmd == "hmp":
        print(q.cmd("human-monitor-command", **{"command-line": sys.argv[2]}))
    elif cmd == "status":
        print(q.cmd("query-status"))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
