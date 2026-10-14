/*
 * test_ppu.c -- the LCD controller: timing, modes, background, window and
 * sprites.
 */
#include "test.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Loads a 8x8 tile where every pixel is the given colour index. */
static void load_solid_tile(GB *gb, int tile, uint8_t color)
{
    uint16_t base = (uint16_t)(0x8000 + tile * 16);
    uint8_t lo = (color & 1) ? 0xFF : 0x00;
    uint8_t hi = (color & 2) ? 0xFF : 0x00;

    for (int row = 0; row < 8; row++) {
        gb_write(gb, (uint16_t)(base + row * 2), lo);
        gb_write(gb, (uint16_t)(base + row * 2 + 1), hi);
    }
}

/* Fills a 32x32 tile map so every entry points at `tile`. */
static void load_map(GB *gb, uint16_t base, int tile)
{
    for (int i = 0; i < 32 * 32; i++)
        gb_write(gb, (uint16_t)(base + i), (uint8_t)tile);
}

/* Turns the LCD on with a straightforward configuration and renders line 0. */
/* Rewinds the LCD to the top of the frame and renders line 0, keeping
 * whatever LCDC and BGP the test configured.  The reset palette $FC is the
 * identity mapping, so tests that do not care about palettes can ignore it. */
static void render_line0(Fixture *f)
{
    uint8_t lcdc = f->gb.ppu.lcdc;

    gb_write(&f->gb, 0xFF40, 0x00); /* off, which rewinds to line 0   */
    gb_write(&f->gb, 0xFF40, lcdc); /* back on with the test's setup   */
    ppu_tick(&f->gb, 80 + 1);       /* mode 3 renders the line         */
}

static uint32_t pixel(Fixture *f, int x, int y)
{
    return f->gb.ppu.fb[(size_t)y * GB_SCREEN_W + x];
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

static void t_ppu_starts_in_oam_scan(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    CHECK_EQ(f.gb.ppu.ly, 0);
    CHECK_EQ(f.gb.ppu.mode, 2);
    CHECK_EQ(f.gb.ppu.dot, 0);
    fixture_destroy(&f);
}

static void t_ppu_mode_two_lasts_eighty_dots(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    ppu_tick(&f.gb, 79);
    CHECK_EQ(f.gb.ppu.mode, 2);
    ppu_tick(&f.gb, 1);
    CHECK_EQ(f.gb.ppu.mode, 3);
    fixture_destroy(&f);
}

static void t_ppu_mode_three_lasts_172_dots(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    ppu_tick(&f.gb, 80);
    CHECK_EQ(f.gb.ppu.mode, 3);
    CHECK_EQ(f.gb.ppu.dot, 80);

    /* Mode 3 runs from dot 80 to dot 251, so 171 more dots stay in it. */
    ppu_tick(&f.gb, 171);
    CHECK_EQ(f.gb.ppu.dot, 251);
    CHECK_EQ(f.gb.ppu.mode, 3);

    ppu_tick(&f.gb, 1);
    CHECK_EQ(f.gb.ppu.mode, 0);
    CHECK_EQ(f.gb.ppu.dot, 252);
    fixture_destroy(&f);
}

static void t_ppu_hblank_runs_to_the_end_of_the_line(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    ppu_tick(&f.gb, 252);
    CHECK_EQ(f.gb.ppu.mode, 0);
    ppu_tick(&f.gb, 456 - 252 - 1);
    CHECK_EQ(f.gb.ppu.mode, 0);
    CHECK_EQ(f.gb.ppu.ly, 0);
    ppu_tick(&f.gb, 1);
    CHECK_EQ(f.gb.ppu.ly, 1);
    fixture_destroy(&f);
}

static void t_ppu_a_line_is_456_dots(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    ppu_tick(&f.gb, 456);
    CHECK_EQ(f.gb.ppu.ly, 1);
    ppu_tick(&f.gb, 456);
    CHECK_EQ(f.gb.ppu.ly, 2);
    fixture_destroy(&f);
}

static void t_ppu_counts_through_to_ly_143(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    for (int i = 0; i < 143; i++)
        ppu_tick(&f.gb, 456);

    /* 143 whole lines done, so we are standing at the start of line 143. */
    CHECK_EQ(f.gb.ppu.ly, 143);
    CHECK_EQ(f.gb.ppu.mode, 2);
    CHECK_EQ(f.gb.ppu.dot, 0);
    fixture_destroy(&f);
}

static void t_ppu_vblank_starts_at_line_144(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    for (int i = 0; i < 144; i++)
        ppu_tick(&f.gb, 456);

    CHECK_EQ(f.gb.ppu.ly, 144);
    CHECK_EQ(f.gb.ppu.mode, 1);
    CHECK_EQ(f.gb.iflag & GB_INT_VBLANK, GB_INT_VBLANK);
    /* The frame itself is not over until VBlank ends. */
    CHECK_EQ(f.gb.ppu.frame_done, false);
    fixture_destroy(&f);
}

static void t_ppu_wraps_back_to_line_zero(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    for (int i = 0; i < 154; i++)
        ppu_tick(&f.gb, 456);

    CHECK_EQ(f.gb.ppu.ly, 0);
    CHECK_EQ(f.gb.ppu.mode, 2);
    CHECK_EQ(f.gb.ppu.frame, 1);
    CHECK_EQ(f.gb.ppu.frame_done, true);
    fixture_destroy(&f);
}

static void t_ppu_does_not_advance_when_the_lcd_is_off(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF40, 0x00);
    ppu_tick(&f.gb, 100000);
    CHECK_EQ(f.gb.ppu.ly, 0);
    CHECK_EQ(f.gb.ppu.dot, 0);
    fixture_destroy(&f);
}

