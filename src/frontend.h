/*
 * frontend.h -- the platform layer: everything that touches the terminal
 * lives behind this interface so the emulator core stays pure C99.
 */
#ifndef FRONTEND_H
#define FRONTEND_H

#include "gb.h"

typedef struct {
    long frames;         /* number of frames to run, 0 means "forever" */
    const char *shot;    /* path for a BMP screenshot, or NULL         */
    const char *sound;   /* "none", "auto" or a player command        */
    bool ascii;          /* force plain ASCII glyphs                   */
    bool no_color;       /* force the monochrome glyph set             */
    bool mute;           /* start with audio muted                     */

    /* Display.  A console application cannot ask for a particular size
     * and the window is not always big enough, so the picture is scaled to
     * fit whatever is actually there. */
    double aspect;       /* character cell height / width, 0 for the
                          * built in guess                            */
    int force_cols;      /* pretend the terminal is this wide, 0 to use
                          * the real size                             */
    int force_rows;      /* and this tall                              */
    bool no_resize;      /* never try to grow the console window       */
} FrontendOptions;

/* Runs the emulator until the user quits or `frames` have elapsed.
 * Returns the process exit code. */
int frontend_term_run(GB *gb, const FrontendOptions *opt);

/* Writes the current framebuffer out as a 24 bit uncompressed BMP. */
bool frontend_write_bmp(const GB *gb, const char *path);

#endif /* FRONTEND_H */