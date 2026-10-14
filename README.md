# gbcpu — a Game Boy emulator in C

A Game Boy (DMG) emulator written entirely in C. The video and input
backend draws straight into your terminal, so there is no windowing library,
no SDL, and no external dependencies beyond a C compiler.

Builds and runs on Linux, macOS and Windows.

```
$ make
$ ./gbcpu game.gb
```

See [docs/screenshot.txt](docs/screenshot.txt) for what it draws.

## What it does

* **CPU** — the full Sharp LR35902 instruction set, including the `$CB`
  prefix, the `HALT` bug, the `STOP` state, `EI`'s one-instruction delay and
  the five interrupt vectors with their real priority order.
* **PPU** — dot-accurate mode timing (2 / 3 / 0), background with both tile
  maps and both addressing modes, the window, 8x8 and 8x16 sprites with
  X/Y flip, priority, the OBP0/OBP1 palettes, OAM DMA, `LY`/`LYC`, and the
  `STAT` interrupt line with edge detection.
* **Timer** — a free-running 16-bit divider with `TAC` bit selection, so
  `TIMA` increments on the same falling edges the real hardware uses.
* **APU** — both square channels (one with a working frequency sweep), the
  wave channel, the noise channel, envelopes, length counters, the 8192 Hz
  frame sequencer, `NR50`/`NR51` panning and the DC-blocking output filter.
* **Cartridges** — plain ROM, MBC1, MBC2, MBC3 (including the real-time
  clock) and MBC5 (nine-bit banking), with cartridge RAM and the
  save/load of snapshots.
* **Input** — the full 4x2 joypad matrix, including the interrupt on a
  button press.

## Building

The same source list builds everywhere. There is one caveat per platform,
and nothing to install.

### Linux and macOS

```sh
make                 # builds ./gbcpu and ./gbtests
make test            # runs the unit tests
make run ROMS=x.gb   # builds and runs
```

```sh
cmake -S . -B build && cmake --build build
./build/gbtests      # or: ctest --test-dir build
```

`gcc` and `clang` both work. The only library linked is `libm`, which the
APU's output filter needs; macOS has that folded into the C runtime, so CMake
leaves it out there.

### Windows

Any of these, all from the same checkout:

```bat
build.bat                        :: MSVC, from a Developer Command Prompt
mingw32-make                     :: MinGW-w64
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc
cmake -S . -B build              :: MSVC via the Visual Studio generator
cmake --build build --config Release
ctest --test-dir build -C Release
```

`build.bat` builds and then runs the unit tests for you. CMake picks up
`libm` and the maths library automatically, so the same `CMakeLists.txt`
works with MSVC, MinGW and gcc.

The terminal backend uses the Win32 console API directly: raw key input
through `ReadConsoleInput`, window size through `GetConsoleScreenBufferInfo`,
and ANSI escape sequences through `ENABLE_VIRTUAL_TERMINAL_PROCESSING`.
On Windows 10 1511 and later those escapes are interpreted by the console
itself, so the picture is drawn exactly as it is anywhere else.

Audio works the same way it does elsewhere — the samples are piped to an
external player. `--sound auto` looks for `ffplay` and falls back to
PowerShell's `SoundPlayer`.

Both binaries land next to each other as `gbcpu.exe` and `gbtests.exe`.

## Using it

```
Usage: gbcpu [options] <rom>

  -f, --frames N      run N frames and exit (0 = run until quit)
  -s, --screenshot P  write the final frame to a 24 bit BMP file
      --sound MODE    none | auto | "player command"
      --ascii         use plain ASCII glyphs instead of block characters
      --no-color      disable 24 bit colour output
      --mono          force the monochrome density glyphs
  -q, --quiet         do not print cartridge information

Display:
      --size WxH      pretend the terminal is W columns by H rows
      --aspect N      character cell height as a multiple of its width
      --no-resize     never try to grow a window too small for the picture

  -h, --help          show this help
  -V, --version       show the version
```