static void t_ppu_reenabling_restarts_the_frame(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    for (int i = 0; i < 10; i++)
        ppu_tick(&f.gb, 456);
    CHECK_EQ(f.gb.ppu.ly, 10);

    gb_write(&f.gb, 0xFF40, 0x00);
    CHECK_EQ(f.gb.ppu.ly, 0);
    CHECK_EQ(f.gb.ppu.frame_done, true);

    gb_write(&f.gb, 0xFF40, 0x91);
    CHECK_EQ(f.gb.ppu.ly, 0);
    CHECK_EQ(f.gb.ppu.mode, 2);
    CHECK_EQ(f.gb.ppu.dot, 0);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* STAT and LYC                                                         */
/* ------------------------------------------------------------------ */

static void t_ppu_lyc_stat_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF45, 50);  /* LYC = 50            */
    gb_write(&f.gb, 0xFF41, 0x40); /* enable LYC compare  */
    f.gb.iflag = 0;

    ppu_tick(&f.gb, 456 * 49);
    CHECK_EQ(f.gb.iflag & GB_INT_LCDSTAT, 0);

    ppu_tick(&f.gb, 456);
    CHECK_EQ(f.gb.ppu.ly, 50);
    CHECK_EQ(f.gb.iflag & GB_INT_LCDSTAT, GB_INT_LCDSTAT);
    fixture_destroy(&f);
}

static void t_ppu_stat_only_fires_on_the_rising_edge(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int interrupts = 0;

    /* LYC = 0, so the condition is true for the whole of line 0 and false
     * everywhere else.  It must raise exactly one interrupt per frame. */
    gb_write(&f.gb, 0xFF45, 0);
    gb_write(&f.gb, 0xFF41, 0x40);
    f.gb.iflag = 0;

    for (int i = 0; i < GB_CYCLES_PER_FRAME * 2; i++) {
        f.gb.iflag = 0;
        ppu_tick(&f.gb, 1);
        if (f.gb.iflag & GB_INT_LCDSTAT)
            interrupts++;
    }

    CHECK_EQ(interrupts, 2); /* once in each of the two frames */
    fixture_destroy(&f);
}

static void t_ppu_hblank_stat_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF41, 0x08); /* enable mode 0 STAT */
    f.gb.iflag = 0;

    ppu_tick(&f.gb, 252);
    CHECK_EQ(f.gb.ppu.mode, 0);
    CHECK_EQ(f.gb.iflag & GB_INT_LCDSTAT, GB_INT_LCDSTAT);
    fixture_destroy(&f);
}

