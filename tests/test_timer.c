/*
 * test_timer.c -- DIV, TIMA, TMA and TAC.
 */
#include "test.h"

/* Runs the machine for `cycles` T-cycles without executing any code. */
static void spin(GB *gb, int cycles)
{
    timer_tick(gb, cycles);
}

static void t_timer_div_counts_up(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.div = 0;
    spin(&f.gb, 256);
    CHECK_EQ(timer_div(&f.gb), 1);

    spin(&f.gb, 256);
    CHECK_EQ(timer_div(&f.gb), 2);
    fixture_destroy(&f);
}

static void t_timer_div_uses_the_upper_byte(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.div = 0x00FF;
    CHECK_EQ(timer_div(&f.gb), 0x00);
    spin(&f.gb, 1);
    CHECK_EQ(timer_div(&f.gb), 0x01);
    fixture_destroy(&f);
}

static void t_timer_div_wraps_after_65536(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.div = 0xFFFF;
    spin(&f.gb, 1);
    CHECK_EQ(f.gb.timer.div, 0);
    CHECK_EQ(timer_div(&f.gb), 0);
    fixture_destroy(&f);
}

static void t_timer_div_write_resets(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.div = 0x1234;
    gb_write(&f.gb, 0xFF04, 0xAB);
    CHECK_EQ(f.gb.timer.div, 0);
    CHECK_EQ(timer_div(&f.gb), 0);
    fixture_destroy(&f);
}

static void t_timer_does_not_run_while_disabled(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x00;
    f.gb.timer.tima = 0;
    f.gb.timer.div = 0;
    spin(&f.gb, 4096);
    CHECK_EQ(f.gb.timer.tima, 0);
    /* The divider keeps running though. */
    CHECK_EQ(timer_div(&f.gb), 16);
    fixture_destroy(&f);
}

/* Each TAC setting taps a different bit of the divider, so the periods are
 * 1024, 16, 64 and 256 T-cycles per TIMA step. */
static void t_timer_tac_frequency_0(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x04; /* enabled, tap bit 9 -> every 1024 T-cycles */
    f.gb.timer.tima = 0;
    f.gb.timer.div = 0;
    spin(&f.gb, 1023);
    CHECK_EQ(f.gb.timer.tima, 0);
    spin(&f.gb, 1);
    CHECK_EQ(f.gb.timer.tima, 1);
    fixture_destroy(&f);
}

static void t_timer_tac_frequency_1(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x05; /* tap bit 3 -> every 16 T-cycles */
    f.gb.timer.tima = 0;
    f.gb.timer.div = 0;
    spin(&f.gb, 15);
    CHECK_EQ(f.gb.timer.tima, 0);
    spin(&f.gb, 1);
    CHECK_EQ(f.gb.timer.tima, 1);
    fixture_destroy(&f);
}

static void t_timer_tac_frequency_2(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x06; /* tap bit 5 -> every 64 T-cycles */
    f.gb.timer.tima = 0;
    f.gb.timer.div = 0;
    spin(&f.gb, 63);
    CHECK_EQ(f.gb.timer.tima, 0);
    spin(&f.gb, 1);
    CHECK_EQ(f.gb.timer.tima, 1);
    fixture_destroy(&f);
}

static void t_timer_tac_frequency_3(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x07; /* tap bit 7 -> every 256 T-cycles */
    f.gb.timer.tima = 0;
    f.gb.timer.div = 0;
    spin(&f.gb, 255);
    CHECK_EQ(f.gb.timer.tima, 0);
    spin(&f.gb, 1);
    CHECK_EQ(f.gb.timer.tima, 1);
    fixture_destroy(&f);
}

static void t_timer_overflow_loads_tma_and_raises_an_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x05;
    f.gb.timer.tima = 0xFF;
    f.gb.timer.tma = 0x42;
    f.gb.timer.div = 0;
    f.gb.iflag = 0;

    spin(&f.gb, 16); /* one step: $FF wraps */

    CHECK_EQ(f.gb.timer.tima, 0x42);
    CHECK_EQ(f.gb.iflag & GB_INT_TIMER, GB_INT_TIMER);
    fixture_destroy(&f);
}