| Key | Action |
| --- | --- |
| Arrow keys, `WASD` or `HJKL` | d-pad |
| `Z` | A button |
| `X` | B button |
| `Enter` | Start |
| `Space` | Select |
| `P` | pause / resume |
| `R` | reset the machine |
| `M` | mute / unmute |
| `?` | controls |
| `Q`, `Escape` or `Ctrl-C` | quit |

A terminal reports key *presses* but never releases, so a key is held for a
quarter second after you press it and then lets go. Tapping again holds it
longer, which is enough to walk around with.

### Fitting the window

The picture is 160 by 144 pixels and the terminal is whatever size it
happens to be, so the size is measured every frame and the picture is
scaled to fit. Resizing the window takes effect immediately, with no
restart, because the real size comes from `TIOCGWINSZ` on a Unix host and
`GetConsoleScreenBufferInfo` on Windows — the window rectangle, not the
scrollback buffer, which is usually much larger than what you can see.

Each character cell carries two vertically stacked pixels using the half
block glyph `▀`, so `COLUMNS` by `ROWS` cells offer `COLUMNS` by
`ROWS x 2` pixels. One scale is used for both axes so the picture keeps its
shape, and whichever axis runs out of room first decides how big it gets.
Whatever is left over is left blank, with the picture centred in both
directions.

Scaling works in the direction that suits it. When the picture is being
shrunk, a cell covers a block of source pixels and the block is averaged,
so thin lines survive. When it is being enlarged, a cell shows one source
pixel and repeats it; averaging there would flatten every group of enlarged
pixels into a single colour.

A character cell is usually about twice as tall as it is wide, which is
what makes the picture come out square. If yours are squarer the picture
will look stretched, so `--aspect` takes the real ratio and the layout is
planned around it:

```sh
./gbcpu --aspect 1.8 game.gb     # nearly square cells
./gbcpu --aspect 2.5 game.gb     # unusually tall cells
```

`--size WxH` overrides the measured size, which is what makes the
renderer testable from a script and lets you force a fixed layout:

```sh
./gbcpu --size 100x40 game.gb
```

Colour terminals get 24-bit foreground and background colours; everything
else falls back to block characters, or to a plain ASCII density ramp
(` `, `.`, `o`, `#`) with `--ascii`.

The colour escape is about forty bytes, and sending one per cell costs
several megabytes a second on a large window — enough that a slow link
blocks on the write and stops the emulator reading the keyboard. It is
therefore only sent when the colour actually changes, which matters
because a Game Boy screen is mostly long runs of one shade. Measured on
the demo ROM at 160 by 73, a frame drops from 160 KB to 20 KB; at 80 by 24
it goes from 17 KB to 5.5 KB. Redraws are then capped at thirty a second,
which halves what is left.

### When the window is too small

One cell carries one pixel across and two down, so showing the screen at
its own size takes **160 columns by 72 rows**, plus one for the status
line. Below that the picture is scaled down to fit, and once it is under
about half size — roughly 80 by 36 cells — several screen pixels share
every cell and the result stops being recognisable. When that happens the
status line says so and gives the size that would fix it:

```
 TETRIS [MBC1] 59.9 fps [terminal too small: 160x73]
```

### Resizing the console window

If the picture is being squeezed, the emulator asks for a bigger window
before the first frame and puts the old size back on the way out. It only
ever grows, and never asks for less than it already has, so a window that
is already large enough is left alone — taking space away from someone who
arranged their screen would be rude. `--no-resize` turns it off.

On Windows the console window belongs to the program, so it is resized
directly with `SetConsoleScreenBufferSize` and `SetConsoleWindowInfo`.

On a Unix host the window belongs to the terminal emulator, so this is a
request rather than a command: `CSI 8 ; rows ; cols t`, understood by
xterm and everything that copied its behaviour, including iTerm2, WezTerm,
kitty and GNOME Terminal. A terminal that does not implement it ignores
the sequence, and the picture is scaled to whatever room there is, with
the status line saying what would help. `platform_can_resize_console()`
reports whether asking is worth trying.

### Audio

A terminal cannot make sound, so the APU output is optionally piped to an
external player as raw signed 16-bit stereo PCM at 44.1 kHz:

