#!/usr/bin/env python3
"""
devshot.py - grab screenshots from the dashboard over the USB debug console.

    python tools/devshot.py COM9 out.png
    python tools/devshot.py COM9 race.png --cmd "screen race" --cmd "speed 57"
    python tools/devshot.py COM9 --all shots/            # standard screenshot set
    python tools/devshot.py COM9 --cmd fps --cmd bench   # just run commands

Every --cmd is sent in order and must answer "OK <cmd>" (an "ERR" aborts).
Then, if an output file is given, `shot` is sent and the frame is saved as
PNG. --all captures every screen / skin / speed / settings tab in dark and
light theme into a folder and restores the previous UI state afterwards.

Opening the port does not reset the board (DTR/RTS are kept low).
Requires: pyserial, Pillow.
"""

import argparse
import os
import sys
import time

import serial
from PIL import Image

W, H = 480, 320


class Dev:
    def __init__(self, port, verbose=False):
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.2
        s.dtr = False  # never toggle the ESP32-S3 USB-JTAG reset lines
        s.rts = False
        s.open()
        self.s = s
        self.verbose = verbose
        self.buf = b""
        time.sleep(0.1)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def _fill(self, deadline):
        chunk = self.s.read(self.s.in_waiting or 1)
        if chunk:
            self.buf += chunk
        elif time.time() > deadline:
            raise TimeoutError("device did not answer")

    def readline(self, deadline):
        while b"\n" not in self.buf:
            self._fill(deadline)
        line, self.buf = self.buf.split(b"\n", 1)
        return line.rstrip(b"\r").decode("utf-8", "replace")

    def readexact(self, n, deadline):
        while len(self.buf) < n:
            self._fill(deadline)
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def cmd(self, c, timeout=5.0):
        """Send a command, return the info lines printed before OK."""
        self.s.write((c.strip() + "\n").encode())
        name = c.split()[0].lower()
        deadline = time.time() + timeout
        info = []
        while True:
            line = self.readline(deadline)
            if line.startswith("OK " + name) or line == "OK " + name:
                return info
            if line.startswith("ERR"):
                raise RuntimeError(f"{c!r}: {line}")
            info.append(line)
            if self.verbose:
                print("  <", line)

    def shot(self, path, timeout=15.0):
        self.s.write(b"shot\n")
        deadline = time.time() + timeout
        while True:
            line = self.readline(deadline)
            if line.startswith("SHOT "):
                break
            if line.startswith("ERR"):
                raise RuntimeError("shot: " + line)
        _, w, h = line.split()
        w, h = int(w), int(h)
        raw = self.readexact(w * h * 2, deadline)
        tail = self.readexact(5, deadline)
        if tail != b"\nEND\n":
            raise RuntimeError(f"shot: bad trailer {tail!r} (serial data interleaved?)")
        self.readline(deadline)  # OK shot
        img = Image.new("RGB", (w, h))
        px = []
        for i in range(0, len(raw), 2):
            v = raw[i] | (raw[i + 1] << 8)
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            px.append(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        img.putdata(px)
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        img.save(path)
        return img


def parse_state(lines):
    for ln in lines:
        if ln.startswith("STATE "):
            return dict(kv.split("=", 1) for kv in ln[6:].split())
    return {}


def shot_retry(dev, path, tries=3):
    for i in range(tries):
        try:
            dev.shot(path)
            print("saved", path)
            return
        except (RuntimeError, TimeoutError) as e:
            print("retry:", e)
            time.sleep(0.3)
            dev.s.reset_input_buffer()
            dev.buf = b""
    raise RuntimeError("shot failed")


def capture_all(dev, folder, settle):
    st = parse_state(dev.cmd("state"))
    print("current state:", st)
    try:
        for theme in ("dark", "light"):
            dev.cmd(f"theme {theme}")
            for skin in range(4):
                dev.cmd(f"skin {skin}")
                dev.cmd("screen dash")
                for v in (0, 35, 70):
                    dev.cmd(f"speed {v}")
                    dev.cmd("accel {:.2f}".format(v / 200.0))
                    time.sleep(settle)
                    shot_retry(dev, os.path.join(folder, f"dash_skin{skin}_{v:02d}kmh_{theme}.png"))
            dev.cmd("screen race")
            for v in (0, 35, 70):
                dev.cmd(f"speed {v}")
                time.sleep(settle)
                shot_retry(dev, os.path.join(folder, f"race_{v:02d}kmh_{theme}.png"))
            for tab in range(6):
                dev.cmd(f"tab {tab}")
                time.sleep(settle)
                shot_retry(dev, os.path.join(folder, f"settings_tab{tab}_{theme}.png"))
    finally:
        dev.cmd("speed off")
        dev.cmd("accel off")
        for key, c in (("theme", "theme"), ("skin", "skin"), ("tab", "tab"), ("screen", "screen")):
            if key in st:
                dev.cmd(f"{c} {st[key]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("out", nargs="?", help="PNG to write (omit to only run --cmd)")
    ap.add_argument("--cmd", action="append", default=[], help="console command to send first (repeatable)")
    ap.add_argument("--all", metavar="FOLDER", help="capture the standard screenshot set into FOLDER")
    ap.add_argument("--settle", type=float, default=0.25, help="seconds to wait before a shot (default 0.25)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    dev = Dev(a.port, a.verbose)
    try:
        dev.cmd("ping")
        for c in a.cmd:
            for ln in dev.cmd(c, timeout=30.0):
                print(ln)
        if a.all:
            capture_all(dev, a.all, a.settle)
        if a.out:
            time.sleep(a.settle)
            shot_retry(dev, a.out)
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