static void t_ppu_vblank_stat_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF41, 0x10); /* enable mode 1 STAT */
    f.gb.iflag = 0;

    ppu_tick(&f.gb, 456 * 144);
    CHECK_EQ(f.gb.ppu.mode, 1);
    CHECK_EQ(f.gb.iflag & GB_INT_LCDSTAT, GB_INT_LCDSTAT);
    fixture_destroy(&f);
}

static void t_ppu_oam_stat_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF41, 0x20); /* enable mode 2 STAT */
    f.gb.iflag = 0;

    /* We are already in mode 2, so the line has to drop and rise again:
     * finishing this line enters mode 2 on the next one. */
    ppu_tick(&f.gb, 456 * 3);
    CHECK_EQ(f.gb.iflag & GB_INT_LCDSTAT, GB_INT_LCDSTAT);
    fixture_destroy(&f);
}

static void t_ppu_stat_bit_seven_always_reads_high(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF41, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x80, 0x80);
    fixture_destroy(&f);
}

static void t_ppu_lyc_can_be_changed_mid_frame(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF41, 0x40);
    f.gb.iflag = 0;

    ppu_tick(&f.gb, 456 * 5);
    gb_write(&f.gb, 0xFF45, 10); /* match the line we are about to enter */
    ppu_tick(&f.gb, 456 * 5);    /* arrive at line 10 */

    CHECK_EQ(f.gb.ppu.ly, 10);
    CHECK_EQ(f.gb.iflag & GB_INT_LCDSTAT, GB_INT_LCDSTAT);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Background rendering                                                 */
/* ------------------------------------------------------------------ */

static void t_ppu_background_draws_a_solid_tile(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 3);         /* tile 1 is fully shade 3 */
    load_map(&f.gb, 0x9800, 1);
    render_line0(&f);

    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[3]);
    CHECK_EQ(pixel(&f, 7, 0), f.gb.ppu.palette[3]);
    CHECK_EQ(pixel(&f, 159, 0), f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_ppu_tile_index_zero_uses_the_same_data(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* LCDC bit 4 clear points tile data at $8800 instead of $8000. */
    load_solid_tile(&f.gb, 1, 3);
    load_map(&f.gb, 0x9800, 1);

    gb_write(&f.gb, 0xFF40, 0x81); /* LCD on, BG on, $8800 tile data */
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    /* Index 1 now refers to $8810, which we never filled. */
    CHECK_NE(pixel(&f, 0, 0), f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_ppu_scx_scrolls_the_line(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* A checkerboard of two tiles so any shift is visible. */
    load_solid_tile(&f.gb, 1, 3);
    load_solid_tile(&f.gb, 2, 0);
    for (int i = 0; i < 32 * 32; i++)
        gb_write(&f.gb, (uint16_t)(0x9800 + i), (uint8_t)((i % 2) ? 2 : 1));

    render_line0(&f);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[3]);

    gb_write(&f.gb, 0xFF43, 8); /* SCX = 8 */
    render_line0(&f);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[0]); /* now tile 2 */
    CHECK_EQ(pixel(&f, 8, 0), f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_ppu_scy_scrolls_the_map(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 3);
    for (int i = 0; i < 32 * 32; i++)
        gb_write(&f.gb, (uint16_t)(0x9800 + i), (uint8_t)((i / 32) ? 2 : 1));

    render_line0(&f);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[3]);

    gb_write(&f.gb, 0xFF42, 8); /* SCY = 8, so line 0 shows map row 1 */
    render_line0(&f);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[0]);
    fixture_destroy(&f);
}

static void t_ppu_second_map_can_be_selected(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 3);
    load_map(&f.gb, 0x9800, 2); /* unused */
    load_map(&f.gb, 0x9C00, 1); /* the real content */

    render_line0(&f);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[0]); /* $9800 is empty */

    gb_write(&f.gb, 0xFF40, 0x99); /* LCDC bit 3: use $9C00 */
    render_line0(&f);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_ppu_bgp_remaps_the_shades(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 2); /* colour index 2 */
    load_map(&f.gb, 0x9800, 1);

    gb_write(&f.gb, 0xFF40, 0x91);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[2]);

    /* Send index 2 to shade 0. */
    gb_write(&f.gb, 0xFF47, 0xC4); /* index 2 now maps to shade 0 */
    render_line0(&f);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[0]);
    fixture_destroy(&f);
}