```sh
./gbcpu --sound auto game.gb       # finds ffplay, paplay, aplay or afplay
./gbcpu --sound "ffplay -nodisp -f s16le -i -" game.gb
```

On Windows, `--sound auto` looks for `ffplay` and otherwise falls back to
PowerShell, which can play a PCM stream through `SoundPlayer`.

Without `--sound` the samples are still generated and mixed exactly the same
way; they are simply discarded.

## Tests

There are 315 unit tests, and the build wires them into `ctest`. They run
unchanged on every platform, including the parts of the host layer that used
to be hidden behind `#ifdef`.

```sh
./gbtests              # everything
./gbtests cpu ppu      # only the named suites
```

| Suite | Covers |
| --- | --- |
| `cpu` | every opcode group, flags, `DAA`, rotates, `$CB` forms, interrupts, `HALT`/`STOP` |
| `timing` | the whole opcode-by-opcode timing table, taken and not taken |
| `memory` | every bus region, echo RAM, OAM DMA timing, the I/O register file |
| `cart` | header parsing, ROM/RAM size codes, and MBC1/2/3/5 banking |
| `timer` | `DIV`, all four `TAC` rates, overflow and reload |
| `ppu` | mode timing, `LY`/`LYC`/`STAT`, background, window, sprites, priority |
| `apu` | registers, the frame sequencer, all four channels, panning, envelopes |
| `input` | the joypad matrix and the serial port |
| `state` | save-state round trips and continued execution |
| `platform` | key decoding, the timed key holds, files, paths, timing, resize |
| `layout` | fitting the screen to terminals from 1x1 to 1000x300, up and down |
| `integration` | hand-written programs run end to end |

`tools/make_demo_rom.py` writes a synthetic ROM (a scrolling checkerboard)
so you can try the emulator without a commercial game:

```sh
python3 tools/make_demo_rom.py demo.gb
./gbcpu demo.gb
```

`tools/check_terminal.py` drives the emulator through a pseudo terminal and
checks the whole interactive path: raw mode, key decoding, the redraws, every
command key and a clean exit. `tools/check_sizes.py` does the same at a
spread of terminal sizes and checks the picture stays inside each one.
`tools/show_frame.py` replays what a terminal would actually display,
colours and all, which is the quickest way to look at a change:

```sh
python3 tools/check_terminal.py demo.gb ./gbcpu
python3 tools/check_sizes.py    demo.gb ./gbcpu
python3 tools/show_frame.py 100x40 demo.gb
```

These need a pty, so they are kept out of the unit suite, but they run on
both Linux and macOS.

### Checking against the hardware reference

The instruction timings come from the opcode tables in the *Game Boy
Complete Technical Reference*, which lists every instruction as a duration
in machine cycles. Rather than transcribe them once and hope, the table can
be re-derived from the document and compared with the running emulator:

```sh
make timings                      # prints what each opcode actually costs
python3 tools/check_timings.py    # compares it with the reference
```

That compares 256 documented timings, including the taken and not-taken cost
of every conditional branch. It needs `pdftotext` and a copy of the
reference; the results are also baked into the `timing` test suite, which
needs neither and runs everywhere.

It has already earned its keep: it caught the ALU operations with a memory
operand taking three machine cycles instead of two. Every `ADD A,(HL)`,
`AND (HL)`, `CP (HL)` and the rest were a whole M-cycle slow, which is
exactly the kind of drift that quietly desynchronises a game's timing.

CI builds and tests on Linux with both compilers, on macOS, and on Windows
with both MSVC and MinGW.

### Checking against Pan Docs

A second reference is used the same way, for everything that is a table of
values rather than a duration: *Pan Docs* records what every I/O register
reads at `$0100`, the layout of the sound registers, and how long the pixel
transfer takes on a given scanline. Those were worked through by hand and the
results are baked into the `memory`, `apu`, `ppu` and `timer` suites, so they
are checked on every build without needing the document.

That pass turned up several faults, each of which is now covered by a test:

