/*
 * platform.h -- the thin layer between the emulator frontend and the
 * operating system.
 *
 * Everything the rest of the program needs from the host lives here:
 * terminal size, raw key input, timing, and opening a child process for
 * audio.  There is one implementation per supported system, in
 * platform_posix.c and platform_win32.c, and each compiles to nothing when
 * it is not the host.
 *
 * Input is reported as PlatformKey codes rather than raw bytes, so the
 * frontend never has to know whether it is talking to a Unix terminal or
 * to the Windows console.  The byte-level decoding of ANSI escape
 * sequences lives in platform_keys.c, which is shared and unit tested on
 * both systems.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/*
 * A key press, in the vocabulary the frontend works in.  The d-pad codes
 * are separate from the buttons because a host may report two directions
 * as two presses in a row, and holding both is how you get a diagonal.
 */
typedef enum {
    PLATFORM_KEY_NONE = 0,

    PLATFORM_KEY_UP,
    PLATFORM_KEY_DOWN,
    PLATFORM_KEY_LEFT,
    PLATFORM_KEY_RIGHT,

    PLATFORM_KEY_A,
    PLATFORM_KEY_B,
    PLATFORM_KEY_START,
    PLATFORM_KEY_SELECT,

    PLATFORM_KEY_PAUSE,   /* p */
    PLATFORM_KEY_RESET,   /* r */
    PLATFORM_KEY_MUTE,    /* m */
    PLATFORM_KEY_HELP,    /* ? */
    PLATFORM_KEY_QUIT,    /* q, escape, ctrl-C */

    /* Anything not bound to a command, such as a function key.  The
     * frontend ignores these but the decoder still reports them, which is
     * what makes it possible to test that they are swallowed cleanly. */
    PLATFORM_KEY_UNMAPPED,

    PLATFORM_KEY_COUNT
} PlatformKey;

/* Reads the keys that are already waiting and writes their codes into
 * `out`, returning how many were written.  Never blocks.  Anything the
 * frontend does not care about is reported as PLATFORM_KEY_UNMAPPED so
 * that the caller's buffer accounting stays simple. */
int platform_read_keys(PlatformKey *out, int cap);

/*
 * Decodes a chunk of terminal input into key codes.  This is the shared
 * half of input handling: it understands plain ASCII and the ANSI escape
 * sequences that terminals use for the arrow and function keys, and it is
 * compiled and unit tested on every platform.
 *
 * `at` is where to start in `buf`, so a caller can keep decoding the same
 * buffer one key at a time.  Returns the number of keys written to `out`.
 */
int platform_decode_stream(const char *buf, int len, int at, PlatformKey *out,
                           int cap);

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

/* Switches the terminal into raw mode and onto the alternate screen.
 * Returns false when the process is not attached to a terminal, in which
 * case the frontend runs without any of it. */
bool platform_begin_interactive(void);

/* Restores the terminal exactly as it was.  Safe to call even if
 * platform_begin_interactive() returned false. */
void platform_end_interactive(void);

/* Current terminal size, falling back to a sensible default. */
void platform_terminal_size(int *cols, int *rows);

/* Asks the host to make the terminal `cols` by `rows` cells, so a window
 * that is too small for a legible picture can be grown.  Only enlarging is
 * ever done: shrinking someone's window would be rude, and a terminal
 * emulator on a Unix host owns its own size, so this reports false there.
 * The previous size is remembered and put back by
 * platform_end_interactive(). */
bool platform_resize_console(int cols, int rows);

/* True if this platform can resize the terminal at all, so the frontend
 * can tell the user why it cannot rather than quietly doing nothing. */
bool platform_can_resize_console(void);

/* True when the output can render UTF-8 box drawing characters. */
bool platform_supports_utf8(void);

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

/* Monotonic wall clock, in seconds. */
double platform_now(void);

/* Sleep for roughly `ms` milliseconds. */
void platform_sleep_ms(int ms);

/* ------------------------------------------------------------------ */
/* Child processes                                                     */
/* ------------------------------------------------------------------ */

/* Looks for an executable on PATH. */
bool platform_have_program(const char *name);

/* A command that reads raw signed 16-bit stereo PCM on stdin, or NULL if
 * the machine has nothing that can play it. */
const char *platform_default_player(void);

/* Opens `command` for writing, or returns NULL. */
FILE *platform_open_player(const char *command);
int platform_close_player(FILE *pipe);

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

/* fopen() that copes with non-ASCII paths on Windows. */
FILE *platform_fopen(const char *path, const char *mode);

/* A writable directory for scratch files, with a trailing separator. */
const char *platform_temp_dir(void);

#endif /* PLATFORM_H */