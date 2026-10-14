#!/usr/bin/env python3
"""
Drives the emulator through a pseudo terminal, so the interactive path is
tested for real: raw mode, key decoding, rendering, and a clean exit.

Not part of the unit test suite, because it needs a pty and a real
terminal size.  Used by hand and by CI.

    python3 tools/check_terminal.py [rom] [binary]
"""
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import threading
import time

ROM = sys.argv[1] if len(sys.argv) > 1 else "demo.gb"
BIN = sys.argv[2] if len(sys.argv) > 2 else "./gbcpu"

# U+2580, the glyph one character cell uses to show two stacked pixels.
HALF_BLOCK = "▀".encode("utf-8")

failures = []


def check(label, cond, detail=""):
    print(f"  {'ok  ' if cond else 'FAIL'} {label}" + (f"  {detail}" if not cond else ""))
    if not cond:
        failures.append(label)


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


# A real terminal always consumes what the program writes, without being
# asked.  The emulator blocks once the pipe is full, so this reader has to
# keep up or the program will look frozen for reasons that are not its own.
captured = bytearray()
running = True


def reader(fd):
    while running:
        try:
            r, _, _ = select.select([fd], [], [], 0.05)
        except (OSError, ValueError):
            return
        if not r:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return
        if not chunk:
            return
        captured.extend(chunk)


def wait_for(needle, timeout=5.0):
    """Waits until `needle` has appeared in everything read so far."""
    target = bytes(needle)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if target in captured:
            return True
        time.sleep(0.02)
    return target in captured


def mark():
    return len(captured)


def appeared_at(where, needle, timeout=5.0):
    """Waits for `needle` to show up *after* the given mark."""
    target = bytes(needle)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if target in captured[where:]:
            return True
        time.sleep(0.02)
    return target in captured[where:]


master, slave = pty.openpty()
set_size(master, 40, 100)

env = dict(os.environ)
env.update({"TERM": "xterm-256color", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"})

proc = subprocess.Popen(
    [BIN, ROM],
    stdin=slave, stdout=slave, stderr=slave,
    env=env, close_fds=True,
)
os.close(slave)

pump = threading.Thread(target=reader, args=(master,), daemon=True)
pump.start()

try:
    wait_for(HALF_BLOCK)

    # --- rendering ---
    check("entered the alternate screen", b"\x1b[?1049h" in captured)
    check("hid the cursor", b"\x1b[?25l" in captured)
    check("drew the half block glyph", HALF_BLOCK in captured)
    check("used true colour", b"\x1b[38;2;" in captured)
    check("showed the status line", appeared_at(0, b"fps"))
    check("drew from the top left of the window", b"\x1b[1;1H" in captured)

    # --- command keys ---
    start = mark()
    os.write(master, b"?")
    check("question mark shows the controls",
          wait_for(b"Game Boy emulator - controls"))

    # The screen has to stay up until a key arrives, not vanish at once.
    time.sleep(0.5)
    check("the controls stay on screen",
          b"Press any key to continue" in captured[start:])

    # Dismissing it is what gets us back to the picture; wait for that
    # before sending anything else, or the next key is swallowed by the
    # same read that closed the help screen.
    resume = mark()
    os.write(master, b" ")
    check("any key closes the controls", appeared_at(resume, b"fps"))

    start = mark()
    os.write(master, b"p")
    check("p pauses", appeared_at(start, b"[paused]"))

    start = mark()
    os.write(master, b"p")
    check("p resumes", appeared_at(start, b"running"))

    start = mark()
    os.write(master, b"m")
    check("m mutes", appeared_at(start, b"[muted]"))

    start = mark()
    os.write(master, b"r")
    check("r resets", appeared_at(start, b"reset"))

    # --- arrow keys must not be mistaken for characters ---
    for seq in (b"\x1b[A", b"\x1b[B", b"\x1b[C", b"\x1b[D"):
        os.write(master, seq)
    time.sleep(0.4)
    check("arrows do not quit the emulator", proc.poll() is None)

    # --- quit ---
    start = mark()
    os.write(master, b"q")
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    time.sleep(0.3)
    tail = bytes(captured[start:])

    check("left the alternate screen", b"\x1b[?1049l" in tail)
    check("restored the cursor", b"\x1b[?25h" in tail)
    check("reset the colours", b"\x1b[0m" in tail)
    check("exited cleanly", proc.returncode == 0, f"return code {proc.returncode}")
finally:
    running = False
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    pump.join(timeout=1.0)
    try:
        os.close(master)
    except OSError:
        pass

print()
if failures:
    print(f"{len(failures)} check(s) failed: {', '.join(failures)}")
    sys.exit(1)
print("terminal path OK")