* The interrupt vector table had VBlank and LCD status **transposed**. VBlank
  is flag bit 0 but lives at `$0048`, while LCD status is flag bit 1 and lives
  at `$0040`, *below* it — so every game was entering its LCD status handler
  when its VBlank fired, and vice versa. This is the fault that kept Tetris
  parked in its intro.
* The noise channel's four sound registers were decoded one address low:
  `$FF22` was treated as NR44, so writing NR43 triggered the channel and
  writing NR44 did nothing.
* Writing zero to the top five bits of `NR12`, `NR22` or `NR42` switches that
  channel's DAC off immediately. The state was only recomputed on the next
  trigger, so a game that silenced a channel kept playing it.
* `STAT` dropped bit 2, the LY/LYC comparison flag, from every read.
* Writing `DIV` or `TAC` can step `TIMA` on its own, because the hardware
  latches the selected divider bit ANDed with the enable, so clearing either
  one can drive that signal low. The code had a comment promising this and no
  code to do it.
* Mode 3 was a flat 172 dots plus six per sprite. It is 172 plus `SCX % 8`,
  plus six for the window, plus a per-object penalty that depends on where in
  the background the object starts, capped so a crowded line still fits the
  scanline. Modelling that exposed a second fault: a line whose transfer fills
  the whole scanline has a zero-length horizontal blank, and the mode advance
  was charging it a dot anyway, so such lines ran 457 dots long and the frame
  drifted.

## How it fits together

```
src/
  gb.h            the whole machine in one header: no platform code
  cpu.c           the LR35902 interpreter
  memory.c        the bus and the memory bank controllers
  io.c            the $FF00-$FF7F register file, and OAM DMA
  ppu.c           the LCD controller
  apu.c           the sound hardware and its sample mixer
  timer.c         DIV/TIMA/TMA/TAC
  joypad.c        the button matrix
  serial.c        the link port
  cart.c          ROM loading and header decoding
  gb.c            assembling the machine, running frames, save states

  platform.h      the boundary between core and host
  platform_posix.c  terminal, timing and files, via POSIX
  platform_win32.c  the same, via the Win32 console
  platform_keys.c   escape sequence decoding, shared by both
  keymap.h/.c     key codes to joypad buttons and timed holds
  layout.h/.c     how the 160x144 screen fits a grid of character cells

  frontend.h      the boundary between core and frontend
  frontend_term.c the terminal backend
  main.c          command line handling
```

The core — everything down to `cart.c` — is plain C11 with no operating
system calls at all. That is what lets the test suite drive the hardware
directly.

Everything above it is confined to a small platform layer with one
implementation per operating system. Both files build everywhere; whichever
is not the host compiles to nothing. Input crosses that boundary as
`PlatformKey` codes rather than raw bytes, so the frontend has no `#ifdef`
in it and the interesting part — decoding escape sequences and stretching
each press into a timed hold — is ordinary portable code that the unit tests
run on every platform.

### Timing

Every device is driven by a single `gb_tick()` that hands each one the number
of T-cycles an instruction consumed. The PPU advances dot by dot and draws
each scanline when mode 3 begins, so a game that writes to VRAM during mode 3
sees the change on the next line, exactly as it would on hardware. Mode 3 is
172 dots, plus a stall for each of the three things that make the fetcher
wait: `SCX % 8` for the shifted first tile, six for the window, and a
penalty per object that depends on where its leftmost pixel falls. A frame is
154 lines, or 70224 cycles, which puts it at the correct 59.73 Hz.

The timer models the divider the way the hardware does — a free-running
counter with `TAC` tapping one of its bits and `TIMA` incrementing on the
falling edge — rather than counting `TIMA` directly, which is what makes the
rate correct at all four `TAC` settings.

The APU mixes at the sample rate instead of once per call, so a long
instruction cannot run the channels ahead of the mixer and flatten the
waveform.

## Known limitations