static void t_timer_overflow_to_zero_keeps_zero(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x05;
    f.gb.timer.tima = 0xFF;
    f.gb.timer.tma = 0x00;
    f.gb.timer.div = 0;
    f.gb.iflag = 0;

    spin(&f.gb, 16);
    CHECK_EQ(f.gb.timer.tima, 0x00);
    CHECK_EQ(f.gb.iflag & GB_INT_TIMER, GB_INT_TIMER);
    fixture_destroy(&f);
}

static void t_timer_counts_between_overflows(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x05;
    f.gb.timer.tima = 0xFD;
    f.gb.timer.tma = 0x00;
    f.gb.timer.div = 0;
    f.gb.iflag = 0;

    spin(&f.gb, 16);
    CHECK_EQ(f.gb.timer.tima, 0xFE);
    CHECK_EQ(f.gb.iflag & GB_INT_TIMER, 0);

    spin(&f.gb, 16);
    CHECK_EQ(f.gb.timer.tima, 0xFF);
    CHECK_EQ(f.gb.iflag & GB_INT_TIMER, 0);

    spin(&f.gb, 16);
    CHECK_EQ(f.gb.timer.tima, 0x00);
    CHECK_EQ(f.gb.iflag & GB_INT_TIMER, GB_INT_TIMER);
    fixture_destroy(&f);
}

static void t_timer_tima_is_writable(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF05, 0x99);
    CHECK_EQ(f.gb.timer.tima, 0x99);
    gb_write(&f.gb, 0xFF06, 0x88);
    CHECK_EQ(f.gb.timer.tma, 0x88);
    fixture_destroy(&f);
}

static void t_timer_timing_stays_right_with_a_busy_divider(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* Start the divider so bit 3 is already high: the falling edge is only
     * eight cycles away rather than a full sixteen. */
    f.gb.timer.tac = 0x05;
    f.gb.timer.tima = 0;
    f.gb.timer.div = 0x0008; /* bit 3 set, bits 0-2 clear */

    spin(&f.gb, 7);
    CHECK_EQ(f.gb.timer.tima, 0);
    spin(&f.gb, 1);
    CHECK_EQ(f.gb.timer.tima, 1);
    fixture_destroy(&f);
}

static void t_timer_running_stays_sane_over_a_frame(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int increments = 0;

    f.gb.timer.tac = 0x05; /* 262144 Hz */
    f.gb.timer.tima = 0;
    f.gb.timer.tma = 0;
    f.gb.timer.div = 0;
    f.gb.iflag = 0;

    for (int i = 0; i < GB_CYCLES_PER_FRAME; i++) {
        uint8_t before = f.gb.timer.tima;
        timer_tick(&f.gb, 1);
        if (f.gb.timer.tima != before)
            increments++; /* TIMA only ever moves on a falling edge */
    }

    /* 70224 T-cycles at 262144 Hz is 4389 TIMA steps. */
    CHECK_MSG(increments >= 4385 && increments <= 4392,
              "expected about 4389 TIMA steps in a frame, saw %d", increments);
    fixture_destroy(&f);
}

static void t_timer_reset_values(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.tac = 0x07;
    timer_reset(&f.gb);
    CHECK_EQ(f.gb.timer.tac, 0x00);
    CHECK_EQ(f.gb.timer.tima, 0x00);
    CHECK_EQ(f.gb.timer.tma, 0x00);
    CHECK_EQ(f.gb.timer.div, 0xABCC);
    fixture_destroy(&f);
}

static void t_timer_div_read_is_live(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.timer.div = 0x0A00;
    CHECK_EQ(gb_read(&f.gb, 0xFF04), 0x0A);
    spin(&f.gb, 256);
    CHECK_EQ(gb_read(&f.gb, 0xFF04), 0x0B);
    fixture_destroy(&f);
}

/*
 * The reference is explicit that writing to TAC, and to DIV, can step TIMA
 * on its own: the hardware latches the selected divider bit ANDed with the
 * enable, so clearing either one can drive that signal low and the edge
 * detector cannot tell that from the divider counting.
 */
