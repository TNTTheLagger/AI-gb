/*
 * test_layout.c -- fitting the screen to whatever room the terminal has.
 *
 * The renderer has to cope with terminals from about 20 columns to several
 * hundred, and to grow the picture as well as shrink it.  These check the
 * arithmetic behind that, which is pure and so runs the same everywhere.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "layout.h"
#include "gb.h"

/* A spread of sizes, from far too small to far too big. */
static const int sizes[][2] = {
    {   1,   1 }, {   2,   1 }, {   4,   2 }, {  20,  10 },
    {  40,  24 }, {  80,  24 }, { 100,  40 }, { 160,  50 },
    { 240,  70 }, { 400, 120 }, { 1000, 300 }
};

static void t_layout_always_fits_inside_the_terminal(void)
{
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int cols = sizes[i][0], rows = sizes[i][1];
        ScreenLayout l = layout_compute(cols, rows, 0.0);

        if (l.cells_x < 1 || l.cells_y < 1) {
            FAIL("%dx%d produced an empty picture (%dx%d)", cols, rows,
                 l.cells_x, l.cells_y);
            continue;
        }
        CHECK_MSG(l.cells_x <= cols, "%dx%d: cells_x %d overflows", cols, rows,
                  l.cells_x);
        CHECK_MSG(l.cells_y <= rows, "%dx%d: cells_y %d overflows", cols, rows,
                  l.cells_y);

        /* The centring margins must be real and must fit. */
        CHECK(l.origin_x >= 0);
        CHECK(l.origin_y >= 0);
        CHECK(l.origin_x + l.cells_x <= cols);
        CHECK(l.origin_y + l.cells_y <= rows);
    }
}

static void t_layout_never_returns_an_empty_picture(void)
{
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ScreenLayout l = layout_compute(sizes[i][0], sizes[i][1], 0.0);

        /* Even a 1x1 terminal gets one cell rather than nothing. */
        CHECK(l.cells_x >= 1);
        CHECK(l.cells_y >= 1);
    }

    ScreenLayout none = layout_compute(0, 0, 0.0);
    CHECK_EQ(none.cells_x, 0);
    CHECK_EQ(none.cells_y, 0);

    ScreenLayout negative = layout_compute(-5, 10, 0.0);
    CHECK_EQ(negative.cells_x, 0);
}

static void t_layout_uses_the_whole_width_when_it_can(void)
{
    /* Plenty of rows, so the scarce resource is the width and it is used
     * up exactly, leaving no margin. */
    ScreenLayout l = layout_compute(200, 200, LAYOUT_DEFAULT_ASPECT);

    CHECK_EQ(l.cells_x, 200);
    CHECK_EQ(l.origin_x, 0);
    CHECK(l.cells_y < 200);
    CHECK(l.scale > 1.0); /* a screen 160 wide fills 200 cells */
}

static void t_layout_uses_the_whole_height_when_it_can(void)
{
    /* The other way round: a very wide terminal with too few rows, so the
     * rows are the scarce resource. */
    ScreenLayout l = layout_compute(600, 200, LAYOUT_DEFAULT_ASPECT);

    CHECK_EQ(l.cells_y, 200);
    CHECK_EQ(l.origin_y, 0);
    CHECK(l.cells_x < 600);
    CHECK(l.scale > 1.0); /* the picture grows to use the room */
}

static void t_layout_native_size_with_the_usual_cells(void)
{
    /* 160 columns of cells shows the screen one pixel per column, and the
     * 72 rows it needs come out of the 144 pixel rows a cell pair holds. */
    ScreenLayout l = layout_compute(160, 72, LAYOUT_DEFAULT_ASPECT);

    CHECK_EQ(l.cells_x, 160);
    CHECK_EQ(l.cells_y, 72);
    CHECK_EQ(l.origin_x, 0);
    CHECK_EQ(l.origin_y, 0);
    CHECK(l.scale > 0.99 && l.scale < 1.01);
}

