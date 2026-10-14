/*
 * test_cpu.c -- the Sharp LR35902 core.
 */
#include "test.h"

/* ------------------------------------------------------------------ */
/* 8 bit loads and register moves                                      */
/* ------------------------------------------------------------------ */

static void t_cpu_nop_consumes_four_cycles(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x00, 0x00, 0x00 };
    int cycles = 0;

    fixture_run(&f, code, sizeof(code), 3);
    cycles = f.gb.cycles;

    CHECK_EQ(REG_PC, 0x0103);
    CHECK_EQ(cycles, 12);
    fixture_destroy(&f);
}

static void t_cpu_ld_bc_immediate(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x01, 0x34, 0x12 }; /* LD BC,$1234 */

    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(BC(f), 0x1234);
    CHECK_EQ(REG_PC, 0x0103);
    fixture_destroy(&f);
}

static void t_cpu_ld_pair_round_trip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x21, 0x00, 0xC0,       /* LD HL,$C000   */
        0x36, 0x5A,             /* LD (HL),$5A   */
        0x7E,                   /* LD A,(HL)     */
        0x22,                   /* LD (HL+),A    */
        0x36, 0x77,             /* LD (HL),$77   */
        0x2A                    /* LD A,(HL+)    */
    };

    fixture_run(&f, code, sizeof(code), 6);
    CHECK_EQ(REG_A, 0x77);    /* the auto increment walked to the next byte */
    CHECK_EQ(HL(f), 0xC002);
    CHECK_EQ(gb_read(&f.gb, 0xC000), 0x5A);
    CHECK_EQ(gb_read(&f.gb, 0xC001), 0x77);
    fixture_destroy(&f);
}

static void t_cpu_ld_r_from_r(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x3E, 0x11,             /* LD A,$11 */
        0x06, 0x22,             /* LD B,$22 */
        0x78,                   /* LD A,B   */
        0x4F,             /* LD C,A   */
        0x79                    /* LD A,C   */
    };

    fixture_run(&f, code, sizeof(code), 5);
    CHECK_EQ(REG_A, 0x22);
    CHECK_EQ(REG_C, 0x22);
    fixture_destroy(&f);
}

static void t_cpu_ldh_uses_high_page(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x3E, 0x5A,             /* LD A,$5A           */
        0xE0, 0x80,             /* LDH ($80),A -> $FF80 (HRAM) */
        0xF0, 0x80,             /* LDH A,($80)        */
        0x4F                    /* LD C,A             */
    };

    fixture_run(&f, code, sizeof(code), 4);
    CHECK_EQ(gb_read(&f.gb, 0xFF80), 0x5A);
    CHECK_EQ(REG_C, 0x5A);
    fixture_destroy(&f);
}

static void t_cpu_ld_c_addresses_high_page(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x0E, 0x90,             /* LD C,$90           */
        0x3E, 0x42,             /* LD A,$42           */
        0xE2,                   /* LD ($FF00+C),A     */
        0xF2                    /* LD A,($FF00+C)     */
    };

    fixture_run(&f, code, sizeof(code), 4);
    CHECK_EQ(gb_read(&f.gb, 0xFF90), 0x42);
    CHECK_EQ(REG_A, 0x42);
    fixture_destroy(&f);
}

static void t_cpu_ld_a16_sp_is_little_endian(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x31, 0x34, 0x12,       /* LD SP,$1234   */
        0x08, 0x00, 0xC0        /* LD ($C000),SP */
    };

    fixture_run(&f, code, sizeof(code), 2);
    CHECK_EQ(gb_read(&f.gb, 0xC000), 0x34);
    CHECK_EQ(gb_read(&f.gb, 0xC001), 0x12);
    fixture_destroy(&f);
}

static void t_cpu_push_pop_af_masks_low_nibble(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x31, 0x00, 0xD0,       /* LD SP,$D000      */
        0x3E, 0x12,             /* LD A,$12         */
        0xF5,                   /* PUSH AF          */
        0xF1,                   /* POP AF           */
        0x87,                   /* ADD A,A          */
        0x4F                    /* LD C,A           */
    };

    /* The F register can only ever hold the top four bits. */
    fixture_run(&f, code, sizeof(code), 6);
    CHECK_EQ(REG_F & 0x0F, 0);
    CHECK_EQ(REG_A, 0x24);
    CHECK_EQ(REG_C, 0x24);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* INC and DEC                                                         */
/* ------------------------------------------------------------------ */

static void t_cpu_inc_sets_zero_and_half(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x3C, 0x3C, 0x3C }; /* INC A x3 from 0xFD */

    REG_A = 0xFD;
    REG_F = FC; /* INC must leave carry alone */
    fixture_run(&f, code, sizeof(code), 3);

    CHECK_EQ(REG_A, 0x00);
    CHECK_EQ(REG_F & FZ, FZ);
    CHECK_EQ(REG_F & FH, FH); /* the last step went from $FF to $00 */
    CHECK_EQ(REG_F & FN, 0);
    CHECK_EQ(REG_F & FC, FC); /* preserved */
    fixture_destroy(&f);
}

static void t_cpu_inc_half_carry_from_low_nibble(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x04 }; /* INC B with B = $0F */

    REG_B = 0x0F;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_B, 0x10);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FZ, 0);
    fixture_destroy(&f);
}

static void t_cpu_dec_sets_negative_and_half(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x0D }; /* DEC C with C = $10 */

    REG_C = 0x10;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_C, 0x0F);
    CHECK_EQ(REG_F & FN, FN);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FZ, 0);
    fixture_destroy(&f);
}

