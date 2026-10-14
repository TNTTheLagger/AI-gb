/*
 * test_input.c -- the joypad matrix and the serial port.
 */
#include "test.h"

/* ------------------------------------------------------------------ */
/* Joypad                                                              */
/* ------------------------------------------------------------------ */

static void t_input_nothing_pressed_reads_high(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF00, 0x30); /* neither row selected */
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0F);
    fixture_destroy(&f);
}

static void t_input_direction_row(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF00, 0x20); /* action row not selected */
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0F);

    gb_set_keys(&f.gb, GB_KEY_LEFT);
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0D); /* P11 pulled low */
    fixture_destroy(&f);
}

static void t_input_action_row(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF00, 0x10); /* direction row not selected */
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0F);

    gb_set_keys(&f.gb, GB_KEY_A);
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0E); /* P14 pulled low */
    fixture_destroy(&f);
}

static void t_input_rows_are_independent(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_set_keys(&f.gb, GB_KEY_A);

    /* Only the action row is visible, so the A button shows. */
    gb_write(&f.gb, 0xFF00, 0x10);
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0E);

    /* Only the direction row is visible, where nothing is held. */
    gb_write(&f.gb, 0xFF00, 0x20);
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0F);
    fixture_destroy(&f);
}

static void t_input_each_direction_maps_to_its_own_line(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    struct {
        uint8_t key;
        uint8_t expected;
    } cases[] = {
        { GB_KEY_RIGHT, 0x0E },
        { GB_KEY_LEFT, 0x0D },
        { GB_KEY_UP, 0x0B },
        { GB_KEY_DOWN, 0x07 }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gb_set_keys(&f.gb, 0x00);
        gb_set_keys(&f.gb, cases[i].key);
        gb_write(&f.gb, 0xFF00, 0x20); /* direction row only */
        CHECK_MSG((gb_read(&f.gb, 0xFF00) & 0x0F) == cases[i].expected,
                  "key %02X: expected %02X, got %02X", cases[i].key,
                  cases[i].expected, gb_read(&f.gb, 0xFF00) & 0x0F);
    }
    fixture_destroy(&f);
}

static void t_input_each_action_maps_to_its_own_line(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    struct {
        uint8_t key;
        uint8_t expected;
    } cases[] = {
        /* A is P14, B is P15, Select is P16 and Start is P17. */
        { GB_KEY_A, 0x0E },
        { GB_KEY_B, 0x0D },
        { GB_KEY_SELECT, 0x0B },
        { GB_KEY_START, 0x07 }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gb_set_keys(&f.gb, 0x00);
        gb_set_keys(&f.gb, cases[i].key);
        gb_write(&f.gb, 0xFF00, 0x10); /* action row only */
        CHECK_MSG((gb_read(&f.gb, 0xFF00) & 0x0F) == cases[i].expected,
                  "key %02X: expected %02X, got %02X", cases[i].key,
                  cases[i].expected, gb_read(&f.gb, 0xFF00) & 0x0F);
    }
    fixture_destroy(&f);
}

static void t_input_two_buttons_at_once(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_set_keys(&f.gb, GB_KEY_LEFT | GB_KEY_UP);
    gb_write(&f.gb, 0xFF00, 0x20); /* direction row visible */
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x09); /* P11 and P12 pulled low */
    fixture_destroy(&f);
}

static void t_input_only_the_select_bits_are_writable(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* Writing takes effect in the row select itself... */
    gb_write(&f.gb, 0xFF00, 0xFF);
    CHECK_EQ(f.gb.joypad.sel, 0x30);

    /* ...and reads back.  The two unused bits above it read as one. */
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x30, 0x30);
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0xC0, 0xC0);
    fixture_destroy(&f);
}

static void t_input_press_raises_an_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.iflag = 0;
    gb_set_keys(&f.gb, GB_KEY_START);
    CHECK_EQ(f.gb.iflag & GB_INT_JOYPAD, GB_INT_JOYPAD);
    fixture_destroy(&f);
}

static void t_input_release_raises_no_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_set_keys(&f.gb, GB_KEY_START);
    f.gb.iflag = 0;
    gb_set_keys(&f.gb, 0x00);
    CHECK_EQ(f.gb.iflag & GB_INT_JOYPAD, 0);
    fixture_destroy(&f);
}

static void t_input_holding_does_not_repeat_the_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_set_keys(&f.gb, GB_KEY_START);
    f.gb.iflag = 0;
    for (int i = 0; i < 5; i++)
        gb_set_keys(&f.gb, GB_KEY_START);
    CHECK_EQ(f.gb.iflag & GB_INT_JOYPAD, 0);
    fixture_destroy(&f);
}