static void t_ppu_blank_when_the_background_is_disabled(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 3);
    load_map(&f.gb, 0x9800, 1);

    gb_write(&f.gb, 0xFF40, 0x90); /* LCD on but LCDC bit 0 clear */
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[0]);
    fixture_destroy(&f);
}

static void t_ppu_pixel_pattern_within_a_tile(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* Tile 1: leftmost pixel 1, everything else 0. */
    gb_write(&f.gb, 0x8010, 0x80); /* low plane  */
    gb_write(&f.gb, 0x8011, 0x00); /* high plane */
    load_map(&f.gb, 0x9800, 1);

    render_line0(&f);

    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[1]);
    CHECK_EQ(pixel(&f, 1, 0), f.gb.ppu.palette[0]);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static void t_ppu_window_covers_the_right_side(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 0); /* background: white */
    load_solid_tile(&f.gb, 2, 3); /* window: black   */
    load_map(&f.gb, 0x9800, 1);
    load_map(&f.gb, 0x9C00, 2);

    gb_write(&f.gb, 0xFF40, 0xF1); /* LCD on, BG on, window on at $9C00 */
    gb_write(&f.gb, 0xFF47, 0xE4);
    gb_write(&f.gb, 0xFF4B, 0x57); /* WX = 87 -> window starts at x=80 */
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 79, 0), f.gb.ppu.palette[0]);
    CHECK_EQ(pixel(&f, 80, 0), f.gb.ppu.palette[3]);
    CHECK_EQ(pixel(&f, 159, 0), f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_ppu_window_is_hidden_above_wy(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 0);
    load_solid_tile(&f.gb, 2, 3);
    load_map(&f.gb, 0x9800, 1);
    load_map(&f.gb, 0x9C00, 2);

    gb_write(&f.gb, 0xFF40, 0xF1);
    gb_write(&f.gb, 0xFF47, 0xE4);
    gb_write(&f.gb, 0xFF4B, 0x07); /* WX = 7 -> starts at x = 0 */
    gb_write(&f.gb, 0xFF4A, 0x64); /* WY = 100, we are on line 0 */
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[0]); /* still background */
    fixture_destroy(&f);
}

static void t_ppu_window_appears_from_wy(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 0);
    load_solid_tile(&f.gb, 2, 3);
    load_map(&f.gb, 0x9800, 1);
    load_map(&f.gb, 0x9C00, 2);

    gb_write(&f.gb, 0xFF40, 0xF1);
    gb_write(&f.gb, 0xFF47, 0xE4);
    gb_write(&f.gb, 0xFF4B, 0x07);
    gb_write(&f.gb, 0xFF4A, 0x00); /* WY = 0 */

    ppu_tick(&f.gb, 81);
    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[3]); /* line 0 is the window */
    fixture_destroy(&f);
}

static void t_ppu_window_line_counter_resets(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 0);
    load_solid_tile(&f.gb, 2, 3);
    load_map(&f.gb, 0x9800, 1);
    load_map(&f.gb, 0x9C00, 2);

    gb_write(&f.gb, 0xFF40, 0xF1);
    gb_write(&f.gb, 0xFF47, 0xE4);
    gb_write(&f.gb, 0xFF4B, 0x07);
    gb_write(&f.gb, 0xFF4A, 0x00);

    ppu_tick(&f.gb, 81);
    CHECK_EQ(f.gb.ppu.window_line, 1);
    ppu_tick(&f.gb, 456); /* finish line 0 */
    ppu_tick(&f.gb, 81);  /* render line 1 */
    CHECK_EQ(f.gb.ppu.window_line, 2);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Sprites                                                             */
/* ------------------------------------------------------------------ */

/* Puts a single sprite into OAM. */
static void set_sprite(Fixture *f, int index, int x, int y, int tile,
                       uint8_t attrs)
{
    uint16_t base = (uint16_t)(GB_MEM_OAM + index * 4);

    gb_write(&f->gb, base + 0, (uint8_t)(y + 16));
    gb_write(&f->gb, base + 1, (uint8_t)(x + 8));
    gb_write(&f->gb, base + 2, (uint8_t)tile);
    gb_write(&f->gb, base + 3, attrs);
}