static void t_layout_cell_pixels_are_always_on_the_screen(void)
{
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int cols = sizes[i][0], rows = sizes[i][1];
        ScreenLayout l = layout_compute(cols, rows, 0.0);

        if (l.cells_x < 1 || l.cells_y < 1)
            continue;

        for (int cy = 0; cy < l.cells_y; cy++) {
            for (int cx = 0; cx < l.cells_x; cx++) {
                int x, yt, yb;

                layout_cell_pixel(&l, cx, cy, &x, &yt, &yb);

                CHECK_MSG(x >= 0 && x < GB_SCREEN_W,
                          "%dx%d cell (%d,%d) column %d", cols, rows, cx, cy, x);
                CHECK_MSG(yt >= 0 && yt < GB_SCREEN_H,
                          "%dx%d cell (%d,%d) top row %d", cols, rows, cx, cy,
                          yt);
                CHECK_MSG(yb >= 0 && yb < GB_SCREEN_H,
                          "%dx%d cell (%d,%d) bottom row %d", cols, rows, cx,
                          cy, yb);
                CHECK_MSG(yb >= yt, "%dx%d cell (%d,%d) rows go backwards",
                          cols, rows, cx, cy);
            }
        }
    }
}

static void t_layout_cell_areas_are_always_on_the_screen(void)
{
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int cols = sizes[i][0], rows = sizes[i][1];
        ScreenLayout l = layout_compute(cols, rows, 0.0);

        if (l.cells_x < 1 || l.cells_y < 1 || l.upscale)
            continue; /* an enlarged cell has no block to cover */

        for (int cy = 0; cy < l.cells_y; cy++) {
            for (int cx = 0; cx < l.cells_x; cx++) {
                int x0, x1, y0, y1, y2;

                layout_cell_area(&l, cx, cy, &x0, &x1, &y0, &y1, &y2);

                CHECK_MSG(x0 >= 0 && x1 > x0 && x1 <= GB_SCREEN_W,
                          "%dx%d cell (%d,%d) columns %d..%d", cols, rows, cx,
                          cy, x0, x1);
                CHECK_MSG(y0 >= 0 && y1 > y0 && y1 <= GB_SCREEN_H,
                          "%dx%d cell (%d,%d) rows %d..%d", cols, rows, cx, cy,
                          y0, y1);
                CHECK_MSG(y2 > y1 && y2 <= GB_SCREEN_H,
                          "%dx%d cell (%d,%d) rows %d..%d", cols, rows, cx, cy,
                          y1, y2);
            }
        }
    }
}

/* Shrinking has to be a clean partition: every source pixel belongs to
 * exactly one cell, or seams and doubled rows appear in the picture. */
static void t_layout_shrinking_covers_every_pixel_exactly_once(void)
{
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int cols = sizes[i][0], rows = sizes[i][1];
        ScreenLayout l = layout_compute(cols, rows, 0.0);

        if (l.cells_x < 1 || l.cells_y < 1 || l.upscale)
            continue;

        /* Columns first. */
        {
            int seen[GB_SCREEN_W];
            memset(seen, 0, sizeof(seen));
            for (int cx = 0; cx < l.cells_x; cx++) {
                int x0, x1, y0, y1, y2;
                layout_cell_area(&l, cx, 0, &x0, &x1, &y0, &y1, &y2);
                for (int x = x0; x < x1; x++)
                    seen[x]++;
            }
            for (int x = 0; x < GB_SCREEN_W; x++)
                CHECK_MSG(seen[x] == 1,
                          "%dx%d: column %d is in %d cells, not one", cols,
                          rows, x, seen[x]);
        }

        /* Then rows, across the whole height. */
        {
            int seen[GB_SCREEN_H];
            memset(seen, 0, sizeof(seen));
            for (int cy = 0; cy < l.cells_y; cy++) {
                int x0, x1, y0, y1, y2;
                layout_cell_area(&l, 0, cy, &x0, &x1, &y0, &y1, &y2);
                for (int y = y0; y < y1; y++)
                    seen[y]++;
                for (int y = y1; y < y2; y++)
                    seen[y]++;
            }
            for (int y = 0; y < GB_SCREEN_H; y++)
                CHECK_MSG(seen[y] == 1,
                          "%dx%d: row %d is in %d cells, not one", cols, rows,
                          y, seen[y]);
        }
    }
}