static void t_input_releasing_and_pressing_again_interrupts(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_set_keys(&f.gb, GB_KEY_A);
    gb_set_keys(&f.gb, 0x00);
    f.gb.iflag = 0;
    gb_set_keys(&f.gb, GB_KEY_A);
    CHECK_EQ(f.gb.iflag & GB_INT_JOYPAD, GB_INT_JOYPAD);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Serial                                                              */
/* ------------------------------------------------------------------ */

static void t_input_serial_registers(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF01, 0x5A);
    CHECK_EQ(gb_read(&f.gb, 0xFF01), 0x5A);
    CHECK_EQ(f.gb.serial.sb, 0x5A);

    gb_write(&f.gb, 0xFF02, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xFF02) & 0x01, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xFF02) & 0x80, 0x00);
    fixture_destroy(&f);
}

static void t_input_serial_internal_clock_completes(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.iflag = 0;
    gb_write(&f.gb, 0xFF01, 0x3C);
    gb_write(&f.gb, 0xFF02, 0x81); /* start with the internal clock */

    CHECK_EQ(f.gb.serial.counter, 8 * 512);
    serial_tick(&f.gb, 8 * 512 - 1);
    CHECK_EQ(f.gb.iflag & GB_INT_SERIAL, 0);
    CHECK_EQ(gb_read(&f.gb, 0xFF02) & 0x80, 0x80);

    serial_tick(&f.gb, 1);
    CHECK_EQ(f.gb.iflag & GB_INT_SERIAL, GB_INT_SERIAL);
    CHECK_EQ(gb_read(&f.gb, 0xFF02) & 0x80, 0x00); /* transfer finished */
    CHECK_EQ(gb_read(&f.gb, 0xFF01), 0xFF); /* nothing attached: idle high */
    fixture_destroy(&f);
}

static void t_input_serial_external_clock_never_finishes(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.iflag = 0;
    gb_write(&f.gb, 0xFF02, 0x80); /* start, but waiting on a partner */

    for (int i = 0; i < 100; i++)
        serial_tick(&f.gb, 512);

    CHECK_EQ(f.gb.iflag & GB_INT_SERIAL, 0);
    CHECK_EQ(gb_read(&f.gb, 0xFF02) & 0x80, 0x80);
    fixture_destroy(&f);
}

static void t_input_serial_does_not_run_while_idle(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.iflag = 0;
    serial_tick(&f.gb, 100000);
    CHECK_EQ(f.gb.iflag & GB_INT_SERIAL, 0);
    fixture_destroy(&f);
}

static void t_input_serial_is_reset(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF01, 0x11);
    gb_write(&f.gb, 0xFF02, 0x81);
    gb_reset(&f.gb);
    CHECK_EQ(f.gb.serial.sb, 0x00);
    CHECK_EQ(f.gb.serial.sc, 0x00);
    fixture_destroy(&f);
}

/*
 * Bits 5 and 4 of $FF00 select which row of the matrix is wired to the
 * column lines: clearing bit 4 selects the d-pad, clearing bit 5 the
 * buttons.  They read back as written.
 */
static void t_input_select_bits_read_back(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_set_keys(&f.gb, 0);

    for (uint8_t sel = 0x00; sel <= 0x30; sel += 0x10) {
        gb_write(&f.gb, 0xFF00, sel);

        CHECK_MSG((gb_read(&f.gb, 0xFF00) & 0x30) == sel,
                  "select $%02X read back as $%02X", sel,
                  gb_read(&f.gb, 0xFF00));
        CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0F);
    }

    /* A held button shows in the low nibble once its row is selected.
     * A is on the button row, selected by clearing bit 5. */
    gb_set_keys(&f.gb, GB_KEY_A);
    gb_write(&f.gb, 0xFF00, 0x10);
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x0F, 0x0E);
    CHECK_EQ(gb_read(&f.gb, 0xFF00) & 0x30, 0x10);

    fixture_destroy(&f);
}

const TestCase input_tests[] = {
    { "input", "select bits read back",      t_input_select_bits_read_back },
    { "input", "nothing pressed reads high", t_input_nothing_pressed_reads_high },
    { "input", "direction row", t_input_direction_row },
    { "input", "action row", t_input_action_row },
    { "input", "rows are independent", t_input_rows_are_independent },
    { "input", "each direction maps to its own line", t_input_each_direction_maps_to_its_own_line },
    { "input", "each action maps to its own line", t_input_each_action_maps_to_its_own_line },
    { "input", "two buttons at once", t_input_two_buttons_at_once },
    { "input", "only the select bits are writable", t_input_only_the_select_bits_are_writable },
    { "input", "press raises an interrupt", t_input_press_raises_an_interrupt },
    { "input", "release raises no interrupt", t_input_release_raises_no_interrupt },
    { "input", "holding does not repeat the interrupt", t_input_holding_does_not_repeat_the_interrupt },
    { "input", "press again interrupts", t_input_releasing_and_pressing_again_interrupts },
    { "input", "serial registers", t_input_serial_registers },
    { "input", "serial internal clock completes", t_input_serial_internal_clock_completes },
    { "input", "serial external clock never finishes", t_input_serial_external_clock_never_finishes },
    { "input", "serial does not run while idle", t_input_serial_does_not_run_while_idle },
    { "input", "serial is reset", t_input_serial_is_reset },
    TEST_SUITE_END
};