static void t_ppu_sprite_is_drawn(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_solid_tile(&f.gb, 1, 3);
    load_map(&f.gb, 0x9800, 2); /* blank background */
    load_solid_tile(&f.gb, 2, 0);

    set_sprite(&f, 0, 16, 0, 1, 0x00);
    gb_write(&f.gb, 0xFF40, 0x93); /* LCD on, BG on, sprites on */
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 16, 0), f.gb.ppu.palette[3]);
    CHECK_EQ(pixel(&f, 23, 0), f.gb.ppu.palette[3]);
    CHECK_EQ(pixel(&f, 15, 0), f.gb.ppu.palette[0]); /* one pixel too far left */
    fixture_destroy(&f);
}

static void t_ppu_sprite_is_clipped_at_the_edges(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_solid_tile(&f.gb, 1, 3);
    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);

    /* Half off the left edge. */
    set_sprite(&f, 0, -4, 0, 1, 0x00);
    gb_write(&f.gb, 0xFF40, 0x93);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 0, 0), f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_ppu_sprite_palettes(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 3);
    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);

    set_sprite(&f, 0, 16, 0, 1, 0x10); /* palette 1 */
    gb_write(&f.gb, 0xFF40, 0x93);
    gb_write(&f.gb, 0xFF47, 0xE4);
    gb_write(&f.gb, 0xFF48, 0xE4); /* OBP0: identity  */
    gb_write(&f.gb, 0xFF49, 0x1B); /* OBP1: send 3 to 0 */
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 16, 0), f.gb.ppu.palette[0]);
    fixture_destroy(&f);
}

static void t_ppu_sprite_x_flip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);
    /* Tile 1: only the leftmost pixel is set. */
    gb_write(&f.gb, 0x8010, 0x80);
    gb_write(&f.gb, 0x8011, 0x00);

    set_sprite(&f, 0, 16, 0, 1, 0x40); /* X flip */
    gb_write(&f.gb, 0xFF40, 0x93);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 23, 0), f.gb.ppu.palette[1]); /* moved to the right */
    CHECK_EQ(pixel(&f, 16, 0), f.gb.ppu.palette[0]);
    fixture_destroy(&f);
}

static void t_ppu_sprite_y_flip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);
    /* Tile 1: only the topmost row is set. */
    gb_write(&f.gb, 0x8010, 0xFF);
    gb_write(&f.gb, 0x8011, 0x00);
    for (int r = 1; r < 8; r++) {
        gb_write(&f.gb, (uint16_t)(0x8010 + r * 2), 0x00);
        gb_write(&f.gb, (uint16_t)(0x8011 + r * 2), 0x00);
    }

    /* Place it so the flipped row lands on screen line 0. */
    set_sprite(&f, 0, 16, 0, 1, 0x20); /* Y flip */
    gb_write(&f.gb, 0xFF40, 0x93);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    /* With a Y flip the bottom row (all empty) is drawn first, so line 0
     * of the sprite is blank and line 7 is solid. */
    CHECK_EQ(pixel(&f, 16, 0), f.gb.ppu.palette[0]);
    fixture_destroy(&f);
}

static void t_ppu_sprite_16_tall(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);
    load_solid_tile(&f.gb, 4, 3); /* second half of the tall sprite */
    load_solid_tile(&f.gb, 5, 3);

    set_sprite(&f, 0, 16, 0, 4, 0x00);
    gb_write(&f.gb, 0xFF40, 0x97); /* bit 2: sprites are 8x16 */
    gb_write(&f.gb, 0xFF47, 0xE4);

    ppu_tick(&f.gb, 81);
    CHECK_EQ(pixel(&f, 16, 0), f.gb.ppu.palette[3]); /* tile 4, top half */

    ppu_tick(&f.gb, 456 - 81);
    ppu_tick(&f.gb, 81);
    CHECK_EQ(f.gb.ppu.ly, 1);
    CHECK_EQ(pixel(&f, 16, 1), f.gb.ppu.palette[3]); /* tile 5, bottom half */
    fixture_destroy(&f);
}

