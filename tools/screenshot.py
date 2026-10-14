#!/usr/bin/env python3
"""Grab the panel's current frame over serial and save it as PNG.

    python tools/screenshot.py COM4 shot.png
    python tools/screenshot.py COM4 fb.png "kbd;shot fb 60"   # real framebuffer, 60 ms after "kbd"

Sends "shot"; the firmware replies "SHOT <w> <h>", then lines "#<base64>"
holding RLE records (u16 count, u16 RGB565 byte-swapped, little-endian), then
"END".  Other tasks keep logging meanwhile, so every line that does not start
with '#' is ignored.  Needs pyserial; Pillow for PNG (falls back to .ppm).
Close any serial monitor first.
"""
import base64
import struct
import sys
import time

import serial


def capture(port, command="shot"):
    """One attempt: returns (w, h, rgb bytes) or raises RuntimeError."""
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 2
    s.dtr = s.rts = False  # opening must not reset the board
    s.open()
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(command.replace(";", "\n").encode() + b"\n")  # several commands in one write

    deadline = time.time() + 10
    while True:
        line = s.readline()
        if line.startswith(b"SHOT "):
            break
        if time.time() > deadline:
            raise RuntimeError("no SHOT header - is the HMI firmware running?")
    w, h = map(int, line.split()[1:3])

    data = bytearray()
    deadline = time.time() + 60
    while True:
        line = s.readline().strip()
        if line == b"END":
            break
        if line.startswith(b"#"):
            data += base64.b64decode(line[1:])
        if time.time() > deadline:
            raise RuntimeError("timeout before END")
    s.close()

    pixels = bytearray()
    for i in range(0, len(data) - 3, 4):
        run, c = struct.unpack_from("<HH", data, i)
        c = ((c & 0xFF) << 8) | (c >> 8)  # undo LovyanGFX/LVGL byte swap
        r, g, b = (c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3
        pixels += bytes((r, g, b)) * run
    if len(pixels) != w * h * 3:
        # A log line from another task split one of our lines: try again.
        raise RuntimeError(f"incomplete frame: {len(pixels) // 3}/{w * h} pixels")
    return w, h, pixels


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    out = sys.argv[2] if len(sys.argv) > 2 else "screenshot.png"
    command = sys.argv[3] if len(sys.argv) > 3 else "shot"
    for attempt in range(1, 4):
        try:
            w, h, pixels = capture(port, command)
            break
        except RuntimeError as e:
            print(f"attempt {attempt}: {e}", file=sys.stderr)
    else:
        sys.exit(1)

    try:
        from PIL import Image
        Image.frombytes("RGB", (w, h), bytes(pixels)).save(out)
    except ImportError:
        out = out.rsplit(".", 1)[0] + ".ppm"
        with open(out, "wb") as f:
            f.write(b"P6 %d %d 255\n" % (w, h) + pixels)
    print(f"saved {out} ({w}x{h})")


if __name__ == "__main__":
    main()