static void t_cpu_inc_hl_reads_and_writes_memory(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x21, 0x00, 0xC0,       /* LD HL,$C000  */
        0x36, 0xFF,             /* LD (HL),$FF  */
        0x34                    /* INC (HL)     */
    };

    fixture_run(&f, code, sizeof(code), 3);
    CHECK_EQ(gb_read(&f.gb, 0xC000), 0x00);
    CHECK_EQ(REG_F & FZ, FZ);
    CHECK_EQ(REG_F & FH, FH);
    fixture_destroy(&f);
}

static void t_cpu_inc_bc_leaves_flags_alone(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x03, 0x0B }; /* INC BC, DEC BC */

    REG_F = 0x00;
    fixture_run(&f, code, sizeof(code), 2);
    CHECK_EQ(BC(f), 0x0000);
    CHECK_EQ(REG_F, 0x00);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Arithmetic                                                           */
/* ------------------------------------------------------------------ */

static void t_cpu_add_sets_half_and_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x80 }; /* ADD A,B */

    REG_A = 0xF0;
    REG_B = 0x20;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_A, 0x10);
    CHECK_EQ(REG_F & FH, 0);  /* 0 + 0 does not carry the low nibble */
    CHECK_EQ(REG_F & FC, FC);
    CHECK_EQ(REG_F & FN, 0);
    CHECK_EQ(REG_F & FZ, 0);
    fixture_destroy(&f);
}

static void t_cpu_add_half_carry_boundary(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xC6, 0x01 }; /* ADD A,$01 */

    REG_A = 0x0F;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_A, 0x10);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_add_zero_result_sets_z(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x87 }; /* ADD A,A with A = 0x80 */

    REG_A = 0x80;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_A, 0x00);
    CHECK_EQ(REG_F & FZ, FZ);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_adc_includes_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x37, 0x8E }; /* SCF, ADC A,(HL) */

    REG_A = 0x0F;
    REG_H = 0xC0;
    REG_L = 0x00;
    gb_write(&f.gb, 0xC000, 0x01);

    fixture_run(&f, code, sizeof(code), 2);
    CHECK_EQ(REG_A, 0x11);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_sub_sets_negative_and_borrow(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x90 }; /* SUB B */

    REG_A = 0x20;
    REG_B = 0x30;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_A, 0xF0);
    CHECK_EQ(REG_F & FN, FN);
    CHECK_EQ(REG_F & FH, 0);  /* 0x0 - 0x0 does not borrow the low nibble */
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_sub_half_borrow(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x90 }; /* SUB B, both low nibbles $3 and $8 */

    REG_A = 0x23;
    REG_B = 0x18;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_A, 0x0B);
    CHECK_EQ(REG_F & FH, FH); /* 3 cannot cover 8, so the nibble borrows */
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_sub_equal_sets_zero(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xD6, 0x40 }; /* SUB $40 */

    REG_A = 0x40;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_A, 0x00);
    CHECK_EQ(REG_F & FZ, FZ);
    CHECK_EQ(REG_F & FC, 0);
    CHECK_EQ(REG_F & FN, FN);
    fixture_destroy(&f);
}

static void t_cpu_sbc_with_borrow(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x37, 0xDE, 0x01 }; /* SCF, SBC A,$01 */

    REG_A = 0x05;
    fixture_run(&f, code, sizeof(code), 2);

    /* 5 - 1 - 1 = 3 */
    CHECK_EQ(REG_A, 0x03);
    CHECK_EQ(REG_F & FN, FN);
    fixture_destroy(&f);
}

static void t_cpu_and_or_xor(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xE6, 0x0F, 0xEE, 0xFF, 0xF6, 0x30 }; /* AND/OR/XOR */

    REG_A = 0xF0;
    fixture_run(&f, code, sizeof(code), 3);

    /* 0xF0 AND 0x0F = 0x00, XOR 0xFF = 0xFF, OR 0x30 = 0xFF */
    CHECK_EQ(REG_A, 0xFF);
    CHECK_EQ(REG_F & FZ, 0);
    fixture_destroy(&f);
}

static void t_cpu_and_sets_half_and_zero(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xE6, 0x00 };

    REG_A = 0x99;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_A, 0x00);
    CHECK_EQ(REG_F & FZ, FZ);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FN, 0);
    fixture_destroy(&f);
}

static void t_cpu_cp_does_not_store(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xFE, 0x10 }; /* CP $10 */

    REG_A = 0x10;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_A, 0x10);
    CHECK_EQ(REG_F & FZ, FZ);
    CHECK_EQ(REG_F & FN, FN);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* DAA and the misc ALU opcodes                                         */
/* ------------------------------------------------------------------ */

static void t_cpu_daa_after_bcd_add(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x3E, 0x45, 0xC6, 0x38, 0x27 }; /* 45 + 38, DAA */

    fixture_run(&f, code, sizeof(code), 3);
    CHECK_EQ(REG_A, 0x83);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_daa_after_bcd_add_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x3E, 0x83, 0xC6, 0x79, 0x27 }; /* 83 + 79, DAA */

    fixture_run(&f, code, sizeof(code), 3);
    CHECK_EQ(REG_A, 0x62);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_daa_after_bcd_subtract(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x3E, 0x83, 0xD6, 0x79, 0x27 }; /* 83 - 79, DAA */

    fixture_run(&f, code, sizeof(code), 3);
    CHECK_EQ(REG_A, 0x04);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_daa_leaves_a_small_digit_alone(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x3E, 0x09, 0xC6, 0x00, 0x27 }; /* 0x09 + 0x00 */

    fixture_run(&f, code, sizeof(code), 3);
    CHECK_EQ(REG_A, 0x09); /* nothing to correct */
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_daa_corrects_a_hex_half_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x3E, 0x0C, 0xC6, 0x0F, 0x27 }; /* 0x0C + 0x0F */

    fixture_run(&f, code, sizeof(code), 3);
    /* 0x0C + 0x0F = 0x1B with a half carry, so DAA adds $06 -> $21 */
    CHECK_EQ(REG_A, 0x21);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_cpl(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x2F }; /* CPL */

    REG_A = 0x35;
    REG_F = 0x00;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_A, 0xCA);
    CHECK_EQ(REG_F & FN, FN);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FZ, 0); /* CPL leaves Z alone */
    fixture_destroy(&f);
}