static void t_timer_writing_div_can_tick_tima(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* The fastest clock, bit 9 of the divider.  Walk to a point where that
     * bit is high, then clear DIV and watch TIMA move. */
    gb_write(&f.gb, 0xFF07, 0x05); /* enable, every four M-cycles */
    gb_write(&f.gb, 0xFF06, 0x00);
    gb_write(&f.gb, 0xFF05, 0x00);

    uint8_t start = gb_read(&f.gb, 0xFF04);
    for (int i = 0; i < 600; i++) {
        gb_write(&f.gb, 0xFF05, 0x10); /* a known, non-zero TIMA */
        gb_tick(&f.gb, 4);
        uint8_t div = gb_read(&f.gb, 0xFF04);

        if (div != start) {
            gb_write(&f.gb, 0xFF04, 0x00);
            CHECK_MSG(gb_read(&f.gb, 0xFF05) == 0x11,
                      "clearing DIV at $%02X should have stepped TIMA to $11, "
                      "got $%02X", div, gb_read(&f.gb, 0xFF05));
            fixture_destroy(&f);
            return;
        }
    }

    FAIL("the divider never moved, so the check proved nothing");
    fixture_destroy(&f);
}

static void t_timer_writing_tac_can_tick_tima(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* Get the selected bit high with the timer running, then switch the
     * enable off: the ANDed signal falls and TIMA steps. */
    gb_write(&f.gb, 0xFF06, 0x00);
    gb_write(&f.gb, 0xFF07, 0x05);

    for (int i = 0; i < 600; i++) {
        gb_write(&f.gb, 0xFF05, 0x20);
        uint8_t div = gb_read(&f.gb, 0xFF04);

        if (div != 0) {
            gb_write(&f.gb, 0xFF07, 0x01); /* enable off */
            CHECK_MSG(gb_read(&f.gb, 0xFF05) == 0x21,
                      "turning TAC off at $%02X should have stepped TIMA to "
                      "$21, got $%02X", div, gb_read(&f.gb, 0xFF05));
            fixture_destroy(&f);
            return;
        }
    }

    FAIL("the divider never moved, so the check proved nothing");
    fixture_destroy(&f);
}

/* Turning the timer off must not tick it, only the divider free runs. */
static void t_timer_disabled_timer_only_counts(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint8_t tima;

    gb_write(&f.gb, 0xFF05, 0x30);
    gb_write(&f.gb, 0xFF07, 0x00); /* disabled */

    tima = gb_read(&f.gb, 0xFF05);
    for (int i = 0; i < 2000; i++)
        gb_tick(&f.gb, 4);

    CHECK_EQ(gb_read(&f.gb, 0xFF05), tima);
    CHECK_NE(gb_read(&f.gb, 0xFF04), 0x00); /* but DIV did move */
    fixture_destroy(&f);
}

const TestCase timer_tests[] = {
    { "timer", "writing div can tick tima",   t_timer_writing_div_can_tick_tima },
    { "timer", "writing tac can tick tima",   t_timer_writing_tac_can_tick_tima },
    { "timer", "disabled timer only counts", t_timer_disabled_timer_only_counts },
    { "timer", "div counts up", t_timer_div_counts_up },
    { "timer", "div uses the upper byte", t_timer_div_uses_the_upper_byte },
    { "timer", "div wraps after 65536", t_timer_div_wraps_after_65536 },
    { "timer", "div write resets", t_timer_div_write_resets },
    { "timer", "does not run while disabled", t_timer_does_not_run_while_disabled },
    { "timer", "tac frequency 0", t_timer_tac_frequency_0 },
    { "timer", "tac frequency 1", t_timer_tac_frequency_1 },
    { "timer", "tac frequency 2", t_timer_tac_frequency_2 },
    { "timer", "tac frequency 3", t_timer_tac_frequency_3 },
    { "timer", "overflow loads tma and interrupts", t_timer_overflow_loads_tma_and_raises_an_interrupt },
    { "timer", "overflow to zero keeps zero", t_timer_overflow_to_zero_keeps_zero },
    { "timer", "counts between overflows", t_timer_counts_between_overflows },
    { "timer", "tima is writable", t_timer_tima_is_writable },
    { "timer", "timing holds mid period", t_timer_timing_stays_right_with_a_busy_divider },
    { "timer", "stays sane over a frame", t_timer_running_stays_sane_over_a_frame },
    { "timer", "reset values", t_timer_reset_values },
    { "timer", "div read is live", t_timer_div_read_is_live },
    TEST_SUITE_END
};