static void t_layout_grows_the_picture_on_a_big_terminal(void)
{
    /* Wider than the screen in cells, so pixels are repeated rather than
     * averaged. */
    ScreenLayout l = layout_compute(320, 300, LAYOUT_DEFAULT_ASPECT);

    CHECK(l.cells_x > GB_SCREEN_W);
    CHECK(l.cells_y * 2 > GB_SCREEN_H);
    CHECK(l.upscale);
    CHECK(l.scale > 1.0);
}

static void t_layout_shrinks_the_picture_on_a_small_terminal(void)
{
    ScreenLayout l = layout_compute(60, 20, LAYOUT_DEFAULT_ASPECT);

    CHECK(l.cells_x < GB_SCREEN_W);
    CHECK(l.cells_y * 2 < GB_SCREEN_H);
    CHECK(!l.upscale);
    CHECK(l.scale < 1.0);
}

static void t_layout_exactly_one_to_one_needs_no_resampling(void)
{
    /* One cell per pixel across, one per two down: nothing is gained and
     * nothing is lost, so the pixels should be used as they are. */
    ScreenLayout l = layout_compute(160, 72, LAYOUT_DEFAULT_ASPECT);

    CHECK_EQ(l.cells_x, GB_SCREEN_W);
    CHECK(l.upscale);
}

/*
 * The cell aspect decides how tall the picture looks.  A squarer cell
 * carries less height per row, so the same screen needs more of them.
 */
static void t_layout_aspect_changes_how_many_rows_are_needed(void)
{
    ScreenLayout tall = layout_compute(200, 300, 3.0);   /* tall cells  */
    ScreenLayout flat = layout_compute(200, 300, 1.0);   /* square cells */

    /* Both are wide enough that the width is what binds. */
    CHECK_EQ(tall.cells_x, 200);
    CHECK_EQ(flat.cells_x, 200);

    /* A square cell shows less picture per row, so more rows are used. */
    CHECK_MSG(flat.cells_y > tall.cells_y,
              "square cells used %d rows, tall cells used %d", flat.cells_y,
              tall.cells_y);
}

static void t_layout_aspect_falls_back_when_nonsense(void)
{
    ScreenLayout zero = layout_compute(100, 40, 0.0);
    ScreenLayout negative = layout_compute(100, 40, -3.0);
    ScreenLayout defaulted = layout_compute(100, 40, LAYOUT_DEFAULT_ASPECT);

    CHECK_EQ(zero.cells_x, defaulted.cells_x);
    CHECK_EQ(zero.cells_y, defaulted.cells_y);
    CHECK_EQ(negative.cells_x, defaulted.cells_x);
    CHECK_EQ(negative.cells_y, defaulted.cells_y);
}

static void t_layout_stays_centred_as_the_terminal_grows(void)
{
    /* Give the picture more and more room; it should stay in the middle. */
    for (int w = 170; w <= 400; w += 47) {
        ScreenLayout l = layout_compute(w, 200, LAYOUT_DEFAULT_ASPECT);
        int left = l.origin_x, right = w - l.origin_x - l.cells_x;

        CHECK_MSG(left == right || left == right + 1,
                  "width %d: %d cells on the left, %d on the right", w, left,
                  right);
    }
}

/* At a scale that divides evenly every cell should cover the same number
 * of pixels, which is what stops a solid block developing faint stripes. */
static void t_layout_cells_are_uniform_at_a_whole_scale(void)
{
    /* Half scale: two screen pixels across, four down, in each cell. */
    ScreenLayout l = layout_compute(80, 36, LAYOUT_DEFAULT_ASPECT);

    CHECK_EQ(l.cells_x, 80);
    CHECK_EQ(l.cells_y, 36);
    CHECK(!l.upscale);

    for (int cy = 0; cy < 3; cy++) {
        int x0, x1, y0, y1, y2;

        layout_cell_area(&l, 0, cy, &x0, &x1, &y0, &y1, &y2);
        CHECK_EQ(x0, 0);
        CHECK_EQ(x1 - x0, 2);
        CHECK_EQ(y1 - y0, 2);
        CHECK_EQ(y2 - y1, 2);
    }
}

