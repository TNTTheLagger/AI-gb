/*
 * platform_win32.c -- the Windows side of the platform layer.
 *
 * Keys are read as console input events and translated into the same byte
 * stream the POSIX build produces, so the frontend needs no #ifdefs and
 * arrow keys arrive as ANSI escape sequences on both systems.
 */
#if defined(_WIN32)

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>

#include "platform.h"

/* ------------------------------------------------------------------ */
/* Console mode                                                       */
/* ------------------------------------------------------------------ */

static DWORD saved_in_mode;
static DWORD saved_out_mode;
static UINT saved_out_cp;
static bool in_mode_saved;
static bool out_mode_saved;
static bool cp_saved;
static bool interactive;

/* Remembered so the window can be handed back its original size on exit. */
static bool console_saved;
static COORD saved_size;

bool platform_begin_interactive(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;

    if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE)
        return false;
    if (GetConsoleMode(in, &mode) == 0)
        return false;
    if (GetConsoleMode(out, &mode) == 0)
        return false;

    saved_in_mode = mode;
    saved_out_mode = mode;
    in_mode_saved = true;
    out_mode_saved = true;

    /* UTF-8 output, otherwise the block glyphs come out as mojibake. */
    {
        UINT cp = GetConsoleOutputCP();
        saved_out_cp = cp;
        cp_saved = true;
        if (cp != CP_UTF8)
            SetConsoleOutputCP(CP_UTF8);
    }

    /* Input: no line editing, no echo, no Ctrl-C termination.  The casts
     * keep the complement of these bit masks from being done in int. */
    mode = saved_in_mode;
    mode &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                     ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_EXTENDED_FLAGS;
    if (SetConsoleMode(in, mode) == 0) {
        in_mode_saved = false;
        return false;
    }

    /* Output: interpret ANSI escapes, and stop Windows adding newlines. */
    mode = saved_out_mode;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    mode &= ~(DWORD)DISABLE_NEWLINE_AUTO_RETURN;
    if (SetConsoleMode(out, mode) == 0) {
        SetConsoleMode(in, saved_in_mode);
        in_mode_saved = false;
        out_mode_saved = false;
        return false;
    }

    interactive = true;
    printf("\033[?1049h\033[?25l");
    fflush(stdout);
    return true;
}

void platform_end_interactive(void)
{
    if (!interactive)
        return;

    printf("\033[0m\033[?25h\033[?1049l");
    fflush(stdout);

    if (in_mode_saved)
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved_in_mode);
    if (out_mode_saved)
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved_out_mode);
    if (cp_saved)
        SetConsoleOutputCP(saved_out_cp);

    /* Give the window back the size it had before we borrowed it. */
    if (console_saved) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        SMALL_RECT want;

        want.Left = 0;
        want.Top = 0;
        want.Right = (SHORT)(saved_size.X - 1);
        want.Bottom = (SHORT)(saved_size.Y - 1);
        if (SetConsoleScreenBufferSize(out, saved_size))
            SetConsoleWindowInfo(out, TRUE, &want);
        console_saved = false;
    }

    interactive = false;
}

void platform_terminal_size(int *cols, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    *cols = 80;
    *rows = 24;

    if (GetConsoleScreenBufferInfo(out, &info)) {
        /* The buffer can be larger than the visible window after a resize,
         * so use the window rectangle rather than the buffer size. */
        int w = info.srWindow.Right - info.srWindow.Left + 1;
        int h = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (w > 0)
            *cols = w;
        if (h > 0)
            *rows = h;
    }
}

bool platform_supports_utf8(void)
{
    return GetConsoleOutputCP() == CP_UTF8;
}

/* ------------------------------------------------------------------ */
/* Resizing the console window                                         */
/* ------------------------------------------------------------------ */

bool platform_can_resize_console(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;

    /* Only a real console has a window we can move.  Under a Windows
     * Terminal tab or a pipe there is nothing to do. */
    return out != INVALID_HANDLE_VALUE &&
           GetConsoleScreenBufferInfo(out, &info);
}

