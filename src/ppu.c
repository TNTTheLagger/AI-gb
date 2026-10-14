/*
 * ppu.c -- the DMG dot based LCD controller.
 *
 * Timing model: a scanline is 456 dots split into mode 2 (80 dots of OAM
 * scan), mode 3 (the pixel transfer) and mode 0 (HBlank).  Mode 3 lasts at
 * least 172 dots and is stretched by six dots for every sprite the line
 * fetches, which is what makes busy lines run long.
 *
 * The line is rendered when mode 3 begins.  Writes a game performs during
 * mode 3 therefore only take effect on the next scanline, matching the way
 * the real LCD behaves.
 */
#include <string.h>

#include "gb.h"

/* Default DMG look: white through black. */
static const uint32_t default_palette[4] = {
    0xFFE0F8D0u, /* 0, lightest */
    0xFF88C070u, /* 1 */
    0xFF346856u, /* 2 */
    0xFF081820u  /* 3, darkest  */
};

/* Dots the pixel transfer is stalled for by each sprite fetch. */

#define MODE_HBLANK 0
#define MODE_VBLANK 1
#define MODE_OAM 2
#define MODE_TRANSFER 3

static inline uint8_t palette_map(uint8_t palette_reg, uint8_t index)
{
    return (uint8_t)((palette_reg >> (index * 2)) & 0x03);
}

static inline uint8_t flip_x(uint8_t v)
{
    return (uint8_t)(((v & 0x01) << 7) | ((v & 0x02) << 5) | ((v & 0x04) << 3) |
                     ((v & 0x08) << 1) | ((v & 0x10) >> 1) | ((v & 0x20) >> 3) |
                     ((v & 0x40) >> 5) | ((v & 0x80) >> 7));
}

/* ------------------------------------------------------------------ */
/* Tile fetch                                                          */
/* ------------------------------------------------------------------ */

