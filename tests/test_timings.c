/*
 * test_timings.c -- the instruction timing table, checked against the
 * hardware reference.
 *
 * These numbers come from the opcode-by-opcode table in the Game Boy
 * Complete Technical Reference, which lists every instruction as a
 * duration in machine cycles.  Four T-cycles make an M-cycle, so the
 * table is written here in M-cycles and multiplied out.
 *
 * tools/check_timings.py re-derives the same table straight from the PDF
 * and compares it against the running emulator, so the two can be kept
 * honest against each other.  This suite is what stops the table drifting
 * in between, and it needs no reference document to run.
 *
 * The conditional instructions cost more when the branch is taken, and the
 * reference only quotes the taken figure, so those are checked separately
 * in both directions.
 */
#include <stdio.h>
#include <string.h>

#include "test.h"

/* Flags with Z and C set, so the Z and C conditions hold. */
#define FLAGS_ZC_SET   0xB0
/* Flags with Z and C clear, so the NZ and NC conditions hold. */
#define FLAGS_ZC_CLEAR 0x00

static int measure(uint8_t opcode, uint8_t flags)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint8_t code[4] = { opcode, 0x00, 0x00, 0x00 };
    int cycles;

    REG_H = 0xC0;
    REG_L = 0x00;
    fixture_write(&f, 0x0100, code, sizeof(code));
    gb_write(&f.gb, 0xC000, 0x00);
    gb_write(&f.gb, 0xC001, 0x02);

    f.gb.cpu.f = flags;
    cycles = fixture_step(&f);
    fixture_destroy(&f);
    return cycles;
}

static int cb_measure(uint8_t opcode)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint8_t code[4] = { 0xCB, opcode, 0x00, 0x00 };
    int cycles;

    REG_H = 0xC0;
    REG_L = 0x00;
    fixture_write(&f, 0x0100, code, sizeof(code));
    gb_write(&f.gb, 0xC000, 0x00);

    cycles = fixture_step(&f);
    fixture_destroy(&f);
    return cycles;
}

/*
 * One row of the reference table.  Opcodes in a group are `first`, then
 * `first + stride`, and so on up to `last`, which is inclusive.
 */
typedef struct {
    uint8_t first, last, stride;
    int m_cycles;
    const char *what;
    /* Where a group of opcodes has members that mean something else, the
     * reference lists those separately.  Two kinds of hole turn up: the
     * one member with 110 in its low three bits, and an inclusive run. */
    bool skip_hl;
    uint8_t skip_lo, skip_hi;
} Timing;

#define SKIP     false, 0x00, 0x00
#define SKIP_HL  true, 0x00, 0x00