/* The glyph carries two stacked pixels, so the halves of a cell should get
 * the same number of rows.  One row of difference is rounding; more is a
 * bug. */
static void t_layout_halves_are_the_same_size_when_shrinking(void)
{
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ScreenLayout l = layout_compute(sizes[i][0], sizes[i][1], 0.0);

        if (l.upscale)
            continue;

        for (int cy = 0; cy < l.cells_y; cy++) {
            int x0, x1, y0, y1, y2;
            int top, bottom;

            layout_cell_area(&l, 0, cy, &x0, &x1, &y0, &y1, &y2);
            top = y1 - y0;
            bottom = y2 - y1;
            CHECK_MSG(top > 0 && bottom > 0 &&
                          (top == bottom ||
                           (top > bottom ? top - bottom : bottom - top) == 1),
                      "%dx%d cell row %d: halves are %d and %d rows",
                      sizes[i][0], sizes[i][1], cy, top, bottom);
        }
    }
}

/*
 * layout_size_for_scale is the inverse of layout_compute: it answers "how
 * big would the window have to be to show the picture this clearly".  The
 * frontend uses it to ask the terminal for a window big enough, so the two
 * have to agree exactly or the request will be the wrong size.
 */
static void t_layout_size_for_scale_is_the_inverse_of_compute(void)
{
    const double scales[] = { 0.5, 0.75, 1.0, 1.5, 2.0, 3.0 };
    const double aspects[] = { 1.0, 1.6, LAYOUT_DEFAULT_ASPECT, 2.5 };

    for (size_t a = 0; a < sizeof(aspects) / sizeof(aspects[0]); a++) {
        for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
            int cols = 0, rows = 0;
            ScreenLayout l;

            layout_size_for_scale(scales[i], aspects[a], &cols, &rows);
            l = layout_compute(cols, rows, aspects[a]);

            /*
             * Whole cells mean the round trip cannot be exact: asking for
             * 43 rows of 2.5 aspect cells lands a fraction under the scale
             * that was asked for, and one less column.  What matters is
             * that the request is close, and that it never asks for less
             * than it claims to be delivering.
             */
            CHECK_MSG(abs(l.cells_x - cols) <= 1 && abs(l.cells_y - rows) <= 1,
                      "asked for %dx%d at scale %g aspect %g, got %dx%d",
                      cols, rows, scales[i], aspects[a], l.cells_x, l.cells_y);
            CHECK_MSG(l.scale >= scales[i] - 0.05,
                      "asked for %dx%d at scale %g aspect %g but only got %g",
                      cols, rows, scales[i], aspects[a], l.scale);
        }
    }
}

/* The whole point of asking is to reach one pixel per cell, which for the
 * Game Boy screen is 160 columns and 72 rows. */
static void t_layout_one_to_one_size_is_the_screen_size(void)
{
    int cols = 0, rows = 0;

    layout_size_for_scale(1.0, LAYOUT_DEFAULT_ASPECT, &cols, &rows);
    CHECK_EQ(cols, GB_SCREEN_W);
    CHECK_EQ(rows, GB_SCREEN_H / 2);
}

/* A cell that is twice as tall as it is wide packs two picture rows into
 * itself, so the same picture needs half as many of them. */
static void t_layout_size_for_scale_follows_the_aspect(void)
{
    int half_cols = 0, half_rows = 0, square_cols = 0, square_rows = 0;

    layout_size_for_scale(1.0, 2.0, &half_cols, &half_rows);
    layout_size_for_scale(1.0, 1.0, &square_cols, &square_rows);

    CHECK_EQ(half_cols, square_cols);
    CHECK_MSG(half_rows * 2 == square_rows,
              "half as tall cells should halve the rows: %d against %d",
              half_rows, square_rows);
}