bool platform_resize_console(int cols, int rows)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    SMALL_RECT want;
    COORD size;

    if (!platform_can_resize_console())
        return false;
    if (cols < 1 || rows < 1 || cols > 10000 || rows > 10000)
        return false;

    GetConsoleScreenBufferInfo(out, &info);

    if (info.dwSize.X >= cols && info.dwSize.Y >= rows) {
        /* Already at least this big.  Never shrink. */
        if (!console_saved) {
            saved_size = info.dwSize;
            console_saved = true;
        }
        return false;
    }

    if (!console_saved) {
        saved_size = info.dwSize;
        console_saved = true;
    }

    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;

    /*
     * The window cannot be larger than the buffer behind it, so the buffer
     * has to grow first.  Setting the buffer size also slides the window
     * around, which is why the window rectangle is set afterwards to pin
     * it to the top left and fill the new buffer exactly.
     */
    want.Left = 0;
    want.Top = 0;
    want.Right = (SHORT)(cols - 1);
    want.Bottom = (SHORT)(rows - 1);

    if (!SetConsoleScreenBufferSize(out, size))
        return false;
    if (!SetConsoleWindowInfo(out, TRUE, &want))
        return false;
    if (!SetConsoleCursorPosition(out, (COORD){ 0, 0 }))
        return false; /* harmless if it fails */

    return true;
}

/* ------------------------------------------------------------------ */
/* Key input                                                          */
/* ------------------------------------------------------------------ */

/*
 * Keys that have a dedicated virtual key code are translated here.  The
 * keys that are just a character are handed to the shared decoder as an
 * ASCII byte, so both platforms agree on what, say, 'z' means.
 */
static PlatformKey translate_vk(DWORD vk, DWORD ctrl)
{
    /* Ctrl-C, with processed input disabled, arrives as an ordinary key
     * rather than killing the process. */
    if (vk == 'C' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0)
        return PLATFORM_KEY_QUIT;

    switch (vk) {
    case VK_UP:     return PLATFORM_KEY_UP;
    case VK_DOWN:   return PLATFORM_KEY_DOWN;
    case VK_LEFT:   return PLATFORM_KEY_LEFT;
    case VK_RIGHT:  return PLATFORM_KEY_RIGHT;
    case VK_HOME:   return PLATFORM_KEY_UP;    /* terminal conventions */
    case VK_END:    return PLATFORM_KEY_DOWN;
    case VK_ESCAPE: return PLATFORM_KEY_QUIT;
    case VK_SPACE:  return PLATFORM_KEY_SELECT;
    case VK_RETURN: return PLATFORM_KEY_START;
    default:        return PLATFORM_KEY_UNMAPPED;
    }
}

/* The ASCII character a virtual key code stands for, or 0 if it has none. */
static char vk_to_ascii(DWORD vk)
{
    /* Letters and digits have virtual key codes equal to their ASCII. */
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return (char)vk;

    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return (char)('0' + (vk - VK_NUMPAD0));

    return 0;
}

int platform_read_keys(PlatformKey *out, int cap)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD events[64];
    PlatformKey keys[64];
    int produced = 0;
    DWORD count = 0;
    int kept;

    if (!out || cap <= 0)
        return 0;

    if (WaitForSingleObject(in, 0) != WAIT_OBJECT_0)
        return 0;
    if (!ReadConsoleInput(in, events, 64, &count))
        return 0;

    /* Drain everything the console has queued before deciding what fits in
     * the caller's buffer, otherwise a burst of typing would leave the
     * console stuck holding events we never look at again. */
    for (DWORD i = 0; i < count; i++) {
        const KEY_EVENT_RECORD *ev;
        PlatformKey key;
        char ch;

        if (events[i].EventType != KEY_EVENT)
            continue;
        if (!events[i].Event.KeyEvent.bKeyDown)
            continue; /* ignore auto-repeat and key releases */

        ev = &events[i].Event.KeyEvent;
        key = translate_vk(ev->wVirtualKeyCode, ev->dwControlKeyState);

        if (key == PLATFORM_KEY_UNMAPPED) {
            ch = vk_to_ascii(ev->wVirtualKeyCode);
            if (ch) {
                /* One ASCII byte always decodes to exactly one key. */
                platform_decode_stream(&ch, 1, 0, &keys[produced], 1);
                produced++;
            } else {
                /* Function keys, the numpad operators, and so on. */
                keys[produced++] = PLATFORM_KEY_UNMAPPED;
            }
            continue;
        }

        keys[produced++] = key;
    }

    kept = produced < cap ? produced : cap;
    for (int i = 0; i < kept; i++)
        out[i] = keys[i];

    return kept;
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

