/*
 * platform_keys.c -- turning a terminal's byte stream into key codes.
 *
 * A terminal hands us bytes: printable keys arrive as themselves and the
 * arrow and function keys as ANSI escape sequences.  Windows consoles
 * report virtual key codes instead, so platform_win32.c translates those
 * directly and never comes through here.  Everything else on both systems
 * shares this file, which is why it has no operating system calls at all
 * and can be unit tested anywhere.
 */
#include "platform.h"

#include <string.h>

/* The escape sequences we understand, and the key each one means.
 * Lengths are measured rather than written down, because a length that is
 * off by one silently matches a prefix of the wrong sequence. */
typedef struct {
    const char *seq;
    PlatformKey key;
} Escape;

static const Escape escapes[] = {
    /* CSI A/B/C/D, and the H/F forms some terminals send for Home/End. */
    { "\033[A", PLATFORM_KEY_UP },
    { "\033[B", PLATFORM_KEY_DOWN },
    { "\033[C", PLATFORM_KEY_RIGHT },
    { "\033[D", PLATFORM_KEY_LEFT },
    { "\033[H", PLATFORM_KEY_UP },
    { "\033[F", PLATFORM_KEY_DOWN },

    /* Arrows in application cursor key mode, as xterm can be set up. */
    { "\033OA", PLATFORM_KEY_UP },
    { "\033OB", PLATFORM_KEY_DOWN },
    { "\033OC", PLATFORM_KEY_RIGHT },
    { "\033OD", PLATFORM_KEY_LEFT },

    /* Ctrl+arrow and friends, which carry a modifier parameter. */
    { "\033[1;5A", PLATFORM_KEY_UP },
    { "\033[1;5B", PLATFORM_KEY_DOWN },
    { "\033[1;5C", PLATFORM_KEY_RIGHT },
    { "\033[1;5D", PLATFORM_KEY_LEFT },

    /* Delete. */
    { "\033[3~", PLATFORM_KEY_UNMAPPED }
};

static PlatformKey map_ascii(int c)
{
    switch (c) {
    /* D-pad: the arrows themselves, and the WASD and HJKL clusters. */
    case 'w': case 'W': case 'k': case 'K': return PLATFORM_KEY_UP;
    case 's': case 'S': case 'j': case 'J': return PLATFORM_KEY_DOWN;
    case 'a': case 'A': case 'h': case 'H': return PLATFORM_KEY_LEFT;
    case 'd': case 'D': case 'l': case 'L': return PLATFORM_KEY_RIGHT;

    case 'z': case 'Z': return PLATFORM_KEY_A;
    case 'x': case 'X': return PLATFORM_KEY_B;
    case '\r': case '\n': return PLATFORM_KEY_START;
    case ' ':                return PLATFORM_KEY_SELECT;

    case 'p': case 'P': return PLATFORM_KEY_PAUSE;
    case 'r': case 'R': return PLATFORM_KEY_RESET;
    case 'm': case 'M': return PLATFORM_KEY_MUTE;
    case '?':              return PLATFORM_KEY_HELP;

    /* Escape, Ctrl-C (the raw mode has processed input turned off, so it
     * arrives as an ordinary 0x03) and the q that quits. */
    case 0x1B: case 0x03: case 'q': case 'Q': return PLATFORM_KEY_QUIT;

    default: return PLATFORM_KEY_UNMAPPED;
    }
}

static int emit(PlatformKey *out, int cap, int used, PlatformKey key)
{
    if (used < cap)
        out[used] = key;
    return used + 1;
}

int platform_decode_stream(const char *buf, int len, int at, PlatformKey *out,
                           int cap)
{
    int used = 0;

    if (!buf || !out || cap <= 0)
        return 0;

    while (at < len) {
        unsigned char c = (unsigned char)buf[at];

        if (c == 0x1B && at + 1 < len) {
            size_t i;
            bool matched = false;

            for (i = 0; i < sizeof(escapes) / sizeof(escapes[0]); i++) {
                const Escape *e = &escapes[i];
                size_t elen = strlen(e->seq);

                if (at + (int)elen > len ||
                    memcmp(buf + at, e->seq, elen) != 0)
                    continue;

                used = emit(out, cap, used, e->key);
                at += (int)elen;
                matched = true;
                break;
            }
            if (matched)
                continue;

            /* An escape sequence we do not know.  Consume the whole thing
             * rather than reporting its letters as key presses: a stray
             * bracket sequence should never make the emulator jump around.
             * One unmapped event stands for the lot, whether or not the
             * sequence was complete when the read ended. */
            if (buf[at + 1] == '[' || buf[at + 1] == 'O') {
                int end = at + 2;

                while (end < len) {
                    unsigned char p = (unsigned char)buf[end];
                    /* Parameter and intermediate bytes run to the final
                     * byte, which is in the range @ through ~. */
                    if (p >= 0x40 && p <= 0x7E)
                        break;
                    end++;
                }
                used = emit(out, cap, used, PLATFORM_KEY_UNMAPPED);
                at = (end < len) ? end + 1 : len;
                continue;
            }

            /* A bare escape followed by something else. */
            used = emit(out, cap, used, PLATFORM_KEY_QUIT);
            at++;
            continue;
        }

        used = emit(out, cap, used, map_ascii(c));
        at++;
    }

    return used;
}