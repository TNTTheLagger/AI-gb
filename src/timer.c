/*
 * timer.c -- DIV / TIMA / TMA / TAC.
 *
 * The divider is modelled the way the hardware does it: a free running
 * 16 bit counter advances once per T-cycle and DIV is simply its upper
 * byte.  TAC taps one of its bits (9, 3, 5 or 7) and TIMA is incremented
 * on the falling edge of that bit.  This reproduces the behaviour games
 * actually rely on, such as the TIMA reload taking four cycles.
 */
#include "gb.h"

#define TAC_ENABLE 0x04

/*
 * The signal the falling edge detector actually watches.  It is the
 * selected divider bit ANDed with the enable, because that is the gate the
 * hardware latches; writing either DIV or TAC can change what this returns
 * and so can produce an edge out of nothing.
 */
static inline uint8_t timer_signal(const Timer *t)
{
    uint8_t bit;

    if (!(t->tac & TAC_ENABLE))
        return 0;

    switch (t->tac & 0x03) {
    case 0:  bit = (uint8_t)((t->div >> 9) & 1); break;
    case 1:  bit = (uint8_t)((t->div >> 3) & 1); break;
    case 2:  bit = (uint8_t)((t->div >> 5) & 1); break;
    default: bit = (uint8_t)((t->div >> 7) & 1); break;
    }
    return bit;
}

/* One falling edge of that signal, which is what steps TIMA. */
static void timer_edge(GB *gb)
{
    Timer *t = &gb->timer;

    t->tima++;
    if (t->tima == 0x00) {
        t->tima = t->tma;
        gb->iflag |= GB_INT_TIMER;
    }
}

void timer_tick(GB *gb, int cycles)
{
    Timer *t = &gb->timer;

    if (cycles <= 0)
        return;

    for (int i = 0; i < cycles; i++) {
        uint8_t before = timer_signal(t);

        t->div++;
        t->sample = timer_signal(t);

        if (before && !t->sample)
            timer_edge(gb);
    }
}

void timer_write(GB *gb, uint16_t addr, uint8_t value)
{
    Timer *t = &gb->timer;

    switch (addr) {
    case 0xFF04: /* DIV */
        /* Clearing the divider changes the bit the edge detector watches,
         * so a write can tick TIMA on its own.  The reference calls this
         * out as "writing to this register may increase TIMA once". */
        {
            uint8_t before = timer_signal(t);

            t->div = 0;
            if (before && !timer_signal(t))
                timer_edge(gb);
        }
        break;

    case 0xFF05: /* TIMA */
        t->tima = value;
        break;

    case 0xFF06: /* TMA */
        t->tma = value;
        break;

    case 0xFF07: /* TAC */
        /* Same story: switching the enable bit off while the selected
         * divider bit is high drives the signal low, and that is an edge. */
        {
            uint8_t before = timer_signal(t);

            t->tac = (uint8_t)(value & 0x07);
            if (before && !timer_signal(t))
                timer_edge(gb);
        }
        break;

    default:
        break;
    }
}

void timer_reset(GB *gb)
{
    Timer *t = &gb->timer;

    /* Register values the boot ROM leaves behind. */
    t->div = 0xABCC;
    t->tima = 0x00;
    t->tma = 0x00;
    t->tac = 0x00;
    t->sample = 0;
}

uint8_t timer_div(const GB *gb)
{
    return (uint8_t)(gb->timer.div >> 8);
}