/*
 * frontend_term.c -- a terminal video and input backend.
 *
 * The 160x144 framebuffer is scaled to whatever the terminal can hold and
 * drawn with the half block glyph, so one character cell shows two
 * vertically stacked pixels.  Where colour or UTF-8 is unavailable it
 * falls back to block characters, or to a plain ASCII density ramp.
 *
 * This file contains no operating system calls: everything the host can
 * provide comes through platform.h, which has a POSIX and a Windows
 * implementation.  A terminal reports key presses but never releases, so a
 * keypress holds the button for a short moment and then lets go; tapping
 * again holds it longer.  That is enough for a d-pad.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend.h"
#include "keymap.h"
#include "layout.h"
#include "platform.h"

#define AUDIO_RATE 44100
#define REDRAW_HZ 30.0 /* how often the picture is sent to the terminal */

/* How long a note stays on the status line before the line goes back to
 * reporting the current state. */
#define NOTE_SECONDS 2.5

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Optional audio: raw S16LE stereo piped to an external player         */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *pipe;
    int16_t buffer[2048];
    size_t filled;
    bool muted;
} AudioOut;

static void audio_flush(AudioOut *a);

static bool audio_open(AudioOut *a, const char *setting)
{
    const char *cmd = setting;

    memset(a, 0, sizeof(*a));

    if (!setting || strcmp(setting, "none") == 0)
        return false;

    if (strcmp(setting, "auto") == 0) {
        cmd = platform_default_player();
        if (!cmd)
            return false; /* nothing on this machine can play PCM */
    }

    a->pipe = platform_open_player(cmd);
    return a->pipe != NULL;
}

static void audio_feed(AudioOut *a, GB *gb)
{
    size_t capacity = sizeof(a->buffer) / (2 * sizeof(a->buffer[0]));
    size_t room;

    if (!a->pipe || a->muted) {
        /* Still consume samples so the ring never backs up. */
        while (apu_available(&gb->apu) > APU_RING_SIZE / 2) {
            int16_t scratch[256];
            apu_drain(&gb->apu, scratch, 128);
        }
        return;
    }

    if (a->filled >= capacity) {
        audio_flush(a);
        return;
    }

    room = capacity - a->filled;
    apu_drain(&gb->apu, a->buffer + a->filled * 2, room);
    a->filled += room;

    if (a->filled >= capacity)
        audio_flush(a);
}

static void audio_flush(AudioOut *a)
{
    if (!a->pipe || !a->filled)
        return;
    fwrite(a->buffer, sizeof(int16_t), a->filled * 2, a->pipe);
    a->filled = 0;
    fflush(a->pipe);
}

static void audio_close(AudioOut *a)
{
    if (!a->pipe)
        return;
    audio_flush(a);
    platform_close_player(a->pipe);
    a->pipe = NULL;
}

/* ------------------------------------------------------------------ */
/* BMP screenshots                                                     */
/* ------------------------------------------------------------------ */

static void put_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    fwrite(b, 1, 4, f);
}

static void put_u16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
    fwrite(b, 1, 2, f);
}

bool frontend_write_bmp(const GB *gb, const char *path)
{
    FILE *f = platform_fopen(path, "wb");
    uint32_t w = GB_SCREEN_W, h = GB_SCREEN_H;
    uint32_t row_bytes = w * 3;
    uint32_t row_padding = (4u - (row_bytes % 4u)) % 4u;
    uint32_t data_size = (row_bytes + row_padding) * h;

    if (!f)
        return false;

    fwrite("BM", 1, 2, f);
    put_u32(f, 14 + 40 + data_size);
    put_u32(f, 0);
    put_u32(f, 14 + 40);

    put_u32(f, 40);           /* header size        */
    put_u32(f, w);
    put_u32(f, h);
    put_u16(f, 1);
    put_u16(f, 24);
    put_u32(f, 0);            /* BI_RGB             */
    put_u32(f, data_size);
    put_u32(f, 2835);
    put_u32(f, 2835);
    put_u32(f, 0);
    put_u32(f, 0);

    for (int y = (int)h - 1; y >= 0; y--) { /* BMP rows run bottom up */
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = gb->ppu.fb[(size_t)y * w + x];
            uint8_t rgb[3];
            rgb[0] = (uint8_t)((p >> 16) & 0xFF);
            rgb[1] = (uint8_t)((p >> 8) & 0xFF);
            rgb[2] = (uint8_t)(p & 0xFF);
            fwrite(rgb, 1, 3, f);
        }
        for (uint32_t i = 0; i < row_padding; i++)
            fputc(0, f);
    }

    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ */
/* Screen renderer                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    bool use_color;
    bool use_unicode;
} Glyphs;

/* Density ramp for terminals that can only do plain ASCII. */
static const char *const ascii_ramp[4] = { " ", ".", "o", "#" };

static int shade_level(uint32_t rgb)
{
    int lum = (int)(((rgb >> 16) & 0xFF) * 30 + ((rgb >> 8) & 0xFF) * 59 +
                    (rgb & 0xFF) * 11) / 100;

    if (lum > 200) return 0;
    if (lum > 140) return 1;
    if (lum > 70)  return 2;
    return 3;
}

/* Area average over a block of the framebuffer.  Used when shrinking, where
 * several source pixels share one cell and dropping any of them would lose
 * detail. */
static uint32_t sample_block(const GB *gb, int x0, int x1, int y0, int y1)
{
    uint64_t r = 0, g = 0, b = 0;
    uint32_t n = 0;

    for (int y = y0; y < y1; y++) {
        const uint32_t *row = &gb->ppu.fb[(size_t)y * GB_SCREEN_W];
        for (int x = x0; x < x1; x++) {
            r += (row[x] >> 16) & 0xFF;
            g += (row[x] >> 8) & 0xFF;
            b += row[x] & 0xFF;
            n++;
        }
    }
    if (n == 0)
        return 0;
    return (uint32_t)(((r / n) << 16) | ((g / n) << 8) | (b / n));
}

/* A single pixel, for when the picture is being enlarged and each source
 * pixel covers more than one cell.  Averaging here would wash every group
 * of enlarged pixels into one flat colour. */
static uint32_t sample_pixel(const GB *gb, int x, int y)
{
    return gb->ppu.fb[(size_t)y * GB_SCREEN_W + x];
}

/* Pick the glyph for one cell when we have no colour to work with. */
static const char *glyph_for(int top, int bottom, bool unicode)
{
    if (unicode) {
        if (top >= 3 && bottom >= 3) return "\xE2\x96\x88"; /* full block */
        if (top >= 3) return "\xE2\x96\x80";               /* upper half */
        if (bottom >= 3) return "\xE2\x96\x84";             /* lower half */
    }
    {
        int level = top > bottom ? top : bottom;
        return ascii_ramp[level];
    }
}

/*
 * Draws the screen into `cols` by `rows`, scaling it to fit and centring
 * whatever is left over.  A cell carries two stacked pixels, so choosing
 * the grid is arithmetic rather than drawing; layout.c does that and this
 * only fills the cells in.
 */
