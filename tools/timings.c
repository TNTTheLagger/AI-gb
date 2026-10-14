/*
 * timings.c -- prints what every opcode actually costs, so the numbers can
 * be checked against the hardware reference.
 *
 * The conditional instructions cost different amounts depending on
 * whether the branch is taken, so every opcode is run twice: once with
 * the flags set and once with them clear.  The checker then knows that the
 * larger of the two is the taken cost and can compare that with the
 * document, which only lists the taken figure.
 *
 * Prints "<opcode in hex> <flags in hex> <T-cycles>" per line.  CB
 * prefixed opcodes are reported as 0x100 plus the opcode number.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb.h"
#include "test.h"

/* Both conditions true: Z and C set, so Z and C branch. */
#define FLAGS_ZC_SET   0xB0
/* Both conditions false: Z and C clear, so NZ and NC branch.  The two
 * settings between them make every conditional instruction take exactly
 * once, which is what lets the checker compare against the document's
 * taken figure. */
#define FLAGS_ZC_CLEAR 0x00

static void run(Fixture *f, const uint8_t *code, size_t len, int opcode,
                uint8_t flags)
{
    int cycles;

    fixture_write(f, 0x0100, code, len);
    gb_reset(&f->gb);
    f->gb.cpu.pc = 0x0100;
    f->gb.cpu.sp = 0xFFFE;
    f->gb.cpu.f = flags;
    f->gb.ie = 0;
    /* Point the register and memory operands somewhere safe: no cartridge
     * RAM, no OAM DMA, no interrupts, and a stack that will not run away. */
    f->gb.cpu.h = 0x00;
    f->gb.cpu.l = 0xC0; /* HL -> 0xC000, plain work RAM */
    f->gb.wram[0x0000] = 0x00;
    f->gb.wram[0x0001] = 0x02;

    cycles = fixture_step(f);
    printf("%X %02X %d\n", opcode, flags, cycles);
}

int main(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    static const uint8_t nops[2] = { 0x00, 0x00 };

    /* Two NOPs after every program, so an instruction that falls through
     * lands on something harmless rather than running away. */
    fixture_write(&f, 0x0200, nops, sizeof(nops));

    for (int op = 0x00; op <= 0xFF; op++) {
        uint8_t code[3] = { (uint8_t)op, 0x00, 0x00 };

        run(&f, code, sizeof(code), op, FLAGS_ZC_SET);
        run(&f, code, sizeof(code), op, FLAGS_ZC_CLEAR);
    }

    for (int op = 0x00; op <= 0xFF; op++) {
        uint8_t code[4] = { 0xCB, (uint8_t)op, 0x00, 0x00 };

        run(&f, code, sizeof(code), 0x100 + op, FLAGS_ZC_SET);
        run(&f, code, sizeof(code), 0x100 + op, FLAGS_ZC_CLEAR);
    }

    fixture_destroy(&f);
    return 0;
}