static void t_cpu_scf_then_ccf(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x37, 0x3F }; /* SCF, CCF */

    REG_F = 0;
    fixture_run(&f, code, sizeof(code), 2);

    /* SCF clears N and H and sets the carry; CCF then flips that to 0. */
    CHECK_EQ(REG_F & FN, 0);
    CHECK_EQ(REG_F & FH, 0);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_scf_sets_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x37 }; /* SCF */

    REG_F = FN | FH;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_F & FC, FC);
    CHECK_EQ(REG_F & FN, 0);
    CHECK_EQ(REG_F & FH, 0);
    fixture_destroy(&f);
}

static void t_cpu_ccf_flips_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x37, 0x3F }; /* SCF then CCF */

    REG_F = 0;
    fixture_run(&f, code, sizeof(code), 2);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_rotates_clear_zero(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x07 }; /* RLCA with A = 0 */

    REG_A = 0x00;
    REG_F = FZ;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_A, 0x00);
    CHECK_EQ(REG_F & FZ, 0); /* the A rotates never set Z */
    fixture_destroy(&f);
}

static void t_cpu_rla_uses_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x37, 0x17 }; /* SCF then RLA */

    REG_A = 0x80;
    fixture_run(&f, code, sizeof(code), 2);

    /* 0x80 shifted left, carry in is 1, and the old bit 7 becomes carry */
    CHECK_EQ(REG_A, 0x01);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_rra_uses_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x37, 0x1F }; /* SCF then RRA */

    REG_A = 0x01;
    fixture_run(&f, code, sizeof(code), 2);

    CHECK_EQ(REG_A, 0x80);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* 16 bit arithmetic                                                    */
/* ------------------------------------------------------------------ */

static void t_cpu_add_hl_flags(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x21, 0xFF, 0x0F,       /* LD HL,$0FFF  */
        0x01, 0x01, 0x00,       /* LD BC,$0001  */
        0x09                    /* ADD HL,BC   */
    };

    REG_F = 0xF0; /* every flag set going in */
    fixture_run(&f, code, sizeof(code), 3);

    CHECK_EQ(HL(f), 0x1000);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FC, 0);
    CHECK_EQ(REG_F & FN, 0);
    CHECK_EQ(REG_F & FZ, FZ); /* ADD HL never touches Z */
    fixture_destroy(&f);
}

static void t_cpu_add_hl_carry_out(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x21, 0x00, 0xF0,       /* LD HL,$F000 */
        0x01, 0x00, 0x20,       /* LD BC,$2000 */
        0x09                    /* ADD HL,BC  */
    };

    fixture_run(&f, code, sizeof(code), 3);
    CHECK_EQ(HL(f), 0x1000);
    CHECK_EQ(REG_F & FC, FC);
    CHECK_EQ(REG_F & FH, 0);
    fixture_destroy(&f);
}

static void t_cpu_add_sp_signed(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xE8, 0xFE }; /* ADD SP,-2 */

    fixture_load(&f, code, sizeof(code));
    REG_SP = 0x0100;
    fixture_exec(&f, 1);

    CHECK_EQ(REG_SP, 0x00FE);
    CHECK_EQ(REG_F & FN, 0);
    CHECK_EQ(REG_F & FZ, 0); /* ADD SP always clears Z */
    fixture_destroy(&f);
}

static void t_cpu_add_sp_half_carry_uses_unsigned(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xF8, 0x01 }; /* LD HL,SP+1 */

    fixture_load(&f, code, sizeof(code));
    REG_SP = 0x000F;
    fixture_exec(&f, 1);

    CHECK_EQ(HL(f), 0x0010);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FC, 0);
    fixture_destroy(&f);
}

static void t_cpu_ld_hl_sp_signed(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xF8, 0x80 }; /* LD HL,SP-128 */

    fixture_load(&f, code, sizeof(code));
    REG_SP = 0xD000;
    fixture_exec(&f, 1);

    CHECK_EQ(HL(f), 0xCF80);
    CHECK_EQ(REG_SP, 0xD000); /* SP itself is untouched */
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Control flow                                                         */
/* ------------------------------------------------------------------ */

static void t_cpu_jp_absolute(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xC3, 0x50, 0x01 }; /* JP $0150 */

    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_PC, 0x0150);
    fixture_destroy(&f);
}

static void t_cpu_jp_hl(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xE9 }; /* JP (HL) */

    REG_H = 0x03;
    REG_L = 0x21;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_PC, 0x0321);
    fixture_destroy(&f);
}