static void draw_screen(GB *gb, int cols, int rows, double aspect,
                        const Glyphs *glyphs)
{
    ScreenLayout layout;
    static char line[65536];

    /* The colours the terminal is currently set to.  A Game Boy screen is
     * mostly long runs of one shade, and a 24 bit colour escape is around
     * forty bytes, so sending one per cell costs about twelve megabytes a
     * second at a large window and stops the emulator reading its input
     * while it blocks on the write.  Emitting only on a change is what
     * keeps the picture cheap to draw. */
    uint32_t cur_fg = 0xFFFFFFFFu, cur_bg = 0xFFFFFFFFu;

    if (cols < 1 || rows < 1)
        return;

    layout = layout_compute(cols, rows, aspect);
    if (layout.cells_x < 1 || layout.cells_y < 1)
        return;

    for (int cy = 0; cy < layout.cells_y; cy++) {
        size_t len = 0;
        int row = layout.origin_y + cy + 1;

        for (int i = 0; i < layout.origin_x; i++) {
            if (len + 1 >= sizeof(line))
                break;
            line[len++] = ' ';
        }

        for (int cx = 0; cx < layout.cells_x; cx++) {
            uint32_t top, bot;

            if (layout.upscale) {
                int x, y_top, y_bottom;

                layout_cell_pixel(&layout, cx, cy, &x, &y_top, &y_bottom);
                top = sample_pixel(gb, x, y_top);
                bot = sample_pixel(gb, x, y_bottom);
            } else {
                int x0, x1, y0, y1, y2;

                layout_cell_area(&layout, cx, cy, &x0, &x1, &y0, &y1, &y2);
                top = sample_block(gb, x0, x1, y0, y1);
                bot = sample_block(gb, x0, x1, y1, y2);
            }

            if (glyphs->use_color) {
                size_t room = sizeof(line) - len;
                int n;

                if (top != cur_fg) {
                    n = snprintf(line + len, room,
                                 "\033[38;2;%u;%u;%um",
                                 (top >> 16) & 0xFF, (top >> 8) & 0xFF,
                                 top & 0xFF);
                    if (n < 0 || (size_t)n >= room)
                        break;
                    len += (size_t)n;
                    cur_fg = top;
                }
                if (bot != cur_bg) {
                    n = snprintf(line + len, room,
                                 "\033[48;2;%u;%u;%um",
                                 (bot >> 16) & 0xFF, (bot >> 8) & 0xFF,
                                 bot & 0xFF);
                    if (n < 0 || (size_t)n >= sizeof(line) - len)
                        break;
                    len += (size_t)n;
                    cur_bg = bot;
                }
                if (len + 3 >= sizeof(line))
                    break;
                memcpy(line + len, "\xE2\x96\x80", 3); /* upper half block */
                len += 3;
            } else {
                const char *g = glyph_for(shade_level(top), shade_level(bot),
                                          glyphs->use_unicode);
                size_t glen = strlen(g);
                if (len + glen + 8 >= sizeof(line))
                    break;
                memcpy(line + len, g, glen);
                len += glen;
            }
        }

        /*
         * The erase to end of line is done with the colours off, so the
         * margin beside the picture is the terminal's own background
         * rather than whichever shade the last cell happened to be.  That
         * means the colours really are back to their defaults afterwards,
         * so the run tracking starts again on the next line.
         */
        printf("\033[%d;1H%.*s\033[0m\033[K", row, (int)len, line);
        cur_fg = 0xFFFFFFFFu;
        cur_bg = 0xFFFFFFFFu;
    }

    printf("\033[0m");
}

static const char *mapper_name(const Cart *c)
{
    switch (c->mapper) {
    case MAPPER_MBC1: return "MBC1";
    case MAPPER_MBC2: return "MBC2";
    case MAPPER_MBC3: return "MBC3";
    case MAPPER_MBC5: return "MBC5";
    case MAPPER_ROM: return "ROM only";
    default: return "unsupported mapper";
    }
}

/* Builds the status line.  It is returned as a string rather than printed
 * straight away so the caller can notice when it changes and redraw it
 * even though the picture itself has not moved. */
static void format_status(const GB *gb, char *buf, size_t cap, const char *note,
                          double fps, bool paused, bool muted)
{
    snprintf(buf, cap, " %s [%s] %.1f fps %s%s%s",
             gb->cart.title[0] ? gb->cart.title : "(untitled)",
             mapper_name(&gb->cart), fps, paused ? "[paused] " : "",
             muted ? "[muted] " : "", note);
}

static void draw_status(const char *text, int cols, int row)
{
    int n = (int)strlen(text);

    if (n > cols)
        n = cols;

    printf("\033[%d;1H\033[K%.*s", row, n, text);
}

static void draw_help(void)
{
    printf("\033[2J\033[H");
    printf("Game Boy emulator - controls\n\n");
    printf("  Arrow keys / WASD / HJKL   d-pad\n");
    printf("  Z                          A button\n");
    printf("  X                          B button\n");
    printf("  Enter                      Start\n");
    printf("  Space                      Select\n");
    printf("  P                          pause or resume\n");
    printf("  R                          reset the machine\n");
    printf("  M                          mute or unmute\n");
    printf("  Q or Ctrl-C                quit\n\n");
    printf("The picture is drawn one screen pixel per character cell, and a\n");
    printf("cell carries two pixels down, so a 160 by 73 window shows the\n");
    printf("Game Boy screen exactly.  A smaller window still works, but the\n");
    printf("picture is scaled down to fit.\n\n");
    printf("Press any key to continue...\n");
    fflush(stdout);
}

/*
 * Holds the controls screen up until a key arrives.  Without the wait it
 * would be wiped by the very next frame, long before it could be read.
 * Returns true if the key that dismissed it was the quit key.
 */