/* Nonsense in, a usable answer out: the caller must never be handed a zero
 * that it would turn into a request for a window of no size. */
static void t_layout_size_for_scale_survives_nonsense(void)
{
    int cols = 99, rows = 99;

    layout_size_for_scale(1.0, -1.0, &cols, &rows);
    CHECK_EQ(cols, GB_SCREEN_W); /* the aspect fell back to the default */

    layout_size_for_scale(0.0, LAYOUT_DEFAULT_ASPECT, &cols, &rows);
    CHECK_EQ(cols, 0);
    CHECK_EQ(rows, 0);

    layout_size_for_scale(-2.0, LAYOUT_DEFAULT_ASPECT, &cols, &rows);
    CHECK_EQ(cols, 0);
    CHECK_EQ(rows, 0);

    /* A scale too small to need even one cell still asks for one, because
     * a zero sized window is not something a terminal can be given. */
    layout_size_for_scale(0.0001, LAYOUT_DEFAULT_ASPECT, &cols, &rows);
    CHECK_EQ(cols, 1);
    CHECK_EQ(rows, 1);
}

/* Whatever this returns has to be a size a terminal could actually have, or
 * the request would be nonsense on the way out. */
static void t_layout_size_for_scale_is_always_sane(void)
{
    static const double scales[] = { 0.01, 0.5, 1.0, 2.0, 10.0 };
    int cols = -1, rows = -1;

    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        layout_size_for_scale(scales[i], LAYOUT_DEFAULT_ASPECT, &cols, &rows);
        CHECK(cols > 0 && cols <= 10000);
        CHECK(rows > 0 && rows <= 10000);
    }
}

const TestCase layout_tests[] = {
    { "layout", "always fits inside the terminal", t_layout_always_fits_inside_the_terminal },
    { "layout", "never returns an empty picture",    t_layout_never_returns_an_empty_picture },
    { "layout", "uses the whole width it can",      t_layout_uses_the_whole_width_when_it_can },
    { "layout", "uses the whole height it can",     t_layout_uses_the_whole_height_when_it_can },
    { "layout", "one to one at the native size",     t_layout_native_size_with_the_usual_cells },
    { "layout", "cell pixels stay on the screen",    t_layout_cell_pixels_are_always_on_the_screen },
    { "layout", "cell areas stay on the screen",     t_layout_cell_areas_are_always_on_the_screen },
    { "layout", "shrinking covers each pixel once",   t_layout_shrinking_covers_every_pixel_exactly_once },
    { "layout", "grows the picture when there is room", t_layout_grows_the_picture_on_a_big_terminal },
    { "layout", "shrinks the picture when tight",    t_layout_shrinks_the_picture_on_a_small_terminal },
    { "layout", "one to one needs no resampling",    t_layout_exactly_one_to_one_needs_no_resampling },
    { "layout", "aspect changes the rows needed",    t_layout_aspect_changes_how_many_rows_are_needed },
    { "layout", "nonsense aspect falls back",        t_layout_aspect_falls_back_when_nonsense },
    { "layout", "stays centred as the terminal grows", t_layout_stays_centred_as_the_terminal_grows },
    { "layout", "cells are uniform at a whole scale", t_layout_cells_are_uniform_at_a_whole_scale },
    { "layout", "the halves match when shrinking",   t_layout_halves_are_the_same_size_when_shrinking },
    { "layout", "size for scale inverts compute",  t_layout_size_for_scale_is_the_inverse_of_compute },
    { "layout", "one to one is the screen size",  t_layout_one_to_one_size_is_the_screen_size },
    { "layout", "size for scale follows aspect",  t_layout_size_for_scale_follows_the_aspect },
    { "layout", "size for scale survives nonsense", t_layout_size_for_scale_survives_nonsense },
    { "layout", "size for scale is always sane",  t_layout_size_for_scale_is_always_sane },

    TEST_SUITE_END
};