static void t_cpu_conditional_jp_cycle_counts(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0xC2, 0x50, 0x01,       /* JP NZ,$0150  */
        0xCA, 0x60, 0x01        /* JP Z,$0160   */
    };

    fixture_load(&f, code, sizeof(code));

    REG_F = 0x00; /* Z clear: the first jump is taken */
    CHECK_EQ(fixture_step(&f), 16);
    CHECK_EQ(REG_PC, 0x0150);

    fixture_load(&f, code, sizeof(code));
    REG_F = FZ;
    REG_PC = 0x0103;
    CHECK_EQ(fixture_step(&f), 16);
    CHECK_EQ(REG_PC, 0x0160);

    /* Not taken: only the operand fetch is paid for. */
    fixture_load(&f, code, sizeof(code));
    REG_F = 0x00;
    REG_PC = 0x0103;
    CHECK_EQ(fixture_step(&f), 12);
    CHECK_EQ(REG_PC, 0x0106);
    fixture_destroy(&f);
}

static void t_cpu_jr_relative(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x18, 0x7F }; /* JR +127 */

    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_PC, 0x0102 + 0x7F);
    fixture_destroy(&f);
}

static void t_cpu_jr_negative_offset(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x18, 0xFE }; /* JR -2 */

    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_PC, 0x0100); /* jumps back to itself */
    fixture_destroy(&f);
}

static void t_cpu_conditional_jr_not_taken_cycles(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x20, 0x10 }; /* JR NZ,+16 */

    fixture_load(&f, code, sizeof(code));

    REG_F = FZ;
    CHECK_EQ(fixture_step(&f), 8);
    CHECK_EQ(REG_PC, 0x0102);

    fixture_load(&f, code, sizeof(code));
    REG_F = 0x00;
    CHECK_EQ(fixture_step(&f), 12);
    CHECK_EQ(REG_PC, 0x0112);
    fixture_destroy(&f);
}

static void t_cpu_call_and_ret(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCD, 0x50, 0x01 }; /* CALL $0150 */

    fixture_load(&f, code, sizeof(code));
    REG_SP = 0xFFFE;
    CHECK_EQ(fixture_step(&f), 24);
    CHECK_EQ(REG_PC, 0x0150);
    CHECK_EQ(REG_SP, 0xFFFC);
    CHECK_EQ(gb_read(&f.gb, 0xFFFC), 0x03); /* return address, low byte  */
    CHECK_EQ(gb_read(&f.gb, 0xFFFD), 0x01); /* high byte                 */

    fixture_load(&f, code, sizeof(code));
    REG_SP = 0xFFFE;
    fixture_step(&f);
    /* $0150 is a NOP, so patch a RET in before coming back. */
    fixture_byte(&f, 0x0150, 0xC9);
    CHECK_EQ(fixture_step(&f), 16);
    CHECK_EQ(REG_PC, 0x0103);
    CHECK_EQ(REG_SP, 0xFFFE);
    fixture_destroy(&f);
}

static void t_cpu_conditional_call_not_taken(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xC4, 0x50, 0x01 }; /* CALL NZ,$0150 */

    fixture_load(&f, code, sizeof(code));
    REG_F = FZ;
    REG_SP = 0xFFFE;
    CHECK_EQ(fixture_step(&f), 12);
    CHECK_EQ(REG_PC, 0x0103);
    CHECK_EQ(REG_SP, 0xFFFE); /* nothing was pushed */
    fixture_destroy(&f);
}

static void t_cpu_rst_pushes_and_jumps(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xFF }; /* RST $38 */

    fixture_load(&f, code, sizeof(code));
    REG_SP = 0xFFFE;
    CHECK_EQ(fixture_step(&f), 16);
    CHECK_EQ(REG_PC, 0x0038);
    CHECK_EQ(REG_SP, 0xFFFC);
    CHECK_EQ(gb_read(&f.gb, 0xFFFC), 0x01);
    CHECK_EQ(gb_read(&f.gb, 0xFFFD), 0x01);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* $CB prefix                                                           */
/* ------------------------------------------------------------------ */

static void t_cpu_cb_rlc(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x00 }; /* RLC B */

    REG_B = 0x85;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_B, 0x0B);
    CHECK_EQ(REG_F & FC, FC);
    CHECK_EQ(REG_F & FZ, 0);
    CHECK_EQ(REG_F & FN, 0);
    CHECK_EQ(REG_F & FH, 0);
    fixture_destroy(&f);
}

static void t_cpu_cb_rrc(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x09 }; /* RRC C */

    REG_C = 0x01;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_C, 0x80);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_cb_rl_uses_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x12 }; /* RL D  */

    REG_D = 0x80;
    REG_F = FC;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_D, 0x01);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_cb_rr_uses_carry(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x1B }; /* RR E  */

    REG_E = 0x01;
    REG_F = FC;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_E, 0x80);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_cb_sla(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x24 }; /* SLA H */

    REG_H = 0x81;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_H, 0x02);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_cb_sra_keeps_sign(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x2D }; /* SRA L */

    REG_L = 0x81;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_L, 0xC0); /* bit 7 survives */
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_cb_srl(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x3F }; /* SRL A */

    REG_A = 0x81;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_A, 0x40);
    CHECK_EQ(REG_F & FC, FC);
    fixture_destroy(&f);
}

static void t_cpu_cb_swap(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x30 }; /* SWAP B */

    REG_B = 0x12;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_B, 0x21);
    CHECK_EQ(REG_F & FC, 0);
    CHECK_EQ(REG_F & FH, 0);
    CHECK_EQ(REG_F & FN, 0);
    fixture_destroy(&f);
}