static double perf_frequency(void)
{
    static LARGE_INTEGER freq;
    static bool known;

    if (!known) {
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
            freq.QuadPart = 1000;
        known = true;
    }
    return (double)freq.QuadPart;
}

double platform_now(void)
{
    static LARGE_INTEGER origin;
    static bool known;
    LARGE_INTEGER now;

    if (!QueryPerformanceCounter(&now))
        return (double)GetTickCount64() / 1000.0;

    if (!known) {
        origin = now;
        known = true;
    }

    return (double)(now.QuadPart - origin.QuadPart) / perf_frequency();
}

void platform_sleep_ms(int ms)
{
    if (ms > 0)
        Sleep((DWORD)ms);
}

/* ------------------------------------------------------------------ */
/* Child processes                                                     */
/* ------------------------------------------------------------------ */

bool platform_have_program(const char *name)
{
    char path[1024];
    DWORD len;
    char *p;

    if (!name || !*name)
        return false;

    len = GetEnvironmentVariableA("PATH", path, (DWORD)sizeof(path));
    if (len == 0 || len >= sizeof(path))
        return false;

    p = path;
    while (*p) {
        char *sep = strchr(p, ';');
        size_t dir_len = sep ? (size_t)(sep - p) : strlen(p);

        if (dir_len > 0 && dir_len + 1 + strlen(name) < sizeof(path)) {
            memcpy(path + dir_len, "\\", 2);
            strcpy(path + dir_len + 1, name);
            if (_access(path, 0) == 0)
                return true;
        }
        if (!sep)
            break;
        p = sep + 1;
    }
    return false;
}

const char *platform_default_player(void)
{
    if (platform_have_program("ffplay.exe"))
        return "ffplay -nodisp -autoexit -loglevel quiet -f s16le "
               "-ar 44100 -ac 2 -i -";
    /* PowerShell can play a WAV file, so wrap the stream in one. */
    if (platform_have_program("powershell.exe"))
        return "powershell -NoProfile -Command "
               "$b=New-Object byte[] 65536;"
               "$s=New-Object System.IO.MemoryStream;"
               "while(($n=[Console]::OpenStandardInput().Read($b,0,65536)) -gt 0){"
               "$s.Write($b,0,$n)};"
               "$w=New-Object System.IO.MemoryStream("
               "New-Object System.IO.BinaryReader($s.ToArray()).ReadUInt16(),0);"
               "[System.Media.SoundPlayer]::new($w).PlaySync()";
    return NULL;
}

FILE *platform_open_player(const char *command)
{
    if (!command)
        return NULL;
    return _popen(command, "wb");
}

int platform_close_player(FILE *pipe)
{
    if (!pipe)
        return 0;
    return _pclose(pipe);
}

/* ------------------------------------------------------------------ */
/* Files and scratch space                                             */
/* ------------------------------------------------------------------ */

FILE *platform_fopen(const char *path, const char *mode)
{
    FILE *f = NULL;
    wchar_t *wpath;
    wchar_t wmode[8];
    size_t i;
    int len;

    len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (len > 0) {
        wpath = (wchar_t *)malloc((size_t)len * sizeof(wchar_t));
        if (wpath) {
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, len);
            for (i = 0; mode[i] && i < sizeof(wmode) / sizeof(wmode[0]) - 1; i++)
                wmode[i] = (wchar_t)mode[i];
            wmode[i] = 0;
            f = _wfopen(wpath, wmode);
            free(wpath);
        }
    }

    /* Fall back to the narrow API, which uses the active code page. */
    return f ? f : fopen(path, mode);
}

const char *platform_temp_dir(void)
{
    const char *dir = getenv("TEMP");

    if (dir && *dir)
        return dir;
    dir = getenv("TMP");
    if (dir && *dir)
        return dir;
    return ".\\";
}

#endif /* _WIN32 */

/* Exactly one of the two platform files compiles on any given host and the
 * other becomes empty, which ISO C does not allow and MSVC warns about
 * under /Wall.  One declaration keeps the unit well formed. */
typedef int gb_platform_unit_not_empty;