static void t_ppu_smaller_x_wins(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_solid_tile(&f.gb, 1, 3);
    load_solid_tile(&f.gb, 2, 2);
    load_map(&f.gb, 0x9800, 0);

    /* Two overlapping sprites; the one further right sits first in OAM. */
    set_sprite(&f, 0, 20, 0, 1, 0x00); /* covers x = 20..27 */
    set_sprite(&f, 1, 16, 0, 2, 0x00); /* covers x = 16..23 */

    gb_write(&f.gb, 0xFF40, 0x93);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    /* Sprite 1 has the smaller X, so its shade 2 wins in the overlap. */
    CHECK_EQ(pixel(&f, 20, 0), f.gb.ppu.palette[2]);
    CHECK_EQ(pixel(&f, 24, 0), f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_ppu_equal_x_resolves_to_lower_oam_index(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_solid_tile(&f.gb, 1, 3);
    load_solid_tile(&f.gb, 2, 2);
    load_map(&f.gb, 0x9800, 0);

    set_sprite(&f, 0, 16, 0, 1, 0x00);
    set_sprite(&f, 1, 16, 0, 2, 0x00);

    gb_write(&f.gb, 0xFF40, 0x93);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 20, 0), f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_ppu_sprite_behind_the_background(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 3);
    load_solid_tile(&f.gb, 2, 2);
    load_map(&f.gb, 0x9800, 1); /* opaque background */

    set_sprite(&f, 0, 16, 0, 2, 0x80); /* behind background */
    gb_write(&f.gb, 0xFF40, 0x93);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 20, 0), f.gb.ppu.palette[3]); /* background wins */
    fixture_destroy(&f);
}

static void t_ppu_sprite_shows_over_a_blank_background(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_solid_tile(&f.gb, 1, 3);
    load_solid_tile(&f.gb, 2, 0);
    load_map(&f.gb, 0x9800, 2); /* colour index 0, so transparent */

    set_sprite(&f, 0, 16, 0, 1, 0x80); /* flagged behind, but BG is 0 */
    gb_write(&f.gb, 0xFF40, 0x93);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 20, 0), f.gb.ppu.palette[3]); /* sprite is visible */
    fixture_destroy(&f);
}

static void t_ppu_sprites_disabled_by_lcdc(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_solid_tile(&f.gb, 1, 3);
    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);

    set_sprite(&f, 0, 16, 0, 1, 0x00);
    gb_write(&f.gb, 0xFF40, 0x91); /* LCD on, sprites off */
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(pixel(&f, 16, 0), f.gb.ppu.palette[0]);
    fixture_destroy(&f);
}

static void t_ppu_sprites_stretch_mode_three(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);
    load_solid_tile(&f.gb, 4, 3);
    load_solid_tile(&f.gb, 5, 3);

    /* An object whose leftmost pixel is at screen x 8, with no scrolling,
     * so it lands on the first pixel of tile column one. */
    set_sprite(&f, 0, 16, 0, 4, 0x00);
    gb_write(&f.gb, 0xFF40, 0x97);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    /* The reference's rule: 172, plus the six pixels of its own tile that
     * are still to come less the two the fetcher gets anyway (7 - 2 = 5),
     * plus a flat six to fetch the object. */
    CHECK_EQ(f.gb.ppu.mode3_len, 172 + 5 + 6);
    CHECK_EQ(f.gb.ppu.mode, 3);
    fixture_destroy(&f);
}

/* Scrolling delays the start of mode 3 by however far the first tile is
 * shifted, which is a stall just like any other. */
static void t_ppu_scx_delays_the_first_fetch(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);

    gb_write(&f.gb, 0xFF40, 0x91);
    gb_write(&f.gb, 0xFF47, 0xE4);

    for (uint8_t scx = 0; scx < 8; scx++) {
        gb_write(&f.gb, 0xFF43, scx);
        ppu_tick(&f.gb, 456); /* a whole line, back to line 0 in mode 2 */
        ppu_tick(&f.gb, 81);
        CHECK_MSG(f.gb.ppu.mode3_len == 172 + scx,
                  "SCX %u should cost %u dots of penalty, got %d",
                  scx, 172 + scx, f.gb.ppu.mode3_len);
        ppu_tick(&f.gb, 456 * 154); /* skip the rest of the frame */
    }
    fixture_destroy(&f);
}