static bool help_wait(PlatformKey *inbuf, int cap)
{
    for (;;) {
        int n = platform_read_keys(inbuf, cap);

        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (keymap_command(inbuf[i]) == KEYMAP_CMD_QUIT)
                    return true;
            }
            return false;
        }
        platform_sleep_ms(20);
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int frontend_term_run(GB *gb, const FrontendOptions *opt)
{
    AudioOut audio;
    Glyphs glyphs;
    KeyState keys;
    static uint32_t last_fb[GB_SCREEN_W * GB_SCREEN_H];
    char last_status[512];
    bool last_screen = false;
    bool status_valid = false;
    bool paused = false;
    bool interactive;
    long frames = 0;
    long fps_frames = 0;
    double fps_time;
    double fps = 0.0;
    char note[64] = "";
    double note_until = 0.0;
    PlatformKey inbuf[32];
    bool quit = false;
    double last_draw = -1.0;
    double aspect;

    memset(&last_fb, 0, sizeof(last_fb));
    keymap_init(&keys);
    fps_time = platform_now();

    interactive = platform_begin_interactive();

    aspect = (opt->aspect > 0.0) ? opt->aspect : LAYOUT_DEFAULT_ASPECT;

    glyphs.use_color = !opt->no_color && interactive;
    glyphs.use_unicode = !opt->ascii && interactive && platform_supports_utf8();

    audio_open(&audio, opt->sound);
    audio.muted = opt->mute;
    apu_set_sample_rate(&gb->apu, AUDIO_RATE);

    /*
     * Work out how much room the picture wants before the first frame.
     *
     * A character cell carries one screen pixel across and two down, so a
     * window of 160 by 72 cells shows the Game Boy screen one to one.
     * Anything smaller has to be scaled down, and past about half scale
     * the result stops being recognisable, so when the terminal can be
     * asked for a bigger window it is: the console on Windows, and on a
     * Unix host the terminal's own resize sequence.  Terminals that do not
     * implement it simply ignore the request, and the picture is scaled to
     * whatever room there is, with the status line saying what would help.
     */
    if (interactive && !opt->no_resize && opt->force_cols == 0 &&
        opt->force_rows == 0) {
        int have_cols = 0, have_rows = 0, want_cols, want_rows;
        ScreenLayout l;

        platform_terminal_size(&have_cols, &have_rows);
        l = layout_compute(have_cols, have_rows, aspect);

        /* Only worth asking when the picture is actually being squeezed. */
        if (l.scale > 0.0 && l.scale < 1.0) {
            layout_size_for_scale(1.0, aspect, &want_cols, &want_rows);
            want_rows += 1; /* the status line */

            /*
             * One axis is often already big enough while the other is not.
             * Asking for the ideal size on both would then shrink the axis
             * that was fine, so each is raised to what is already there:
             * grow the cramped direction, leave the other alone.
             */
            if (want_cols < have_cols)
                want_cols = have_cols;
            if (want_rows < have_rows)
                want_rows = have_rows;

            if (platform_resize_console(want_cols, want_rows)) {
                snprintf(note, sizeof(note), "resizing terminal");
                note_until = platform_now() + NOTE_SECONDS;
            }
        }
    }

    while (!quit) {
        double frame_start = platform_now();
        double now = frame_start;
        int cols = 80, rows = 24, screen_rows;
        int n;

        /* The real size, re-read every frame so a resize of the window is
         * picked up straight away.  An explicit --size overrides it, which
         * is what makes the renderer testable from a script. */
        platform_terminal_size(&cols, &rows);
        if (opt->force_cols > 0)
            cols = opt->force_cols;
        if (opt->force_rows > 0)
            rows = opt->force_rows;

        screen_rows = rows - 1; /* the bottom line is the status bar */
        if (screen_rows < 1)
            screen_rows = 1;

        n = platform_read_keys(inbuf, (int)(sizeof(inbuf) / sizeof(inbuf[0])));

        /* ---- input ---- */
        for (int i = 0; i < n; i++) {
            switch (keymap_command(inbuf[i])) {
            case KEYMAP_CMD_QUIT:
                quit = true;
                break;
            case KEYMAP_CMD_PAUSE:
                paused = !paused;
                snprintf(note, sizeof(note), paused ? "paused" : "running");
                note_until = now + NOTE_SECONDS;
                break;
            case KEYMAP_CMD_RESET:
                gb_reset(gb);
                keymap_clear(&keys);
                last_screen = false;
                snprintf(note, sizeof(note), "reset");
                note_until = now + NOTE_SECONDS;
                break;
            case KEYMAP_CMD_MUTE:
                audio.muted = !audio.muted;
                snprintf(note, sizeof(note), audio.muted ? "muted" : "unmuted");
                note_until = now + NOTE_SECONDS;
                break;
            case KEYMAP_CMD_HELP:
            case KEYMAP_CMD_NONE:
                /* A button is held for a moment; help is queued and dealt
                 * with below, once the whole batch of keys is in. */
                keymap_press(&keys, inbuf[i], now);
                break;
            }
        }

        if (keymap_take_help(&keys)) {
            if (interactive) {
                draw_help();
                quit = help_wait(inbuf, (int)(sizeof(inbuf) / sizeof(inbuf[0])));
            }
            last_screen = false;  /* force a full redraw afterwards */
        }

        gb_set_keys(gb, keymap_sample(&keys, now));
        audio_feed(&audio, gb);

        /* ---- emulation ---- */
        if (!paused) {
            gb_run_frame(gb);
            frames++;
        }

        /* ---- present ---- */
        /* Re-read the size: the window may have been resized while the
         * frame was being emulated, and the grid has to be recomputed when
         * it has. */
        {
            int last_cols = cols, last_rows = rows;

            platform_terminal_size(&cols, &rows);
            if (opt->force_cols > 0)
                cols = opt->force_cols;
            if (opt->force_rows > 0)
                rows = opt->force_rows;

            if (cols != last_cols || rows != last_rows)
                last_screen = false; /* the old grid is the wrong shape now */

            screen_rows = rows - 1;
            if (screen_rows < 1)
                screen_rows = 1;
        }

        if (interactive) {
            /*
             * Redrawing a full colour picture costs on the order of a
             * hundred kilobytes of escape sequences.  Doing that on every
             * emulated frame asks a terminal for several megabytes a
             * second, and if it cannot keep up the write blocks and the
             * emulator stops reading the keyboard.  Thirty redraws a
             * second is plenty for a 160x144 screen and halves the load.
             *
             * The status line is tracked separately: pausing or muting
             * leaves the picture untouched, and the change still has to be
             * shown.
             */
            char status[512];
            char hint[96] = "";
            ScreenLayout shown_layout = layout_compute(cols, screen_rows, aspect);
            bool picture_changed =
                !last_screen || memcmp(last_fb, gb->ppu.fb, sizeof(last_fb)) != 0;

            /*
             * Below about half scale several screen pixels share every
             * cell and the picture stops being recognisable.  Saying so, and
             * saying what would fix it, is more use than a blurred screen
             * with no explanation.
             */
            if (shown_layout.scale > 0.0 && shown_layout.scale < 0.55) {
                int want_cols, want_rows;

                layout_size_for_scale(1.0, aspect, &want_cols, &want_rows);
                snprintf(hint, sizeof(hint), "[terminal too small: %dx%d] ",
                         want_cols, want_rows + 1);
            }

            /* Notes are for what just happened; once they have had their
             * moment the line goes back to whatever is true right now,
             * which includes the advice about the window size. */
            if (note[0] && now >= note_until)
                note[0] = '\0';

            format_status(gb, status, sizeof(status), note[0] ? note : hint, fps,
                          paused, audio.muted);

            bool status_changed =
                !status_valid || strcmp(status, last_status) != 0;

            if ((picture_changed || status_changed) &&
                now - last_draw >= 1.0 / REDRAW_HZ) {
                if (picture_changed) {
                    memcpy(last_fb, gb->ppu.fb, sizeof(last_fb));
                    last_screen = true;
                    draw_screen(gb, cols, screen_rows, aspect, &glyphs);
                }
                snprintf(last_status, sizeof(last_status), "%s", status);
                status_valid = true;
                last_draw = now;
                draw_status(status, cols, screen_rows + 1);
                fflush(stdout);
            }
        }

        fps_frames++;
        {
            double t = platform_now();
            if (t - fps_time >= 1.0) {
                fps = (double)fps_frames / (t - fps_time);
                fps_time = t;
                fps_frames = 0;
            }
        }

        if (opt->frames > 0 && frames >= opt->frames)
            break;

        /* Only throttle when a human is watching. */
        if (interactive) {
            double elapsed = platform_now() - frame_start;
            const double target = 1.0 / 70.0;
            if (elapsed < target)
                platform_sleep_ms((int)((target - elapsed) * 1000.0));
        }
    }

    audio_close(&audio);
    platform_end_interactive();

    if (opt->shot)
        frontend_write_bmp(gb, opt->shot);

    return 0;
}
