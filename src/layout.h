/*
 * layout.h -- working out how the Game Boy screen maps onto character
 * cells.
 *
 * The picture is 160 by 144 pixels.  One character cell carries two
 * vertically stacked pixels using the half block glyph, so the terminal
 * offers `cols` by `rows * 2` of them.  This module decides how many cells
 * to use, where to put them, and which source pixels each cell shows.
 *
 * Whether the picture is being enlarged or shrunk decides how a cell is
 * read, and the two cases need different arithmetic: a shrunken cell covers
 * a block of source pixels that has to be averaged, while an enlarged one
 * covers part of a single pixel, which has to be repeated.  Both axes turn
 * the same way at once because the scale is shared, so one flag is enough.
 *
 * There is no terminal or emulator here, which is what lets the arithmetic
 * be unit tested on any machine.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdbool.h>

/* How many screen pixels wide a character cell is assumed to be, as a
 * multiple of its height.  Two is right for most terminal fonts.  A
 * squarer cell needs more rows to show the same picture, and that is what
 * this value is for. */
#define LAYOUT_DEFAULT_ASPECT 2.0

typedef struct {
    int cells_x;        /* cells across the picture            */
    int cells_y;        /* cells down the picture             */
    int origin_x;       /* blank cells to the left, to centre */
    int origin_y;       /* blank cells above, to centre       */
    double scale;       /* screen pixels per Game Boy pixel   */
    bool upscale;       /* the picture is being enlarged, so a
                         * cell repeats one source pixel instead
                         * of averaging several             */
} ScreenLayout;

/* Picks the grid for the given terminal size.  `cell_aspect` is the cell
 * height as a multiple of its width; zero or less uses
 * LAYOUT_DEFAULT_ASPECT. */
ScreenLayout layout_compute(int term_cols, int term_rows, double cell_aspect);

/* The terminal size that would show the picture at `scale` Game Boy pixels
 * per cell, given the cell aspect.  This is the inverse of
 * layout_compute, and it is what the frontend asks the host for when the
 * window is too small: one screen pixel per cell is `scale` 1.0, which
 * needs 160 columns and 72 rows.  The caller adds a row for anything it
 * draws alongside the picture. */
void layout_size_for_scale(double scale, double cell_aspect, int *cols,
                           int *rows);

/* Which source pixel a cell shows when the picture is enlarged.  Both rows
 * are always inside the screen, and for the last row of cells both may be
 * the same pixel, which is what happens when each pixel covers several
 * cells. */
void layout_cell_pixel(const ScreenLayout *l, int cx, int cy, int *x,
                       int *y_top, int *y_bottom);

/* Which block of source pixels a cell covers when the picture is shrunk.
 * The ranges are half open and each holds at least one pixel, and none
 * leaves the screen. */
void layout_cell_area(const ScreenLayout *l, int cx, int cy, int *x0, int *x1,
                      int *y0, int *y1, int *y2);

#endif /* LAYOUT_H */