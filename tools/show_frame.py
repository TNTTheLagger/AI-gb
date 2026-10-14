#!/usr/bin/env python3
"""
Runs the emulator in a pty and shows what the terminal would actually
display, by replaying the escape sequences into a character grid including
the 24 bit colours, so the picture can be judged rather than just counted.

    python3 tools/show_frame.py COLSxROWS [rom] [binary] [frames]
"""
import fcntl
import os
import pty
import re
import select
import struct
import subprocess
import sys
import termios
import time

cols, rows = (int(v) for v in sys.argv[1].split("x"))
rom = sys.argv[2] if len(sys.argv) > 2 else "demo.gb"
binary = sys.argv[3] if len(sys.argv) > 3 else "./gbcpu"
frames = sys.argv[4] if len(sys.argv) > 4 else "20"

master, slave = pty.openpty()
fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

env = dict(os.environ)
env.update({"TERM": "xterm-256color", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"})

proc = subprocess.Popen(
    [binary, "--size", f"{cols}x{rows}", "-f", frames, rom],
    stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True,
)
os.close(slave)

raw = bytearray()
deadline = time.time() + 60
while time.time() < deadline and proc.poll() is None:
    r, _, _ = select.select([master], [], [], 0.05)
    if not r:
        continue
    try:
        chunk = os.read(master, 1 << 16)
    except OSError:
        break
    if not chunk:
        break
    raw.extend(chunk)
proc.wait()
os.close(master)

# --- replay the screen, colours and all --------------------------------
RAMP = "@%#*+=-:. "  # darkest to lightest

def shade(rgb):
    lum = (rgb[0] * 30 + rgb[1] * 59 + rgb[2] * 11) // 100
    return RAMP[min(len(RAMP) - 1, lum * len(RAMP) // 256)]

grid = [[(" ", (255, 255, 255), (0, 0, 0)) for _ in range(cols)]
        for _ in range(rows)]

cy = cx = 0
fg, bg = (255, 255, 255), (0, 0, 0)
text = raw.decode("utf-8", "replace")
i = 0

TOKEN = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")

while i < len(text):
    ch = text[i]

    if ch == "\x1b":
        m = TOKEN.match(text, i)
        if not m:
            i += 1
            continue
        params, final = m.group(1), m.group(2)

        if final == "H":
            parts = params.split(";") if params else []
            cy = int(parts[0]) - 1 if len(parts) > 0 and parts[0] else 0
            cx = int(parts[1]) - 1 if len(parts) > 1 and parts[1] else 0
        elif final == "K":
            for x in range(max(cx, 0), cols):
                if 0 <= cy < rows:
                    grid[cy][x] = (" ", fg, bg)
        elif final == "m":
            nums = [int(p) if p else 0 for p in params.split(";")] if params else [0]
            k = 0
            while k < len(nums):
                n = nums[k]
                if n == 0:
                    fg, bg = (255, 255, 255), (0, 0, 0)
                elif n == 38 and k + 4 < len(nums) and nums[k + 1] == 2:
                    fg = (nums[k + 2], nums[k + 3], nums[k + 4])
                    k += 4
                elif n == 48 and k + 4 < len(nums) and nums[k + 1] == 2:
                    bg = (nums[k + 2], nums[k + 3], nums[k + 4])
                    k += 4
                k += 1
        i = m.end()
        continue

    if 0 <= cy < rows and 0 <= cx < cols:
        # The half block paints its top half in the foreground and its
        # bottom half in the background, so both matter.
        if ch == "▀":
            grid[cy][cx] = ("▀", fg, bg)
        else:
            grid[cy][cx] = (ch, fg, bg)
    cx += 1
    if cx >= cols:
        cx = 0
        cy += 1
    i += 1

print(f"--- {cols}x{rows} : {rom} ---")
for line in grid:
    out = []
    for ch, f, b in line:
        if ch == "▀":
            out.append(shade(f) + shade(b))   # two pixel rows
        else:
            out.append(ch)
    print("|" + "".join(out) + "|")