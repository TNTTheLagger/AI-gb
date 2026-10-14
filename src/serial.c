/*
 * serial.c -- the $FF01/$FF02 link port.
 *
 * There is no cable attached, so a transfer started with the internal
 * clock simply shifts in the idle high line after eight bits and raises
 * the serial interrupt.  A transfer waiting on an external clock never
 * completes, exactly like real hardware with nobody on the other end.
 */
#include "gb.h"

/* One bit travels at 8192 Hz, which is 512 T-cycles. */
#define SERIAL_BIT_CYCLES 512
#define SERIAL_BITS 8

void serial_tick(GB *gb, int cycles)
{
    Serial *s = &gb->serial;

    if (!(s->sc & 0x80)) {
        s->counter = SERIAL_BITS * SERIAL_BIT_CYCLES;
        return;
    }

    if (!(s->sc & 0x01))
        return; /* external clock, waiting for a partner that never arrives */

    s->counter -= cycles;
    while (s->counter <= 0) {
        s->counter += SERIAL_BITS * SERIAL_BIT_CYCLES;
        s->sb = 0xFF; /* the idle line is pulled high */
        s->sc &= (uint8_t)~0x80;
        gb->iflag |= GB_INT_SERIAL;
        return;
    }
}