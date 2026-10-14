# Build the emulator and its unit tests with a plain Makefile.
#
#   Linux / macOS (gcc or clang):   make
#   Windows (MinGW-w64):            mingw32-make
#   Windows (MSVC):                 build.bat
#
# The same source list builds everywhere: src/platform_posix.c and
# src/platform_win32.c each compile to nothing on the wrong host.
#
# CFLAGS is yours to override, for instance
#
#   make CFLAGS="-O1 -g -fsanitize=address,undefined" \
#        LDFLAGS="-fsanitize=address,undefined"
#
# and the flags the build cannot do without are kept regardless.

CC       ?= cc
CFLAGS   ?= -O2
LDFLAGS  ?=
LDLIBS   ?=

# Not overridable: the code is C11 and needs its own headers to be found.
BASE_CFLAGS := -std=c11 -Wall -Wextra -Isrc -Itests
COMPILE     := $(CC) $(BASE_CFLAGS) $(CFLAGS)
LINK        := $(CC) $(CFLAGS) $(LDFLAGS)

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME_S),Windows_NT)
    EXE := .exe
else
    EXE :=
    # POSIX only: the maths functions live in a separate library.
    LDLIBS += -lm
endif

# Track header dependencies so editing a header rebuilds everything that
# includes it.  Without this, changing a struct silently leaves stale
# objects behind and the binary ends up with mismatched layouts.
DEPFLAGS := -MMD -MP

CORE_SRC := src/gb.c src/cpu.c src/memory.c src/io.c src/ppu.c src/apu.c \
            src/timer.c src/joypad.c src/serial.c src/cart.c \
            src/platform_posix.c src/platform_win32.c src/platform_keys.c \
            src/keymap.c src/layout.c
CORE_OBJ := $(CORE_SRC:.c=.o)

EMU_SRC  := src/main.c src/frontend_term.c
EMU_OBJ  := $(EMU_SRC:.c=.o)

TEST_SRC := tests/main.c tests/fixture.c tests/test_cpu.c \
            tests/test_timings.c tests/test_memory.c \
            tests/test_cart.c tests/test_timer.c tests/test_ppu.c \
            tests/test_apu.c tests/test_input.c tests/test_state.c \
            tests/test_platform.c tests/test_layout.c \
            tests/test_integration.c
TEST_OBJ := $(TEST_SRC:.c=.o)

ALL_OBJ := $(CORE_OBJ) $(EMU_OBJ) $(TEST_OBJ)

ROMS ?=

.PHONY: all clean test run timings

all: gbcpu$(EXE) gbtests$(EXE)

gbcpu$(EXE): $(CORE_OBJ) $(EMU_OBJ)
	$(LINK) -o $@ $^ $(LDLIBS)

gbtests$(EXE): $(CORE_OBJ) $(TEST_OBJ)
	$(LINK) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(COMPILE) $(DEPFLAGS) -c $< -o $@

-include $(ALL_OBJ:.o=.d)

test: gbtests$(EXE)
	./gbtests$(EXE)

run: gbcpu$(EXE)
	./gbcpu$(EXE) $(ROMS)

# Prints what every opcode costs, for tools/check_timings.py to compare
# against the hardware reference.
timings: gbtimings$(EXE)
	./gbtimings$(EXE) > timings.txt

gbtimings$(EXE): $(CORE_OBJ) tests/fixture.o tools/timings.o
	$(LINK) -o $@ $^ $(LDLIBS)

tools/timings.o: tools/timings.c
	$(COMPILE) -c $< -o $@

clean:
	rm -f $(ALL_OBJ) tools/timings.o gbcpu gbtests gbtimings timings.txt
	rm -f $(ALL_OBJ:.o=.d) tools/timings.d