static void t_cpu_cb_bit_sets_flags_without_storing(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x40 }; /* BIT 0,B */

    REG_B = 0xFE;
    fixture_run(&f, code, sizeof(code), 1);

    CHECK_EQ(REG_B, 0xFE); /* BIT never changes the operand */
    CHECK_EQ(REG_F & FZ, FZ);
    CHECK_EQ(REG_F & FH, FH);
    CHECK_EQ(REG_F & FN, 0);
    fixture_destroy(&f);
}

static void t_cpu_cb_bit_set(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x78 }; /* BIT 7,B */

    REG_B = 0x80;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_F & FZ, 0);
    fixture_destroy(&f);
}

static void t_cpu_cb_res_and_set(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x80, 0xCB, 0xC0 }; /* RES 0,B / SET 0,B */

    REG_B = 0xFF;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(REG_B, 0xFE); /* RES cleared bit 0 */

    fixture_run(&f, code, sizeof(code), 2);
    CHECK_EQ(REG_B, 0xFF); /* SET put it back */
    fixture_destroy(&f);
}

static void t_cpu_cb_on_memory(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x21, 0x00, 0xC0,       /* LD HL,$C000  */
        0xCB, 0x06              /* RLC (HL)     */
    };

    gb_write(&f.gb, 0xC000, 0x01);
    fixture_run(&f, code, sizeof(code), 2);
    CHECK_EQ(gb_read(&f.gb, 0xC000), 0x02);
    fixture_destroy(&f);
}

static void t_cpu_cb_bit_memory_cycles(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xCB, 0x46 }; /* BIT 0,(HL) */

    fixture_load(&f, code, sizeof(code));
    REG_H = 0xC0;
    REG_L = 0x00;
    gb_write(&f.gb, 0xC000, 0x02);
    CHECK_EQ(fixture_step(&f), 12);
    CHECK_EQ(REG_F & FZ, FZ);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                           */
/* ------------------------------------------------------------------ */

static void t_cpu_interrupt_is_taken_when_enabled(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    REG_SP = 0xFFFE;
    REG_PC = 0x0200;
    f.gb.cpu.ime = true;
    f.gb.ie = GB_INT_VBLANK;
    f.gb.iflag = GB_INT_VBLANK;

    CHECK_EQ(fixture_step(&f), 20);
    CHECK_MSG(REG_PC == 0x0040,
              "VBlank is flag bit 0 and its vector is $0040, not $0048");
    CHECK_EQ(f.gb.iflag & GB_INT_VBLANK, 0);
    CHECK_EQ(f.gb.cpu.ime, false);        /* hardware clears IME */
    CHECK_EQ(REG_SP, 0xFFFC);
    CHECK_EQ(gb_read(&f.gb, 0xFFFC), 0x00); /* pushed PC low  */
    CHECK_EQ(gb_read(&f.gb, 0xFFFD), 0x02); /* pushed PC high */
    fixture_destroy(&f);
}

static void t_cpu_interrupt_ignored_when_disabled(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    REG_PC = 0x0200;
    f.gb.cpu.ime = false;
    f.gb.ie = GB_INT_VBLANK;
    f.gb.iflag = GB_INT_VBLANK;

    CHECK_EQ(fixture_step(&f), 4);
    CHECK_EQ(REG_PC, 0x0201);
    CHECK_EQ(f.gb.iflag & GB_INT_VBLANK, GB_INT_VBLANK); /* still pending */
    fixture_destroy(&f);
}

static void t_cpu_interrupt_priority_order(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    REG_SP = 0xFFFE;
    REG_PC = 0x0200;
    f.gb.cpu.ime = true;
    f.gb.ie = GB_INT_VBLANK | GB_INT_TIMER;
    /* Both pending: VBlank has the lower bit so it wins. */
    f.gb.iflag = GB_INT_VBLANK | GB_INT_TIMER;

    fixture_step(&f);
    CHECK_EQ(REG_PC, 0x0040);
    CHECK_EQ(f.gb.iflag & GB_INT_VBLANK, 0);
    CHECK_EQ(f.gb.iflag & GB_INT_TIMER, GB_INT_TIMER);
    fixture_destroy(&f);
}

static void t_cpu_ei_is_delayed_by_one_instruction(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xFB, 0x00, 0x00 }; /* EI, NOP, NOP */

    fixture_load(&f, code, sizeof(code));
    f.gb.cpu.ime = false;

    fixture_step(&f); /* EI: IME must still be off on the very next step */
    CHECK_EQ(f.gb.cpu.ime, false);
    CHECK_EQ(f.gb.cpu.ime_delay, 1);

    fixture_step(&f); /* the NOP completes, only now does IME come on */
    CHECK_EQ(f.gb.cpu.ime, true);
    CHECK_EQ(f.gb.cpu.ime_delay, 0);
    fixture_destroy(&f);
}

static void t_cpu_ei_then_di_blocks_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xFB, 0xF3, 0x00 }; /* EI, DI, NOP */

    fixture_run(&f, code, sizeof(code), 3);
    f.gb.ie = GB_INT_VBLANK;
    f.gb.iflag = GB_INT_VBLANK;
    fixture_exec(&f, 0);
    /* Re-run with the flags in place. */
    fixture_load(&f, code, sizeof(code));
    f.gb.ie = GB_INT_VBLANK;
    f.gb.iflag = GB_INT_VBLANK;
    fixture_exec(&f, 3);
    CHECK_EQ(f.gb.cpu.ime, false);
    CHECK_EQ(REG_PC, 0x0103); /* no interrupt was serviced */
    CHECK_EQ(f.gb.iflag & GB_INT_VBLANK, GB_INT_VBLANK);
    fixture_destroy(&f);
}

