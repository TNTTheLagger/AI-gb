/*
 * joypad.c -- the $FF00 register: a 4x2 matrix of buttons scanned by
 * selecting one of the two rows and reading the column lines.
 */
#include "gb.h"

/* Clearing bit 4 of P1 selects the d-pad, clearing bit 5 the buttons. */
#define ROW_DIRECTIONS 0x10
#define ROW_ACTIONS 0x20

/*
 * Build the four column lines.  A line reads low (0) when the button
 * underneath it is held down, so an idle button leaves the line high.
 */
static uint8_t joypad_lines(const Joypad *j)
{
    uint8_t lines = 0;

    if (!(j->sel & ROW_ACTIONS))
        lines |= (uint8_t)((j->keys >> 4) & 0x0F); /* A, B, Select, Start */
    if (!(j->sel & ROW_DIRECTIONS))
        lines |= (uint8_t)(j->keys & 0x0F); /* Right, Left, Up, Down */

    return (uint8_t)(~lines & 0x0F);
}

/* The register image a read of $FF00 returns: bits 7 and 6 are unused
 * and read as one, bits 5 and 4 read back the row selects as written, and
 * the low nibble is the column lines. */
uint8_t joypad_read(const GB *gb)
{
    uint8_t lines = joypad_lines(&gb->joypad);

    return (uint8_t)(0xC0 | gb->joypad.sel | (lines & 0x0F));
}

void joypad_tick(GB *gb)
{
    Joypad *j = &gb->joypad;
    uint8_t lines = joypad_lines(j);

    for (int i = 0; i < 4; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        bool was_high = (j->prev_line & bit) != 0;
        bool is_high = (lines & bit) != 0;

        /* A high to low transition means a button went down. */
        if (was_high && !is_high)
            gb->iflag |= GB_INT_JOYPAD;

        if (is_high)
            j->prev_line |= bit;
        else
            j->prev_line &= (uint8_t)~bit;
    }
}

void gb_set_keys(GB *gb, uint8_t keys)
{
    gb->joypad.keys = keys;
    joypad_tick(gb);
}