static uint8_t fetch_bg_pixel(GB *gb, uint16_t map_base, uint16_t data_base,
                              uint8_t py, uint8_t px)
{
    uint8_t map_x = (uint8_t)(px >> 3);
    uint8_t map_y = (uint8_t)(py >> 3);
    uint8_t tile = gb->vram[map_base + (map_y * 32) + map_x];
    uint16_t tile_addr = (uint16_t)(data_base + tile * 16);
    uint8_t fine_y = (uint8_t)(py & 7);
    uint8_t bit = (uint8_t)(7 - (px & 7));

    uint8_t lo = gb->vram[(tile_addr + fine_y * 2) & 0x1FFF];
    uint8_t hi = gb->vram[(tile_addr + fine_y * 2 + 1) & 0x1FFF];

    return (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
}

/* ------------------------------------------------------------------ */
/* Mode 3 length                                                       */
/* ------------------------------------------------------------------ */

/*
 * How long the pixel transfer takes.  The floor is the width of the screen
 * plus the fixed cost of the fetcher running behind it; everything on top
 * of that is a stall, and each stall is taken out of horizontal blank
 * instead, which is why a busy line has a shorter mode 0.
 *
 * Three things stall the fetcher:
 *
 *   - Background scrolling.  The first tile of the line arrives shifted
 *     by SCX, and the pixels that fall off the left have to be discarded,
 *     so the line starts SCX % 8 dots late.
 *   - The window.  Switching from the background fetcher to the window
 *     fetcher costs a flat 6 dots.
 *   - Objects.  Each object has to be fetched too, and the cost depends on
 *     where in the background it starts -- see mode3_add_object_penalty.
 */

#define MODE3_BASE 172
#define MODE3_WINDOW_PENALTY 6
#define MODE3_OBJECT_PENALTY 6
#define MODE3_EDGE_OBJECT_PENALTY 11
#define MODE3_MAX_TILES 21   /* 160 pixels / 8, rounded up for SCX */

static int mode3_base_length(const PPU *p)
{
    int len = MODE3_BASE + (p->scx & 7);

    if (p->window_drawn)
        len += MODE3_WINDOW_PENALTY;
    return len;
}

/*
 * The cost of one object, charged as the objects are visited in OAM order.
 *
 * What matters is the object's leftmost pixel, whether or not it ends up
 * being drawn: the fetcher has to be looking at it before it can decide.
 * That pixel sits in a background or window tile, and arriving early in a
 * tile is cheaper than arriving late, because the rest of the tile is
 * already on its way.  Two objects landing in the same tile only pay the
 * background cost once, hence the remembered set.
 */
static void mode3_add_object_penalty(PPU *p, uint8_t oam_x, int sprite_x)
{
    int px, coord, col, within, extra;

    /* An object at X position 0 sits entirely off the left of the screen,
     * so the fetcher never gets any of it ready in advance. */
    if (oam_x == 0) {
        p->mode3_len += MODE3_EDGE_OBJECT_PENALTY;
        return;
    }

    /* The leftmost pixel that lands on the screen.  An object hanging off
     * the left edge is charged as if it started at zero, which is the
     * reading the reference's own wording implies. */
    px = sprite_x < 0 ? 0 : sprite_x;

    if (p->window_drawn && px >= p->window_start) {
        coord = px - p->window_start;
    } else {
        coord = px + p->scx;
    }

    col = coord / 8;
    within = coord & 7;

    if (col < MODE3_MAX_TILES && !p->tile_considered[col]) {
        p->tile_considered[col] = true;
        /* The pixels of this tile still to come, less the two that the
         * fetcher manages regardless. */
        extra = (7 - within) - 2;
        if (extra > 0)
            p->mode3_len += extra;
    }

    p->mode3_len += MODE3_OBJECT_PENALTY;
}

/* ------------------------------------------------------------------ */
/* Scanline renderer                                                   */
/* ------------------------------------------------------------------ */

static void render_scanline(GB *gb)
{
    PPU *p = &gb->ppu;
    uint16_t map_base = (p->lcdc & 0x08) ? 0x1C00 : 0x1800;
    uint16_t data_base = (p->lcdc & 0x10) ? 0x0000 : 0x0800;
    bool bg_enabled = (p->lcdc & 0x01) != 0;
    uint8_t bg_idx[GB_SCREEN_W];
    uint8_t spr_idx[GB_SCREEN_W];
    bool spr_shown[GB_SCREEN_W];
    uint32_t *out = &p->fb[(size_t)p->ly * GB_SCREEN_W];
    int x;

    p->sprites_fetched = 0;
    p->window_drawn = false;
    p->window_start = 0;
    memset(p->tile_considered, 0, sizeof(p->tile_considered));
    p->mode3_len = mode3_base_length(p);

    /* ---- background ---- */
    if (bg_enabled) {
        uint8_t y = (uint8_t)(p->ly + p->scy);
        for (x = 0; x < GB_SCREEN_W; x++)
            bg_idx[x] = fetch_bg_pixel(gb, map_base, data_base, y,
                                       (uint8_t)(x + p->scx));
    } else {
        memset(bg_idx, 0, sizeof(bg_idx));
    }

    /* ---- window ---- */
    p->window_drawn = false;
    if ((p->lcdc & 0x20) && p->ly >= p->wy && p->wx <= 166) {
        uint16_t win_map = (p->lcdc & 0x40) ? 0x1C00 : 0x1800;
        uint8_t y = (uint8_t)(p->wy + p->window_line);
        int start = (int)p->wx - 7;

        if (start < 0)
            start = 0;
        for (x = start; x < GB_SCREEN_W; x++)
            bg_idx[x] = fetch_bg_pixel(gb, win_map, data_base, y,
                                       (uint8_t)(x - start));
        p->window_drawn = true;
        p->window_start = start;
        p->window_line++;
    }

    for (x = 0; x < GB_SCREEN_W; x++)
        bg_idx[x] = palette_map(p->bgp, bg_idx[x]);

    /* ---- sprites ---- */
    memset(spr_shown, 0, sizeof(spr_shown));

    if (p->lcdc & 0x02) {
        int height = (p->lcdc & 0x04) ? 16 : 8;
        uint8_t best_x[GB_SCREEN_W]; /* lowest X wins, ties to lower OAM */

        for (x = 0; x < GB_SCREEN_W; x++)
            best_x[x] = 0xFF;

        for (int i = 0; i < 40; i++) {
            const uint8_t *entry = &gb->oam[i * 4];
            /* OAM stores the position biased by 16 and 8 so that a sprite
             * can hang off the edge of the screen. */
            int sprite_y = (int)(int8_t)entry[0] - 16;
            int sprite_x = (int)(int8_t)entry[1] - 8;
            uint8_t attrs = entry[3];
            int tile_index = entry[2];
            int row_in_tile;
            uint16_t addr;
            uint8_t lo, hi, pal;

            if ((int)p->ly < sprite_y || (int)p->ly >= sprite_y + height)
                continue;

            p->sprites_fetched++;
            mode3_add_object_penalty(p, entry[1], sprite_x);
            row_in_tile = (int)p->ly - sprite_y;

            if (height == 16)
                tile_index &= 0xFE;
            if (row_in_tile >= 8) {
                row_in_tile -= 8;
                tile_index++; /* 8x16 sprites span two tiles */
            }

            /* A Y flip mirrors the row before the tile data is fetched. */
            if (attrs & 0x20)
                row_in_tile = height - 1 - row_in_tile;

            /* Sprite tile data always lives at $8000, never at $8800, so the
             * offset is simply the tile number times sixteen within VRAM. */
            addr = (uint16_t)(tile_index * 16 + row_in_tile * 2);
            lo = gb->vram[addr & 0x1FFF];
            hi = gb->vram[(addr + 1) & 0x1FFF];
            pal = (attrs & 0x10) ? p->obp1 : p->obp0;

            if (attrs & 0x40) { /* X flip */
                lo = flip_x(lo);
                hi = flip_x(hi);
            }

            for (int px = 0; px < 8; px++) {
                int sx = sprite_x + px;
                uint8_t bit = (uint8_t)(7 - px);
                uint8_t color = (uint8_t)(((lo >> bit) & 1) |
                                          (((hi >> bit) & 1) << 1));
                uint8_t rank;

                if (color == 0)
                    continue;
                if (sx < 0 || sx >= GB_SCREEN_W)
                    continue;

                /* DMG priority: the sprite with the smaller X coordinate
                 * wins; equal X resolves to the lower OAM index. */
                rank = (uint8_t)((sprite_x < 0 ? 0 : sprite_x) & 0xFF);
                if (best_x[sx] != 0xFF && best_x[sx] <= rank)
                    continue;

                if (attrs & 0x80) {
                    /* Flagged "behind background": colours 1-3 are still
                     * hidden by any non-zero background pixel. */
                    if (bg_enabled && bg_idx[sx] != 0)
                        continue;
                }

                best_x[sx] = rank;
                spr_shown[sx] = true;
                spr_idx[sx] = palette_map(pal, color);
            }
        }
    }

    /* ---- resolve ---- */
    for (x = 0; x < GB_SCREEN_W; x++)
        out[x] = p->palette[spr_shown[x] ? spr_idx[x] : bg_idx[x]];
}

/* ------------------------------------------------------------------ */
/* STAT interrupt line                                                 */
/* ------------------------------------------------------------------ */

static bool stat_line_state(const GB *gb)
{
    const PPU *p = &gb->ppu;

    if ((p->stat & 0x40) && p->ly == p->lyc)
        return true;
    if ((p->stat & 0x20) && p->mode == MODE_OAM)
        return true;
    if ((p->stat & 0x10) && p->mode == MODE_VBLANK)
        return true;
    if ((p->stat & 0x08) && p->mode == MODE_HBLANK)
        return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Mode transitions                                                    */
/* ------------------------------------------------------------------ */

/* Dot at which the current mode ends. */
static int mode_boundary(const PPU *p)
{
    switch (p->mode) {
    case MODE_OAM: return 80;
    case MODE_TRANSFER: return 80 + p->mode3_len;
    default: return 456;
    }
}

static void advance_mode(GB *gb)
{
    PPU *p = &gb->ppu;

    switch (p->mode) {
    case MODE_OAM:
        /* The pixel transfer begins: this is where the line gets drawn.
         * Drawing the line also works out how long the transfer has to be,
         * because every stall the hardware suffers is one of the things
         * the renderer has just decided on. */
        render_scanline(gb);
        if (p->mode3_len < 172)
            p->mode3_len = 172;   /* the floor is the bare screen width */
        if (p->mode3_len > 376)
            p->mode3_len = 376;   /* mode 0 has to leave at least 80 dots */
        p->mode = MODE_TRANSFER;
        break;

    case MODE_TRANSFER:
        p->mode = MODE_HBLANK;
        break;

    default: /* HBlank or VBlank finished, move on to the next line */
        if (p->ly == 153) {
            /* The 154 line frame period is complete.  The framebuffer is
             * left alone here: every visible line is overwritten as it is
             * drawn, and wiping it now would blank the finished frame for
             * whoever is about to look at it. */
            p->ly = 0;
            p->mode = MODE_OAM;
            p->window_line = 0;
            p->frame_done = true;
            p->frame++;
        } else if (p->ly == 143) {
            gb->iflag |= GB_INT_VBLANK;
            p->ly = 144;
            p->mode = MODE_VBLANK;
            p->window_line = 0;
        } else if (p->ly < 144) {
            p->ly++;
            p->mode = MODE_OAM;
        } else {
            /* Lines 145 to 153 stay in VBlank. */
            p->ly++;
            p->mode = MODE_VBLANK;
        }
        p->dot = 0;
        break;
    }

    ppu_update_stat(gb);
}

/* ------------------------------------------------------------------ */
/* Main tick                                                           */
/* ------------------------------------------------------------------ */

/* Recomputes the STAT interrupt line and raises an interrupt if it has
 * just gone high.  Called whenever the mode, LY or the STAT register
 * change. */
void ppu_update_stat(GB *gb)
{
    bool line = stat_line_state(gb);

    if (line && !gb->ppu.stat_line)
        gb->iflag |= GB_INT_LCDSTAT;
    gb->ppu.stat_line = line;
}

void ppu_tick(GB *gb, int cycles)
{
    PPU *p = &gb->ppu;

    if (!(p->lcdc & 0x80))
        return; /* the LCD is powered down, no timing advances */

    while (cycles > 0) {
        int boundary = mode_boundary(p);
        int step = boundary - p->dot;

        if (step <= 0)
            step = 1;
        if (step > cycles)
            step = cycles;

        p->dot += step;
        cycles -= step;

        if (p->dot >= boundary) {
            /*
             * A mode can be zero dots long: a line crowded enough with
             * objects stretches the pixel transfer to the full 376 dots a
             * scanline has, leaving horizontal blank with nothing left to
             * wait for.  The line still has to end on schedule, so the
             * step past an empty mode is taken without charging a dot for
             * it.
             */
            while (p->dot >= boundary) {
                int was_mode = p->mode;

                advance_mode(gb);
                boundary = mode_boundary(p);
                if (p->dot < boundary)
                    break;
                if (p->mode == was_mode)
                    break; /* nothing moved on; step out rather than spin */
            }
        }
    }
}

void ppu_reset(GB *gb)
{
    PPU *p = &gb->ppu;

    memcpy(p->palette, default_palette, sizeof(default_palette));
    /* The values below are what the DMG boot ROM leaves behind, as
     * recorded in Pan Docs' post-boot hardware register table. */
    p->lcdc = 0x91;
    p->stat = 0x85;   /* LYC compare set, and the mode left at VBlank */
    p->scy = p->scx = 0;
    p->ly = 0;
    p->lyc = 0;
    p->dma = 0xFF;
    /*
     * The three palette registers are the one deliberate departure from the
     * post-boot table.  Hardware leaves them holding whatever the last
     * power cycle left behind, and the reference is explicit that they
     * are uninitialised and must not be relied on; in practice they read
     * $00 or $FF.  An identity mapping is the most useful of the plausible
     * values for a machine that never ran the boot ROM: a game that forgets
     * to set its palettes still shows a readable picture instead of one
     * where every shade is the darkest.
     */
    p->bgp = 0xE4;
    p->obp0 = 0xE4;
    p->obp1 = 0xE4;
    p->wy = p->wx = 0;
    p->dot = 0;
    /*
     * The reference records STAT as $85 at $0100, so the mode field reads
     * 1, VBlank, and bit 2 is set because LY and LYC both start at zero.
     * Both of those are reproduced.  The mode itself starts as 2 instead,
     * because that is where a frame has to begin to draw line 0; entering
     * a frame at LY 0 in VBlank would step straight past line 0.
     */
    p->mode = MODE_OAM;
    p->mode3_len = MODE3_BASE;
    p->frame_done = false;
    p->frame = 0;
    p->stat_line = false;
    p->window_line = 0;
    p->sprites_fetched = 0;
    memset(p->line, 0, sizeof(p->line));
    memset(p->fb, 0, sizeof(p->fb));
    ppu_update_stat(gb);
}

/* Turning the LCD back on restarts the frame from line zero. */
void ppu_lcd_off(GB *gb)
{
    gb->ppu.ly = 0;
    gb->ppu.dot = 0;
    gb->ppu.mode = MODE_HBLANK;
    gb->ppu.window_line = 0;
    gb->ppu.frame_done = true;
    gb->ppu.stat_line = false;
    memset(gb->ppu.fb, 0, sizeof(gb->ppu.fb));
}

void ppu_lcd_on(GB *gb)
{
    gb->ppu.ly = 0;
    gb->ppu.dot = 0;
    gb->ppu.mode = MODE_OAM;
    gb->ppu.window_line = 0;
    gb->ppu.frame_done = false;
    gb->ppu.stat_line = false;
    memset(gb->ppu.fb, 0, sizeof(gb->ppu.fb));
    ppu_update_stat(gb);
}