static void t_cpu_reti_enables_immediately(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0xD9 };

    REG_SP = 0xFFFE;
    f.gb.cpu.ime = false;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(f.gb.cpu.ime, true);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* HALT and STOP                                                        */
/* ------------------------------------------------------------------ */

static void t_cpu_halt_waits_for_interrupt(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x76, 0x00 }; /* HALT, NOP */

    REG_SP = 0xFFFE;
    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(f.gb.cpu.halted, true);

    /* Still halted: an interrupt that cannot be serviced does not wake it. */
    fixture_step(&f);
    CHECK_EQ(f.gb.cpu.halted, true);
    CHECK_EQ(REG_SP, 0xFFFE);

    f.gb.cpu.ime = true;
    f.gb.ie = GB_INT_TIMER;
    f.gb.iflag = GB_INT_TIMER;

    fixture_step(&f);
    CHECK_EQ(f.gb.cpu.halted, false);
    CHECK_EQ(REG_PC, 0x0050); /* jumped to the timer vector */
    fixture_destroy(&f);
}

static void t_cpu_halt_bug_when_disabled_with_pending(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x76,                   /* HALT with IME = 0 and IF already set */
        0x3E, 0x42              /* LD A,$42 -- never runs as written     */
    };

    fixture_load(&f, code, sizeof(code));
    f.gb.cpu.ime = false;
    f.gb.ie = GB_INT_TIMER;
    f.gb.iflag = GB_INT_TIMER;
    fixture_exec(&f, 3);
    CHECK_EQ(f.gb.cpu.halted, false);

    /* The byte after HALT is fetched without the PC moving, so the opcode
     * itself ($3E) is read a second time and lands in A. */
    CHECK_EQ(REG_A, 0x3E);
    CHECK_EQ(REG_PC, 0x0103);
    fixture_destroy(&f);
}

static void t_cpu_stop_is_left_by_a_button(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x10, 0x00 }; /* STOP */

    fixture_run(&f, code, sizeof(code), 1);
    CHECK_EQ(f.gb.cpu.stopped, true);

    /* Still stopped while no button is held. */
    f.gb.cpu.pc = 0x0100;
    fixture_step(&f);
    CHECK_EQ(f.gb.cpu.stopped, true);

    gb_set_keys(&f.gb, GB_KEY_START);
    f.gb.cpu.pc = 0x0100;
    fixture_step(&f);
    CHECK_EQ(f.gb.cpu.stopped, false);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

static void t_cpu_common_opcode_cycle_counts(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    CHECK_EQ(fixture_run_op(&f, 0x00), 4);  /* NOP           */
    CHECK_EQ(fixture_run_op(&f, 0x3E), 8);  /* LD A,n        */
    CHECK_EQ(fixture_run_op(&f, 0x21), 12); /* LD HL,nn      */
    CHECK_EQ(fixture_run_op(&f, 0x46), 8);  /* LD B,(HL)     */
    CHECK_EQ(fixture_run_op(&f, 0x34), 12); /* INC (HL)      */
    CHECK_EQ(fixture_run_op(&f, 0xC3), 16); /* JP nn         */
    CHECK_EQ(fixture_run_op(&f, 0x08), 20); /* LD (nn),SP    */
    CHECK_EQ(fixture_run_op(&f, 0xE8), 16); /* ADD SP,r8     */
    CHECK_EQ(fixture_run_op(&f, 0xF8), 12); /* LD HL,SP+r8   */
    CHECK_EQ(fixture_run_op(&f, 0xC9), 16); /* RET           */
    CHECK_EQ(fixture_run_op(&f, 0xE9), 4);  /* JP (HL)       */
    CHECK_EQ(fixture_run_op(&f, 0x87), 4);  /* ADD A,A       */
    CHECK_EQ(fixture_run_op(&f, 0xE6), 8);  /* AND n         */
    CHECK_EQ(fixture_run_op(&f, 0x01), 12); /* LD BC,nn      */
    CHECK_EQ(fixture_run_op(&f, 0xF5), 16); /* PUSH AF       */
    CHECK_EQ(fixture_run_op(&f, 0xF1), 12); /* POP AF        */
    CHECK_EQ(fixture_run_op(&f, 0xCD), 24); /* CALL nn       */
    CHECK_EQ(fixture_run_op(&f, 0xFF), 16); /* RST 38        */

    fixture_destroy(&f);
}

static void t_cpu_alu_memory_operand_timings(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    REG_H = 0xC0;
    REG_L = 0x00;
    gb_write(&f.gb, 0xC000, 0x00);

    /* Every ALU operation with a memory operand takes 8 cycles, not just
     * the arithmetic ones: the extra M-cycle is for fetching the byte from
     * outside the register file, and the logical operations fetch from
     * there too.  The hardware reference lists all sixteen at 2 M-cycles.
     */
    CHECK_EQ(fixture_run_op(&f, 0x86), 8); /* ADD A,(HL) */
    CHECK_EQ(fixture_run_op(&f, 0x8E), 8); /* ADC A,(HL) */
    CHECK_EQ(fixture_run_op(&f, 0x96), 8); /* SUB (HL)   */
    CHECK_EQ(fixture_run_op(&f, 0x9E), 8); /* XOR A,(HL) */
    CHECK_EQ(fixture_run_op(&f, 0xA6), 8); /* AND A,(HL) */
    CHECK_EQ(fixture_run_op(&f, 0xAE), 8); /* OR A,(HL)  */
    CHECK_EQ(fixture_run_op(&f, 0xBE), 8); /* CP (HL)    */

    /* The immediate forms are the same length and must not drift apart. */
    CHECK_EQ(fixture_run_op(&f, 0xC6), 8); /* ADD A,n */
    CHECK_EQ(fixture_run_op(&f, 0xCE), 8); /* ADC A,n */
    CHECK_EQ(fixture_run_op(&f, 0xD6), 8); /* SUB n   */
    CHECK_EQ(fixture_run_op(&f, 0xEE), 8); /* XOR A,n */
    CHECK_EQ(fixture_run_op(&f, 0xE6), 8); /* AND A,n */
    CHECK_EQ(fixture_run_op(&f, 0xF6), 8); /* OR A,n  */
    CHECK_EQ(fixture_run_op(&f, 0xFE), 8); /* CP n    */

    fixture_destroy(&f);
}

