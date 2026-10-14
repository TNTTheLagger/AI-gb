#!/usr/bin/env python3
"""Writes a small synthetic Game Boy ROM used for trying the emulator out.

It fills tile 0 with black, tile 1 with white, builds a checkerboard map out
of them, and scrolls the whole thing one row per frame from the VBlank
handler so the picture visibly moves.
"""
import sys

SIZE = 0x8000
ROM = bytearray(SIZE)

TILE_DATA = 0x00     # $8000, where tile 0 begins
TILE_MAP = 0x00      # $9800, the background map
VECTOR_VBLANK = 0x40  # $0040


def emit(address, *values):
    for i, v in enumerate(values):
        ROM[address + i] = v


# ---------------------------------------------------------------- entry point
# $0100-$0103 is all the room there is before the cartridge header, so the
# entry point only jumps past the header to the program proper at $0150.
emit(0x0100,
     0x00,                   # NOP
     0xC3, 0x50, 0x01)       # JP $0150

emit(0x0150,
     0x31, 0xFE, 0xFF,       # LD SP,$FFFE

     # --- switch the LCD off, which is only allowed during VBlank ------
     0xF0, 0x44,             # LDH A,($44)   -- LY
     0xFE, 0x90,             # CP $90
     0x38, 0xFA,             # JR C,-6       (until line 144)
     0xAF,                   # XOR A
     0xE0, 0x40,             # LDH ($40),A   -- LCDC: off, VRAM is free

     # --- build a black and a white tile -----------------------------
     0x21, TILE_DATA & 0xFF, 0x80,   # LD HL,$8000
     0x06, 0x10,             # LD B,$10      (16 bytes)
     0x3E, 0x00,             # LD A,$00      (tile 0 = colour 0)
     0x22,                   # LD (HL+),A
     0x05,                   # DEC B
     0x20, 0xFC,             # JR NZ,-4

     0x3E, 0xFF,             # LD A,$FF      (tile 1 = colour 3)
     0x06, 0x10,             # LD B,$10
     0x22,                   # LD (HL+),A
     0x05,                   # DEC B
     0x20, 0xFC,             # JR NZ,-4

     # --- fill the map with a checkerboard ---------------------------
     # Entry n of the 32x32 map gets (column + row) & 1, which is bit 0
     # of L XORed with bit 5 of L (the low bit of the row).
     0x21, TILE_MAP & 0xFF, 0x98,       # LD HL,$9800
     0x7D,                   # LD A,L        <- the loop body
     0x0F, 0x0F, 0x0F, 0x0F, 0x0F,      # RRCA x5     (bit 5 to bit 0)
     0xAD,                   # XOR L
     0xE6, 0x01,             # AND $01
     0x22,                   # LD (HL+),A
     0x7C,                   # LD A,H
     0xFE, 0x9C,             # CP $9C        (past the end of the map?)
     0x20, 0xF1,             # JR NZ,-15     (back to LD A,L)

     # --- switch the LCD on and let VBlank drive the scroll -----------
     0x3E, 0xE4,             # LD A,$E4      (identity palette)
     0xE0, 0x47,             # LDH ($47),A   -- BGP
     0x3E, 0x91,             # LD A,$91
     0xE0, 0x40,             # LDH ($40),A   -- LCDC: on, BG on
     0xAF,                   # XOR A
     0xE0, 0x0F,             # LDH ($0F),A   -- clear IF
     0x3E, 0x01,             # LD A,$01
     0xE0, 0xFF,             # LDH ($FF),A   -- IE = VBlank
     0xFB,                   # EI
     0x76,                   # HALT          <- wait for the next frame
     0x18, 0xFD)             # JR -3         (back to HALT)

# ------------------------------------------------------- VBlank interrupt
emit(VECTOR_VBLANK,
     0xF0, 0x42,             # LDH A,($42)   -- current SCY
     0x3C,                   # INC A
     0xE0, 0x42,             # LDH ($42),A   -- scroll one row per frame
     0x3C,
     0xE0, 0x43,             # LDH ($43),A   -- and one column
     0xD9)                   # RETI

# ------------------------------------------------------------------- header
ROM[0x134:0x13C] = b"DEMO ROM"
ROM[0x143] = 0x00       # not a colour cartridge
ROM[0x147] = 0x00       # ROM only
ROM[0x148] = 0x00       # 32 KiB
ROM[0x149] = 0x00       # no cartridge RAM

# Header checksum over $0134-$014C, which the boot ROM verifies.
check = 0
for byte in ROM[0x134:0x14D]:
    check = (check - byte - 1) & 0xFF
ROM[0x14D] = check

path = sys.argv[1] if len(sys.argv) > 1 else "demo.gb"
with open(path, "wb") as f:
    f.write(ROM)
print("wrote %s (%d bytes)" % (path, len(ROM)))