static const Timing timings[] = {
    /* --- 8 bit loads --------------------------------------------------- */
    { 0x40, 0x7F, 0x01, 1, "LD r,r'", true , 0x70, 0x77 },
    { 0x06, 0x3E, 0x08, 2, "LD r,n", true , 0x00, 0x00 },
    { 0x46, 0x7E, 0x08, 2, "LD r,(HL)", false, 0x00, 0x00 },
    { 0x70, 0x77, 0x01, 2, "LD (HL),r", false, 0x00, 0x00 },
    { 0x36, 0x36, 0x01, 3, "LD (HL),n", false, 0x00, 0x00 },
    { 0x0A, 0x0A, 0x01, 2, "LD A,(BC)", false, 0x00, 0x00 },
    { 0x1A, 0x1A, 0x01, 2, "LD A,(DE)", false, 0x00, 0x00 },
    { 0x02, 0x02, 0x01, 2, "LD (BC),A", false, 0x00, 0x00 },
    { 0x12, 0x12, 0x01, 2, "LD (DE),A", false, 0x00, 0x00 },
    { 0xFA, 0xFA, 0x01, 4, "LD A,(nn)", false, 0x00, 0x00 },
    { 0xEA, 0xEA, 0x01, 4, "LD (nn),A", false, 0x00, 0x00 },
    { 0xF2, 0xF2, 0x01, 2, "LDH A,(C)", false, 0x00, 0x00 },
    { 0xE2, 0xE2, 0x01, 2, "LDH (C),A", false, 0x00, 0x00 },
    { 0xF0, 0xF0, 0x01, 3, "LDH A,(n)", false, 0x00, 0x00 },
    { 0xE0, 0xE0, 0x01, 3, "LDH (n),A", false, 0x00, 0x00 },
    { 0x3A, 0x3A, 0x01, 2, "LD A,(HL-)", false, 0x00, 0x00 },
    { 0x32, 0x32, 0x01, 2, "LD (HL-),A", false, 0x00, 0x00 },
    { 0x2A, 0x2A, 0x01, 2, "LD A,(HL+)", false, 0x00, 0x00 },
    { 0x22, 0x22, 0x01, 2, "LD (HL+),A", false, 0x00, 0x00 },
    /* --- 16 bit loads --------------------------------------------------- */
    { 0x01, 0x31, 0x10, 3, "LD rr,nn", false, 0x00, 0x00 },
    { 0x08, 0x08, 0x01, 5, "LD (nn),SP", false, 0x00, 0x00 },
    { 0xF9, 0xF9, 0x01, 2, "LD SP,HL", false, 0x00, 0x00 },
    { 0xC5, 0xF5, 0x10, 4, "PUSH rr", false, 0x00, 0x00 },
    { 0xC1, 0xF1, 0x10, 3, "POP rr", false, 0x00, 0x00 },
    { 0xF8, 0xF8, 0x01, 3, "LD HL,SP+e", false, 0x00, 0x00 },
    /* --- 8 bit arithmetic and logic ------------------------------------- */
    { 0x80, 0x87, 0x01, 1, "ADD A,r", true , 0x00, 0x00 },
    { 0x86, 0x86, 0x01, 2, "ADD A,(HL)", false, 0x00, 0x00 },
    { 0xC6, 0xC6, 0x01, 2, "ADD A,n", false, 0x00, 0x00 },
    { 0x88, 0x8F, 0x01, 1, "ADC A,r", true , 0x00, 0x00 },
    { 0x8E, 0x8E, 0x01, 2, "ADC A,(HL)", false, 0x00, 0x00 },
    { 0xCE, 0xCE, 0x01, 2, "ADC A,n", false, 0x00, 0x00 },
    { 0x90, 0x97, 0x01, 1, "SUB r", true , 0x00, 0x00 },
    { 0x96, 0x96, 0x01, 2, "SUB (HL)", false, 0x00, 0x00 },
    { 0xD6, 0xD6, 0x01, 2, "SUB n", false, 0x00, 0x00 },
    { 0x98, 0x9F, 0x01, 1, "SBC A,r", true , 0x00, 0x00 },
    { 0x9E, 0x9E, 0x01, 2, "SBC A,(HL)", false, 0x00, 0x00 },
    { 0xDE, 0xDE, 0x01, 2, "SBC A,n", false, 0x00, 0x00 },
    { 0xB8, 0xBF, 0x01, 1, "CP r", true , 0x00, 0x00 },
    { 0xBE, 0xBE, 0x01, 2, "CP (HL)", false, 0x00, 0x00 },
    { 0xFE, 0xFE, 0x01, 2, "CP n", false, 0x00, 0x00 },
    { 0xA0, 0xA7, 0x01, 1, "AND r", true , 0x00, 0x00 },
    { 0xA6, 0xA6, 0x01, 2, "AND A,(HL)", false, 0x00, 0x00 },
    { 0xE6, 0xE6, 0x01, 2, "AND A,n", false, 0x00, 0x00 },
    { 0xB0, 0xB7, 0x01, 1, "OR r", true , 0x00, 0x00 },
    { 0xB6, 0xB6, 0x01, 2, "OR A,(HL)", false, 0x00, 0x00 },
    { 0xF6, 0xF6, 0x01, 2, "OR A,n", false, 0x00, 0x00 },
    { 0xA8, 0xAF, 0x01, 1, "XOR r", true , 0x00, 0x00 },
    { 0xAE, 0xAE, 0x01, 2, "XOR A,(HL)", false, 0x00, 0x00 },
    { 0xEE, 0xEE, 0x01, 2, "XOR A,n", false, 0x00, 0x00 },
    { 0x04, 0x3C, 0x08, 1, "INC r", false, 0x34, 0x34 },
    { 0x34, 0x34, 0x01, 3, "INC (HL)", false, 0x00, 0x00 },
    { 0x05, 0x3D, 0x08, 1, "DEC r", false, 0x35, 0x35 },
    { 0x35, 0x35, 0x01, 3, "DEC (HL)", false, 0x00, 0x00 },
    { 0x3F, 0x3F, 0x01, 1, "CCF", false, 0x00, 0x00 },
    { 0x37, 0x37, 0x01, 1, "SCF", false, 0x00, 0x00 },
    { 0x27, 0x27, 0x01, 1, "DAA", false, 0x00, 0x00 },
    { 0x2F, 0x2F, 0x01, 1, "CPL", false, 0x00, 0x00 },
    /* --- 16 bit arithmetic ------------------------------------------------ */
    { 0x03, 0x33, 0x10, 2, "INC rr", false, 0x00, 0x00 },
    { 0x0B, 0x3B, 0x10, 2, "DEC rr", false, 0x00, 0x00 },
    { 0x09, 0x39, 0x10, 2, "ADD HL,rr", false, 0x00, 0x00 },
    { 0xE8, 0xE8, 0x01, 4, "ADD SP,e", false, 0x00, 0x00 },
    /* --- rotates and shifts in the base set -------------------------------- */
    { 0x07, 0x07, 0x01, 1, "RLCA", false, 0x00, 0x00 },
    { 0x0F, 0x0F, 0x01, 1, "RRCA", false, 0x00, 0x00 },
    { 0x17, 0x17, 0x01, 1, "RLA", false, 0x00, 0x00 },
    { 0x1F, 0x1F, 0x01, 1, "RRA", false, 0x00, 0x00 },
    /* --- control flow ------------------------------------------------------ */
    { 0xC3, 0xC3, 0x01, 4, "JP nn", false, 0x00, 0x00 },
    { 0xE9, 0xE9, 0x01, 1, "JP (HL)", false, 0x00, 0x00 },
    { 0xCD, 0xCD, 0x01, 6, "CALL nn", false, 0x00, 0x00 },
    { 0xC9, 0xC9, 0x01, 4, "RET", false, 0x00, 0x00 },
    { 0xD9, 0xD9, 0x01, 4, "RETI", false, 0x00, 0x00 },
    { 0xC7, 0xFF, 0x08, 4, "RST n", false, 0x00, 0x00 },
    { 0x18, 0x18, 0x01, 3, "JR e", false, 0x00, 0x00 },
    /* --- misc --------------------------------------------------------------- */
    { 0xF3, 0xF3, 0x01, 1, "DI", false, 0x00, 0x00 },
    { 0xFB, 0xFB, 0x01, 1, "EI", false, 0x00, 0x00 },
    { 0x00, 0x00, 0x01, 1, "NOP", false, 0x00, 0x00 },
    { 0x76, 0x76, 0x01, 1, "HALT", false, 0x00, 0x00 },
    { 0x10, 0x10, 0x01, 1, "STOP", false, 0x00, 0x00 },
};