* **No boot ROM.** The machine starts with the register values the DMG boot
  ROM would have left behind, so games that wait for the Nintendo logo
  scroll start immediately instead of after about a second. A handful of
  titles that drive their own intro state machine off the boot ROM's exact
  behaviour may not get past it; see the note on Tetris below. Two registers
  deliberately depart from the documented table: `BGP`, `OBP0` and `OBP1` are
  left as an identity mapping rather than the boot ROM's values. Hardware
  leaves them holding whatever the last power cycle left behind and the
  reference is explicit that they are uninitialised and must not be relied
  on, so an identity mapping is the most useful of the plausible choices for a
  machine that never ran the boot ROM.
* **DMG only.** Game Boy Color cartridges load and are warned about, but
  there is no CGB palette, VRAM banking, speed switching or HDMA. Super
  Game Boy border and palettes are not emulated.
* **Scanline rendering.** Lines are drawn in one pass at the start of mode 3
  rather than pixel by pixel through a FIFO, so a game that changes `SCX` or
  a palette in the middle of a line will not see the split.
* **VRAM is never locked.** On hardware the PPU owns VRAM and OAM during
  modes 2 and 3, and the CPU's writes to them are dropped and its reads
  return garbage. Here they always succeed, which is the forgiving direction
  but not the accurate one.
* **Serial is a stub.** There is no cable attached, so a transfer completes
  by shifting in an idle high line. Two emulators cannot link up.
* **No MBC rumble.** MBC5 rumble cartridges run, but the motor is silent.
* **Terminals only.** There is no SDL, no windowing and no video output other
  than the terminal and the BMP screenshot.
* **Keys are taps, not holds.** The terminal never reports a key release, so
  a button is pressed for 250 ms and released, and diagonal movement relies on
  two taps landing close together. A real console driver would report the
  release and hold for as long as the key is down.

## Where it stands against a commercial ROM

The demo ROM and the unit tests exercise the hardware, but a real cartridge
is a harsher test. Tetris (Rev. 01) loads and runs. Its VBlank handler
executes every frame, its interrupt vectors are rewritten and dispatched to
correctly, and its main loop runs a proper VBlank handshake through `$FF85`.

It gets through most of the intro: the background comes up, the logo's tile
map is written, the RLE decompressor that unpacks the logo tiles runs to
completion and fills VRAM, and the intro state variable at `$FF98` climbs
through 1, 3, 5 to 9. The logo itself is drawn in pieces rather than whole,
and the screen then sits still.

The fault that used to stop it dead — before the vector table was fixed the
framebuffer froze at frame 4 and `$FF98` never left 1 — was the transposed
VBlank and LCD status vectors. The main loop's VBlank handshake is gated on
an LCD status interrupt, and with the two swapped it was never taken, so the
loop spun forever on `$FF85`. Fixing that moved the game from frame 4 to
frame 14 and set the state variable moving; what remains is the logo art
itself, not the state machine.

Everything checked against the two references matches: 256 instruction
timings, the opcode and CB opcode tables, the sound register addresses, the
post-boot register values, the port select bits, the mode 3 length, and the
memory map. The CPU, timer, joypad, interrupts and OAM DMA all behave sanely
under inspection and under the synthetic ROMs in the test suite. What has not
been found yet is whatever this particular title needs for the logo's
character tiles to come out whole. The likeliest candidates left are VRAM and
OAM access not being blocked during mode 3, which is documented but not
implemented here, and finer PPU timing than the scanline renderer provides.

## Credits

The emulator is written from the two hardware references that ship in this
repository. Both are reused under terms that allow redistribution:

* **Pan Docs** (`pandocs_specs.pdf`) — the Game Boy programming manual
  maintained at github.com/gbdev/pandocs, released under **CC0**, a public
  domain dedication. Used for the post-boot register values, the sound
  register addresses, the timer, and the pixel transfer timing.
* **Game Boy Complete Technical Reference** (`gbctr.pdf`) by gekkio, at
  gekkio.fi, licensed under **Creative Commons Attribution-ShareAlike 4.0**.
  Used for the instruction timing tables and the register bit layouts.

`demo.gb` is generated by `tools/make_demo_rom.py` and is ours. No
commercial ROM is included.

## Licence

See `LICENSE`. Game Boy and Game Boy Color are trademarks of Nintendo. This
project contains no Nintendo code or ROM data.
