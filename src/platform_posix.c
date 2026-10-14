/*
 * platform_posix.c -- the POSIX and macOS side of the platform layer.
 */
#if !defined(_WIN32)

#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>

#include "platform.h"

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

static struct termios saved_termios;
static bool raw_active;

/* The size to put back when we are done, captured the first time a resize
 * is asked for so that quitting leaves the terminal as we found it. */
static int saved_cols, saved_rows;
static bool resize_saved;

bool platform_begin_interactive(void)
{
    struct termios t;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return false;

    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0)
        return false;

    t = saved_termios;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0)
        return false;

    raw_active = true;

    /* Alternate screen, cursor hidden, and leave the colours alone. */
    printf("\033[?1049h\033[?25l");
    fflush(stdout);
    return true;
}

void platform_end_interactive(void)
{
    if (!raw_active)
        return;

    printf("\033[0m\033[?25h\033[?1049l");
    if (resize_saved)
        printf("\033[8;%d;%dt", saved_rows, saved_cols);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    raw_active = false;
}

void platform_terminal_size(int *cols, int *rows)
{
    struct winsize ws;
    const char *env;

    *cols = 80;
    *rows = 24;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
        ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return;
    }

    /* Fall back to the environment when there is no real terminal. */
    if ((env = getenv("COLUMNS")) != NULL && atoi(env) > 0)
        *cols = atoi(env);
    if ((env = getenv("LINES")) != NULL && atoi(env) > 0)
        *rows = atoi(env);
}

bool platform_can_resize_console(void)
{
    /*
     * On a Unix host the window belongs to the terminal emulator rather
     * than to us, so this is a request rather than a command.  It is still
     * worth making: CSI 8 ; rows ; cols t is understood by xterm and every
     * terminal that copied its behaviour, including iTerm2, WezTerm, kitty
     * and GNOME Terminal.  A terminal that does not implement it simply
     * ignores the sequence, and the picture is scaled to whatever room
     * there is, so trying costs nothing.
     */
    return isatty(STDOUT_FILENO) != 0;
}

bool platform_resize_console(int cols, int rows)
{
    if (!platform_can_resize_console())
        return false;
    if (cols < 1 || rows < 1 || cols > 10000 || rows > 10000)
        return false;

    {
        int have_cols = 0, have_rows = 0;

        platform_terminal_size(&have_cols, &have_rows);
        if (!resize_saved) {
            saved_cols = have_cols;
            saved_rows = have_rows;
            resize_saved = true;
        }
        if (have_cols >= cols && have_rows >= rows)
            return false; /* never shrink someone's terminal */
    }

    /*
     * CSI 8 ; rows ; cols t asks the terminal to resize its own window.
     * The window manager is free to clamp the result to the screen, and
     * most do, which is exactly the behaviour wanted here.
     */
    printf("\033[8;%d;%dt", rows, cols);
    fflush(stdout);
    return true;
}

int platform_read_keys(PlatformKey *out, int cap)
{
    fd_set set;
    struct timeval tv = { 0, 0 };
    char buf[64];
    ssize_t n;

    if (!out || cap <= 0)
        return 0;

    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    if (select(STDIN_FILENO + 1, &set, NULL, NULL, &tv) <= 0)
        return 0;

    n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0)
        return 0;

    return platform_decode_stream(buf, (int)n, 0, out, cap);
}

bool platform_supports_utf8(void)
{
    const char *vars[3];
    int i;

    vars[0] = getenv("LC_ALL");
    vars[1] = getenv("LC_CTYPE");
    vars[2] = getenv("LANG");

    for (i = 0; i < 3; i++) {
        const char *v = vars[i];
        if (v && *v) {
            return strstr(v, "UTF-8") != NULL || strstr(v, "utf-8") != NULL ||
                   strstr(v, "UTF8") != NULL || strstr(v, "utf8") != NULL;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

double platform_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void platform_sleep_ms(int ms)
{
    struct timespec ts;

    if (ms <= 0)
        return;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Child processes                                                     */
/* ------------------------------------------------------------------ */

bool platform_have_program(const char *name)
{
    const char *path;
    char buf[512];

    if (!name || !*name)
        return false;

    path = getenv("PATH");
    if (!path || !*path)
        return false;

    while (*path) {
        const char *sep = strchr(path, ':');
        size_t len = sep ? (size_t)(sep - path) : strlen(path);

        if (len > 0 && len + 1 + strlen(name) < sizeof(buf)) {
            memcpy(buf, path, len);
            buf[len] = '/';
            strcpy(buf + len + 1, name);
            if (access(buf, X_OK) == 0)
                return true;
        }
        if (!sep)
            break;
        path = sep + 1;
    }
    return false;
}

const char *platform_default_player(void)
{
    if (platform_have_program("ffplay"))
        return "ffplay -nodisp -autoexit -loglevel quiet -f s16le "
               "-ar 44100 -ac 2 -i -";
    if (platform_have_program("paplay"))
        return "paplay --format=s16le --rate=44100 --channels=2";
    if (platform_have_program("aplay"))
        return "aplay -q -f S16_LE -r 44100 -c 2";
    if (platform_have_program("afplay"))
        return "afplay -f S16_LE -r 44100 -c 2";
    return NULL;
}

FILE *platform_open_player(const char *command)
{
    if (!command)
        return NULL;
    return popen(command, "w");
}

int platform_close_player(FILE *pipe)
{
    if (!pipe)
        return 0;
    return pclose(pipe);
}

/* ------------------------------------------------------------------ */
/* Files and scratch space                                             */
/* ------------------------------------------------------------------ */

FILE *platform_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

const char *platform_temp_dir(void)
{
    const char *dir = getenv("TMPDIR");

    if (dir && *dir)
        return dir;
    return "/tmp/";
}

#endif /* !_WIN32 */

/* Exactly one of the two platform files compiles on any given host and the
 * other becomes empty, which ISO C does not allow and MSVC warns about
 * under /Wall.  One declaration keeps the unit well formed. */
typedef int gb_platform_unit_not_empty;
