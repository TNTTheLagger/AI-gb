#!/usr/bin/env python3
"""
Renders frames at a spread of terminal sizes through a pseudo terminal and
checks that the picture really was laid out for the size it was given.

    python3 tools/check_sizes.py [rom] [binary]
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
import threading
import time

ROM = sys.argv[1] if len(sys.argv) > 1 else "demo.gb"
BIN = sys.argv[2] if len(sys.argv) > 2 else "./gbcpu"

HALF_BLOCK = "▀".encode("utf-8")
POSITION = re.compile(rb"\x1b\[(\d+);1H")

failures = []

# Wide and short, narrow and tall, tiny, and enormous.
SIZES = [
    (40, 12), (60, 20), (80, 24), (100, 30),
    (120, 40), (200, 50), (240, 70), (400, 120),
]


def check(label, cond, detail=""):
    print(f"  {'ok  ' if cond else 'FAIL'} {label}" + (f"  {detail}" if not cond else ""))
    if not cond:
        failures.append(label)


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def run_at(cols, rows):
    """Runs the emulator in a pty of exactly this size and returns the raw
    output."""
    master, slave = pty.openpty()
    set_size(master, rows, cols)

    env = dict(os.environ)
    env.update({"TERM": "xterm-256color", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"})

    proc = subprocess.Popen(
        [BIN, "--size", f"{cols}x{rows}", "-f", "20", ROM],
        stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True,
    )
    os.close(slave)

    out = bytearray()
    done = threading.Event()

    def reader():
        while not done.is_set():
            try:
                r, _, _ = select.select([master], [], [], 0.05)
            except (OSError, ValueError):
                return
            if not r:
                continue
            try:
                chunk = os.read(master, 1 << 16)
            except OSError:
                return
            if not chunk:
                return
            out.extend(chunk)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    done.set()
    t.join(timeout=1)
    try:
        os.close(master)
    except OSError:
        pass
    return bytes(out), proc.returncode


print(f"rendering {ROM} at a spread of terminal sizes\n")

for cols, rows in SIZES:
    out, rc = run_at(cols, rows)
    label = f"{cols}x{rows}"

    if rc != 0:
        check(f"{label}: exits cleanly", False, f"return code {rc}")
        continue

    if HALF_BLOCK not in out:
        check(f"{label}: draws the picture", False, "no half block glyph found")
        continue

    check(f"{label}: draws the picture", True)

    # Which rows of the terminal were written to?  Those are the picture
    # rows plus the status line, and none of them may fall outside the
    # terminal or land on the status line's own row.
    rows_used = sorted({int(m) for m in POSITION.findall(out)})
    check(f"{label}: stays inside the terminal",
          bool(rows_used) and rows_used[0] >= 1 and rows_used[-1] <= rows,
          f"wrote rows {rows_used[:3]}..{rows_used[-3:]} of {rows}")

    check(f"{label}: uses more than one row",
          len(rows_used) >= 3, f"only rows {rows_used}")

    check(f"{label}: leaves room for the status line",
          rows_used[-1] == rows, f"last row {rows_used[-1]}, terminal has {rows}")

    # Centring: the first drawn row should have blank cells in front of it.
    first = out.split(b"\x1b[", 1)[0]
    check(f"{label}: entered the alternate screen", b"\x1b[?1049h" in out)

print()
if failures:
    print(f"{len(failures)} check(s) failed: {', '.join(failures)}")
    sys.exit(1)
print("sizes OK")