static void t_cpu_alu_register_operand_timings(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    CHECK_EQ(fixture_run_op(&f, 0x80), 4); /* ADD A,B  */
    CHECK_EQ(fixture_run_op(&f, 0x88), 4); /* ADC A,B  */
    CHECK_EQ(fixture_run_op(&f, 0x90), 4); /* SBC A,B  */
    CHECK_EQ(fixture_run_op(&f, 0x98), 4); /* AND A,B  */
    CHECK_EQ(fixture_run_op(&f, 0xA0), 4); /* XOR A,B  */
    CHECK_EQ(fixture_run_op(&f, 0xA8), 4); /* OR A,B   */
    CHECK_EQ(fixture_run_op(&f, 0xB0), 4); /* CP B     */
    CHECK_EQ(fixture_run_op(&f, 0x78), 4); /* LD A,B   */

    fixture_destroy(&f);
}

static void t_cpu_cb_memory_timings(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t bit0_hl[] = { 0xCB, 0x46 };  /* BIT 0,(HL) */
    const uint8_t rlc_hl[] = { 0xCB, 0x06 };   /* RLC (HL)  */
    const uint8_t res0_hl[] = { 0xCB, 0x86 };  /* RES 0,(HL) */

    fixture_load(&f, bit0_hl, sizeof(bit0_hl));
    REG_H = 0xC0;
    REG_L = 0x00;
    CHECK_EQ(fixture_step(&f), 12);

    fixture_load(&f, rlc_hl, sizeof(rlc_hl));
    REG_H = 0xC0;
    REG_L = 0x00;
    CHECK_EQ(fixture_step(&f), 16);

    fixture_load(&f, res0_hl, sizeof(res0_hl));
    REG_H = 0xC0;
    REG_L = 0x00;
    CHECK_EQ(fixture_step(&f), 16);

    fixture_destroy(&f);
}

/*
 * The $80-$BF block is eight groups of eight: ADD, ADC, SUB, SBC, AND, XOR,
 * OR, CP.  Each group is run once with A = $F0, B = $3C and carry set, so
 * every operation leaves a different result behind.
 */
static void t_cpu_alu_register_groups(void)
{
    static const struct {
        uint8_t op;
        uint8_t a;
        uint8_t f;
    } cases[] = {
        { 0x80, 0x2C, 0x10 },        /* ADD A,B: $F0 + $3C        */
        { 0x88, 0x2D, 0x10 },        /* ADC A,B: + carry          */
        { 0x90, 0xB4, 0x60 },        /* SUB B: low nibble borrows */
        { 0x98, 0xB3, 0x60 },        /* SBC A,B: - carry          */
        { 0xA0, 0x30, 0x20 },        /* AND B                     */
        { 0xA8, 0xCC, 0x00 },        /* XOR B                     */
        { 0xB0, 0xFC, 0x00 },        /* OR B                      */
        { 0xB8, 0xF0, 0x60 },        /* CP B: A unchanged         */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Fixture f = fixture_create(0x8000, 0x00);
        const uint8_t code[] = { cases[i].op };

        fixture_load(&f, code, sizeof(code));
        REG_A = 0xF0;
        REG_B = 0x3C;
        REG_F = 0x10;
        CHECK_EQ(fixture_step(&f), 4);
        CHECK_MSG(REG_A == cases[i].a && REG_F == cases[i].f,
                  "op $%02X: expected A=$%02X F=$%02X, got A=$%02X F=$%02X",
                  cases[i].op, cases[i].a, cases[i].f, REG_A, REG_F);
        fixture_destroy(&f);
    }
}

