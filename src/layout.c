/*
 * layout.c -- see layout.h.
 *
 * The scaling is deliberately different in the two directions.  Growing a
 * pixel to fill several cells should repeat it, otherwise a 2x enlargement
 * turns every group of pixels into a flat average and the picture turns to
 * mush.  Shrinking has to average, or thin lines flicker as the grid slides
 * around.
 */
#include "layout.h"

#include "gb.h"

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

ScreenLayout layout_compute(int term_cols, int term_rows, double cell_aspect)
{
    ScreenLayout l;
    double sx, sy, scale;

    l.cells_x = 0;
    l.cells_y = 0;
    l.origin_x = 0;
    l.origin_y = 0;
    l.scale = 0.0;
    l.upscale = false;

    if (!(cell_aspect > 0.0))
        cell_aspect = LAYOUT_DEFAULT_ASPECT;
    if (term_cols < 1 || term_rows < 1)
        return l;

    /*
     * How big the picture may get.
     *
     * Across, a cell is one screen pixel wide, so `cols` cells offer
     * `cols / 160` times the width of the screen.
     *
     * Down, a cell holds two stacked pixels but is `cell_aspect` times as
     * tall as it is wide, so `rows` cells hold `rows * cell_aspect` widths
     * of picture against a screen that is `144` tall.  Whichever direction
     * runs out of room first decides the size.
     */
    sx = (double)term_cols / (double)GB_SCREEN_W;
    sy = ((double)term_rows * cell_aspect) / (double)GB_SCREEN_H;

    scale = (sx < sy) ? sx : sy;
    if (!(scale > 0.0))
        return l;

    l.scale = scale;
    l.cells_x = (int)((double)GB_SCREEN_W * scale + 0.5);
    l.cells_y = (int)((double)GB_SCREEN_H * scale / cell_aspect + 0.5);

    l.cells_x = clampi(l.cells_x, 1, term_cols);
    l.cells_y = clampi(l.cells_y, 1, term_rows);

    l.origin_x = (term_cols - l.cells_x) / 2;
    l.origin_y = (term_rows - l.cells_y) / 2;

    /*
     * Enlarged or not?  Both axes share one scale, so they always agree,
     * and asking about either is enough.  Checking the vertical direction
     * is the honest test: a cell holds two pixel rows, so the picture has
     * room for more rows than it has when shrinking.
     */
    l.upscale = (l.cells_y * 2) >= GB_SCREEN_H;

    return l;
}

void layout_size_for_scale(double scale, double cell_aspect, int *cols,
                           int *rows)
{
    if (!(cell_aspect > 0.0))
        cell_aspect = LAYOUT_DEFAULT_ASPECT;
    if (!(scale > 0.0)) {
        *cols = 0;
        *rows = 0;
        return;
    }

    /*
     * Across, a cell is one screen pixel wide.  Down, a cell carries two
     * stacked pixels but stands `cell_aspect` widths tall, so the picture
     * needs half as many rows again.
     */
    *cols = (int)(GB_SCREEN_W * scale + 0.5);
    *rows = (int)(GB_SCREEN_H * scale / cell_aspect + 0.5);

    if (*cols < 1) *cols = 1;
    if (*rows < 1) *rows = 1;
}

void layout_cell_pixel(const ScreenLayout *l, int cx, int cy, int *x,
                       int *y_top, int *y_bottom)
{
    int rows = l->cells_y * 2;

    /*
     * Floor division into a grid that has more cells than pixels: every
     * source pixel is claimed by at least one cell and the last one cannot
     * fall off the end, because cx and cy stop short of their maxima.
     */
    *x = cx * GB_SCREEN_W / l->cells_x;
    *y_top = (cy * 2) * GB_SCREEN_H / rows;
    *y_bottom = (cy * 2 + 1) * GB_SCREEN_H / rows;
}

void layout_cell_area(const ScreenLayout *l, int cx, int cy, int *x0, int *x1,
                      int *y0, int *y1, int *y2)
{
    int rows = l->cells_y * 2;

    /*
     * A cell now covers fewer source pixels than it has cells, so the
     * boundaries are made to land exactly on the screen edges: the last
     * cell always ends at the last pixel.  With at least one source pixel
     * per cell, consecutive boundaries are a whole pixel apart and the
     * ranges can never come out empty.
     */
    *x0 = cx * GB_SCREEN_W / l->cells_x;
    *x1 = (cx + 1) * GB_SCREEN_W / l->cells_x;

    *y0 = (cy * 2) * GB_SCREEN_H / rows;
    *y1 = (cy * 2 + 1) * GB_SCREEN_H / rows;
    *y2 = (cy * 2 + 2) * GB_SCREEN_H / rows;
}