/* Two objects in the same background tile only pay the background part
 * once; the second one just costs its own tile fetch. */
static void t_ppu_two_objects_in_one_tile_pay_once(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);

    /* Both start at screen x 8, so both land in tile column one. */
    set_sprite(&f, 0, 16, 0, 4, 0x00);
    set_sprite(&f, 1, 17, 0, 4, 0x00);
    gb_write(&f.gb, 0xFF40, 0x97);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);
    CHECK_EQ(f.gb.ppu.mode3_len, 172 + 5 + 6 + 6);

    fixture_destroy(&f);
}

/* An object hanging off the left edge is charged the flat worst case. */
static void t_ppu_edge_object_costs_eleven_dots(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);

    set_sprite(&f, 0, 8, 0, 4, 0x00); /* OAM X 0: entirely off screen */
    gb_write(&f.gb, 0xFF40, 0x97);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK_EQ(f.gb.ppu.mode3_len, 172 + 11);
    CHECK_EQ(f.gb.ppu.mode, 3);
    fixture_destroy(&f);
}

/* A line can never take longer than 376 dots, or mode 0 would run out of
 * room and the next line would start late. */
static void t_ppu_a_crowded_line_still_fits_in_the_scanline(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint8_t line[GB_SCREEN_W];

    load_map(&f.gb, 0x9800, 2);
    load_solid_tile(&f.gb, 2, 0);

    /* Forty objects, each starting at the far left of its own tile, is
     * more stalls than a scanline has room for. */
    for (int i = 0; i < 40; i++)
        set_sprite(&f, (uint8_t)i, (uint8_t)(8 + (i % 20) * 8), 0, 4, 0x00);
    gb_write(&f.gb, 0xFF40, 0x97);
    gb_write(&f.gb, 0xFF47, 0xE4);
    ppu_tick(&f.gb, 81);

    CHECK(f.gb.ppu.mode3_len <= 376);
    CHECK(f.gb.ppu.mode3_len >= 172);

    /* And the line still completes on schedule. */
    memset(line, 0, sizeof(line));
    ppu_tick(&f.gb, 456 * 154 - 81);
    CHECK_EQ(f.gb.ppu.ly, 0);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Framebuffer and timing end to end                                    */
/* ------------------------------------------------------------------ */

static void t_ppu_frame_completes_in_70224_cycles(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.ppu.frame_done = false;
    ppu_tick(&f.gb, GB_CYCLES_PER_FRAME);
    CHECK_EQ(f.gb.ppu.frame_done, true);
    CHECK_EQ(f.gb.ppu.frame, 1);
    fixture_destroy(&f);
}

static void t_ppu_frames_keep_a_steady_rhythm(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    for (int i = 0; i < 10; i++) {
        f.gb.ppu.frame_done = false;
        ppu_tick(&f.gb, GB_CYCLES_PER_FRAME);
        CHECK_EQ(f.gb.ppu.frame, i + 1);
    }
    fixture_destroy(&f);
}

static void t_ppu_run_frame_returns_on_vblank(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint64_t before = f.gb.cycles;

    gb_run_frame(&f.gb);
    CHECK_EQ(f.gb.ppu.frame_done, true);
    CHECK_EQ(f.gb.ppu.frame >= 1, true);
    CHECK(f.gb.cycles - before >= GB_CYCLES_PER_FRAME);
    fixture_destroy(&f);
}

const TestCase ppu_tests[] = {
    { "ppu", "starts in oam scan", t_ppu_starts_in_oam_scan },
    { "ppu", "mode two lasts eighty dots", t_ppu_mode_two_lasts_eighty_dots },
    { "ppu", "mode three lasts 172 dots", t_ppu_mode_three_lasts_172_dots },
    { "ppu", "hblank runs to the end of the line", t_ppu_hblank_runs_to_the_end_of_the_line },
    { "ppu", "a line is 456 dots", t_ppu_a_line_is_456_dots },
    { "ppu", "counts through to ly 143", t_ppu_counts_through_to_ly_143 },
    { "ppu", "vblank starts at line 144", t_ppu_vblank_starts_at_line_144 },
    { "ppu", "wraps back to line zero", t_ppu_wraps_back_to_line_zero },
    { "ppu", "does not advance when the lcd is off", t_ppu_does_not_advance_when_the_lcd_is_off },
    { "ppu", "re-enabling restarts the frame", t_ppu_reenabling_restarts_the_frame },
    { "ppu", "lyc stat interrupt", t_ppu_lyc_stat_interrupt },
    { "ppu", "stat only fires on the rising edge", t_ppu_stat_only_fires_on_the_rising_edge },
    { "ppu", "hblank stat interrupt", t_ppu_hblank_stat_interrupt },
    { "ppu", "vblank stat interrupt", t_ppu_vblank_stat_interrupt },
    { "ppu", "oam stat interrupt", t_ppu_oam_stat_interrupt },
    { "ppu", "stat bit seven always reads high", t_ppu_stat_bit_seven_always_reads_high },
    { "ppu", "lyc can change mid frame", t_ppu_lyc_can_be_changed_mid_frame },
    { "ppu", "background draws a solid tile", t_ppu_background_draws_a_solid_tile },
    { "ppu", "tile index zero uses the same data", t_ppu_tile_index_zero_uses_the_same_data },
    { "ppu", "scx scrolls the line", t_ppu_scx_scrolls_the_line },
    { "ppu", "scy scrolls the map", t_ppu_scy_scrolls_the_map },
    { "ppu", "second map can be selected", t_ppu_second_map_can_be_selected },
    { "ppu", "bgp remaps the shades", t_ppu_bgp_remaps_the_shades },
    { "ppu", "blank when the background is disabled", t_ppu_blank_when_the_background_is_disabled },
    { "ppu", "pixel pattern within a tile", t_ppu_pixel_pattern_within_a_tile },
    { "ppu", "window covers the right side", t_ppu_window_covers_the_right_side },
    { "ppu", "window is hidden above wy", t_ppu_window_is_hidden_above_wy },
    { "ppu", "window appears from wy", t_ppu_window_appears_from_wy },
    { "ppu", "window line counter resets", t_ppu_window_line_counter_resets },
    { "ppu", "sprite is drawn", t_ppu_sprite_is_drawn },
    { "ppu", "sprite is clipped at the edges", t_ppu_sprite_is_clipped_at_the_edges },
    { "ppu", "sprite palettes", t_ppu_sprite_palettes },
    { "ppu", "sprite x flip", t_ppu_sprite_x_flip },
    { "ppu", "sprite y flip", t_ppu_sprite_y_flip },
    { "ppu", "sprite 16 tall", t_ppu_sprite_16_tall },
    { "ppu", "smaller x wins", t_ppu_smaller_x_wins },
    { "ppu", "equal x resolves to lower oam index", t_ppu_equal_x_resolves_to_lower_oam_index },
    { "ppu", "sprite behind the background", t_ppu_sprite_behind_the_background },
    { "ppu", "sprite shows over a blank background", t_ppu_sprite_shows_over_a_blank_background },
    { "ppu", "sprites disabled by lcdc", t_ppu_sprites_disabled_by_lcdc },
    { "ppu", "sprites stretch mode three", t_ppu_sprites_stretch_mode_three },
    { "ppu", "scx delays the first fetch", t_ppu_scx_delays_the_first_fetch },
    { "ppu", "two objects in one tile pay once", t_ppu_two_objects_in_one_tile_pay_once },
    { "ppu", "edge object costs eleven dots", t_ppu_edge_object_costs_eleven_dots },
    { "ppu", "a crowded line still fits in the scanline", t_ppu_a_crowded_line_still_fits_in_the_scanline },
    { "ppu", "frame completes in 70224 cycles", t_ppu_frame_completes_in_70224_cycles },
    { "ppu", "frames keep a steady rhythm", t_ppu_frames_keep_a_steady_rhythm },
    { "ppu", "run frame returns on vblank", t_ppu_run_frame_returns_on_vblank },
    TEST_SUITE_END
};