const TestCase cpu_tests[] = {
    { "cpu", "alu register groups", t_cpu_alu_register_groups },
    { "cpu", "nop consumes four cycles", t_cpu_nop_consumes_four_cycles },
    { "cpu", "ld bc immediate", t_cpu_ld_bc_immediate },
    { "cpu", "ld pair round trip", t_cpu_ld_pair_round_trip },
    { "cpu", "ld r from r", t_cpu_ld_r_from_r },
    { "cpu", "ldh uses the high page", t_cpu_ldh_uses_high_page },
    { "cpu", "ld (c) addresses the high page", t_cpu_ld_c_addresses_high_page },
    { "cpu", "ld (a16),sp is little endian", t_cpu_ld_a16_sp_is_little_endian },
    { "cpu", "push pop af masks the low nibble", t_cpu_push_pop_af_masks_low_nibble },
    { "cpu", "inc sets zero and half", t_cpu_inc_sets_zero_and_half },
    { "cpu", "inc half carry from the low nibble", t_cpu_inc_half_carry_from_low_nibble },
    { "cpu", "dec sets negative and half", t_cpu_dec_sets_negative_and_half },
    { "cpu", "inc (hl) reads and writes memory", t_cpu_inc_hl_reads_and_writes_memory },
    { "cpu", "inc bc leaves flags alone", t_cpu_inc_bc_leaves_flags_alone },
    { "cpu", "add sets half and carry", t_cpu_add_sets_half_and_carry },
    { "cpu", "add half carry boundary", t_cpu_add_half_carry_boundary },
    { "cpu", "add zero result sets z", t_cpu_add_zero_result_sets_z },
    { "cpu", "adc includes the carry", t_cpu_adc_includes_carry },
    { "cpu", "sub sets negative and borrow", t_cpu_sub_sets_negative_and_borrow },
    { "cpu", "sub half borrow", t_cpu_sub_half_borrow },
    { "cpu", "sub equal sets zero", t_cpu_sub_equal_sets_zero },
    { "cpu", "sbc with borrow", t_cpu_sbc_with_borrow },
    { "cpu", "and or xor", t_cpu_and_or_xor },
    { "cpu", "and sets half and zero", t_cpu_and_sets_half_and_zero },
    { "cpu", "cp does not store", t_cpu_cp_does_not_store },
    { "cpu", "daa after a bcd add", t_cpu_daa_after_bcd_add },
    { "cpu", "daa after a bcd add with carry", t_cpu_daa_after_bcd_add_carry },
    { "cpu", "daa after a bcd subtract", t_cpu_daa_after_bcd_subtract },
    { "cpu", "daa leaves a small digit alone", t_cpu_daa_leaves_a_small_digit_alone },
    { "cpu", "daa corrects a hex half carry", t_cpu_daa_corrects_a_hex_half_carry },
    { "cpu", "cpl", t_cpu_cpl },
    { "cpu", "scf sets the carry", t_cpu_scf_sets_carry },
    { "cpu", "scf then ccf", t_cpu_scf_then_ccf },
    { "cpu", "ccf flips the carry", t_cpu_ccf_flips_carry },
    { "cpu", "rotates clear zero", t_cpu_rotates_clear_zero },
    { "cpu", "rla uses the carry", t_cpu_rla_uses_carry },
    { "cpu", "rra uses the carry", t_cpu_rra_uses_carry },
    { "cpu", "add hl flags", t_cpu_add_hl_flags },
    { "cpu", "add hl carry out", t_cpu_add_hl_carry_out },
    { "cpu", "add sp signed", t_cpu_add_sp_signed },
    { "cpu", "add sp half carry is unsigned", t_cpu_add_sp_half_carry_uses_unsigned },
    { "cpu", "ld hl,sp signed", t_cpu_ld_hl_sp_signed },
    { "cpu", "jp absolute", t_cpu_jp_absolute },
    { "cpu", "jp (hl)", t_cpu_jp_hl },
    { "cpu", "conditional jp cycle counts", t_cpu_conditional_jp_cycle_counts },
    { "cpu", "jr relative", t_cpu_jr_relative },
    { "cpu", "jr negative offset", t_cpu_jr_negative_offset },
    { "cpu", "conditional jr not taken cycles", t_cpu_conditional_jr_not_taken_cycles },
    { "cpu", "call and ret", t_cpu_call_and_ret },
    { "cpu", "conditional call not taken", t_cpu_conditional_call_not_taken },
    { "cpu", "rst pushes and jumps", t_cpu_rst_pushes_and_jumps },
    { "cpu", "cb rlc", t_cpu_cb_rlc },
    { "cpu", "cb rrc", t_cpu_cb_rrc },
    { "cpu", "cb rl uses the carry", t_cpu_cb_rl_uses_carry },
    { "cpu", "cb rr uses the carry", t_cpu_cb_rr_uses_carry },
    { "cpu", "cb sla", t_cpu_cb_sla },
    { "cpu", "cb sra keeps the sign", t_cpu_cb_sra_keeps_sign },
    { "cpu", "cb srl", t_cpu_cb_srl },
    { "cpu", "cb swap", t_cpu_cb_swap },
    { "cpu", "cb bit does not store", t_cpu_cb_bit_sets_flags_without_storing },
    { "cpu", "cb bit set", t_cpu_cb_bit_set },
    { "cpu", "cb res and set", t_cpu_cb_res_and_set },
    { "cpu", "cb on memory", t_cpu_cb_on_memory },
    { "cpu", "cb bit memory cycles", t_cpu_cb_bit_memory_cycles },
    { "cpu", "interrupt taken when enabled", t_cpu_interrupt_is_taken_when_enabled },
    { "cpu", "interrupt ignored when disabled", t_cpu_interrupt_ignored_when_disabled },
    { "cpu", "interrupt priority order", t_cpu_interrupt_priority_order },
    { "cpu", "ei is delayed one instruction", t_cpu_ei_is_delayed_by_one_instruction },
    { "cpu", "ei then di blocks the interrupt", t_cpu_ei_then_di_blocks_interrupt },
    { "cpu", "reti enables ime immediately", t_cpu_reti_enables_immediately },
    { "cpu", "halt waits for an interrupt", t_cpu_halt_waits_for_interrupt },
    { "cpu", "halt bug when disabled with a pending flag", t_cpu_halt_bug_when_disabled_with_pending },
    { "cpu", "stop is left by a button", t_cpu_stop_is_left_by_a_button },
    { "cpu", "common opcode cycle counts", t_cpu_common_opcode_cycle_counts },
    { "cpu", "alu memory operand timings", t_cpu_alu_memory_operand_timings },
    { "cpu", "alu register operand timings", t_cpu_alu_register_operand_timings },
    { "cpu", "cb memory timings", t_cpu_cb_memory_timings },
    TEST_SUITE_END
};