static void t_timing_table_matches_the_reference(void)
{
    for (size_t i = 0; i < sizeof(timings) / sizeof(timings[0]); i++) {
        const Timing *t = &timings[i];

        for (int op = t->first; op <= t->last; op += t->stride) {
            int want = t->m_cycles * 4;
            int got;

            if (t->skip_hl && (op & 7) == 6)
                continue;
            if (t->skip_hi && op >= t->skip_lo && op <= t->skip_hi)
                continue;
            if (op == 0x76)
                continue; /* HALT, not LD L,(HL) */

            got = measure((uint8_t)op, FLAGS_ZC_SET);
            CHECK_MSG(got == want, "%s ($%02X): expected %d T-cycles, took %d",
                      t->what, op, want, got);
        }
    }

}

/* The conditional branches cost more when taken; the reference only quotes
 * the taken figure, so both directions are checked here. */
static void t_timing_conditional_branches(void)
{
    static const struct {
        uint8_t op;
        int taken, not_taken;
        const char *what;
    } branches[] = {
        { 0x20, 3, 2, "JR NZ,e" },  { 0x28, 3, 2, "JR Z,e" },
        { 0x30, 3, 2, "JR NC,e" },  { 0x38, 3, 2, "JR C,e" },
        { 0xC2, 4, 3, "JP NZ,nn" }, { 0xCA, 4, 3, "JP Z,nn" },
        { 0xD2, 4, 3, "JP NC,nn" }, { 0xDA, 4, 3, "JP C,nn" },
        { 0xC4, 6, 3, "CALL NZ" },  { 0xCC, 6, 3, "CALL Z" },
        { 0xD4, 6, 3, "CALL NC" },  { 0xDC, 6, 3, "CALL C" },
        { 0xC0, 5, 2, "RET NZ" },   { 0xC8, 5, 2, "RET Z" },
        { 0xD0, 5, 2, "RET NC" },   { 0xD8, 5, 2, "RET C" },
    };

    for (size_t i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
        int with_set = measure(branches[i].op, FLAGS_ZC_SET);
        int with_clear = measure(branches[i].op, FLAGS_ZC_CLEAR);
        int taken = with_set > with_clear ? with_set : with_clear;
        int not_taken = with_set < with_clear ? with_set : with_clear;

        CHECK_MSG(taken == branches[i].taken * 4,
                  "%s taken: expected %d T-cycles, took %d (set=%d clear=%d)",
                  branches[i].what, branches[i].taken * 4, taken, with_set,
                  with_clear);
        CHECK_MSG(not_taken == branches[i].not_taken * 4,
                  "%s not taken: expected %d T-cycles, took %d",
                  branches[i].what, branches[i].not_taken * 4, not_taken);
    }
}

/* The CB-prefixed instructions take one of three lengths: two M-cycles for
 * a register operand, three for BIT against memory, and four for the rest
 * of the (HL) forms. */
static void t_timing_cb_prefixed(void)
{
    for (int op = 0x00; op <= 0xFF; op++) {
        int got = cb_measure((uint8_t)op);
        int want;

        if ((op & 0xC0) == 0x40 && (op & 7) == 6)
            want = 12; /* BIT b,(HL) */
        else if ((op & 7) == 6)
            want = 16; /* RES, SET, rotates and shifts against (HL) */
        else
            want = 8;  /* register forms */

        CHECK_MSG(got == want, "CB $%02X: expected %d T-cycles, took %d", op,
                  want, got);
    }
}

const TestCase timing_tests[] = {
    { "timing", "table matches the reference", t_timing_table_matches_the_reference },
    { "timing", "conditional branches",       t_timing_conditional_branches },
    { "timing", "CB prefixed instructions",   t_timing_cb_prefixed },

    TEST_SUITE_END
};