/*
 * cpu.c -- Sharp LR35902 ("Game Boy CPU") interpreter.
 *
 * Two level switch: the 256 entry primary opcode table plus a second table
 * for the $CB prefix.  Every function returns the number of T-cycles
 * (clock ticks) the instruction consumed.
 */
#include "gb.h"

/* ------------------------------------------------------------------ */
/* Register pair helpers                                               */
/* ------------------------------------------------------------------ */

#define BC(gb) ((uint16_t)(((uint16_t)(gb)->cpu.b << 8) | (gb)->cpu.c))
#define DE(gb) ((uint16_t)(((uint16_t)(gb)->cpu.d << 8) | (gb)->cpu.e))
#define HL(gb) ((uint16_t)(((uint16_t)(gb)->cpu.h << 8) | (gb)->cpu.l))

static inline void set_bc(GB *gb, uint16_t v)
{
    gb->cpu.b = (uint8_t)(v >> 8);
    gb->cpu.c = (uint8_t)v;
}
static inline void set_de(GB *gb, uint16_t v)
{
    gb->cpu.d = (uint8_t)(v >> 8);
    gb->cpu.e = (uint8_t)v;
}
static inline void set_hl(GB *gb, uint16_t v)
{
    gb->cpu.h = (uint8_t)(v >> 8);
    gb->cpu.l = (uint8_t)v;
}
static inline void set_af(GB *gb, uint16_t v)
{
    gb->cpu.a = (uint8_t)(v >> 8);
    gb->cpu.f = (uint8_t)(v & 0xF0); /* the low nibble always reads back 0 */
}

/* ------------------------------------------------------------------ */
/* Flag helpers                                                        */
/* ------------------------------------------------------------------ */

#define FLAG(gb, m) ((gb)->cpu.f & (m))

static inline void set_flag(GB *gb, uint8_t mask, bool on)
{
    if (on)
        gb->cpu.f |= mask;
    else
        gb->cpu.f &= (uint8_t)~mask;
}

static inline uint8_t fetch8(GB *gb)
{
    return gb_read(gb, gb->cpu.pc++);
}

static inline uint16_t fetch16(GB *gb)
{
    uint16_t lo = fetch8(gb);
    uint16_t hi = fetch8(gb);
    return (uint16_t)(lo | (hi << 8));
}

static inline uint16_t pop16(GB *gb)
{
    uint16_t lo = gb_read(gb, gb->cpu.sp);
    uint16_t hi = gb_read(gb, (uint16_t)(gb->cpu.sp + 1));
    gb->cpu.sp = (uint16_t)(gb->cpu.sp + 2);
    return (uint16_t)(lo | (hi << 8));
}

static inline void push16(GB *gb, uint16_t v)
{
    gb->cpu.sp = (uint16_t)(gb->cpu.sp - 1);
    gb_write(gb, gb->cpu.sp, (uint8_t)(v >> 8));
    gb->cpu.sp = (uint16_t)(gb->cpu.sp - 1);
    gb_write(gb, gb->cpu.sp, (uint8_t)(v & 0xFF));
}

/* ------------------------------------------------------------------ */
/* ALU primitives                                                      */
/* ------------------------------------------------------------------ */

static void alu_add(GB *gb, uint8_t value, bool with_carry)
{
    uint16_t carry = (with_carry && FLAG(gb, GB_FLAG_C)) ? 1 : 0;
    uint16_t result = (uint16_t)gb->cpu.a + value + carry;

    set_flag(gb, GB_FLAG_H, ((gb->cpu.a & 0xF) + (value & 0xF) + carry) > 0xF);
    set_flag(gb, GB_FLAG_C, result > 0xFF);
    set_flag(gb, GB_FLAG_N, false);
    gb->cpu.a = (uint8_t)result;
    set_flag(gb, GB_FLAG_Z, gb->cpu.a == 0);
}

static void alu_sub(GB *gb, uint8_t value, bool with_carry, bool store)
{
    uint16_t carry = (with_carry && FLAG(gb, GB_FLAG_C)) ? 1 : 0;
    uint16_t result = (uint16_t)gb->cpu.a - value - carry;
    int low = (int)(gb->cpu.a & 0xF) - (int)(value & 0xF) - (int)carry;

    /* The half carry means "the low nibble borrowed". */
    set_flag(gb, GB_FLAG_H, low < 0);
    set_flag(gb, GB_FLAG_C, (result & 0x100) != 0);
    set_flag(gb, GB_FLAG_N, true);
    set_flag(gb, GB_FLAG_Z, (uint8_t)result == 0);
    if (store)
        gb->cpu.a = (uint8_t)result;
}

static void alu_and(GB *gb, uint8_t value)
{
    gb->cpu.a &= value;
    gb->cpu.f = 0;
    set_flag(gb, GB_FLAG_Z, gb->cpu.a == 0);
    set_flag(gb, GB_FLAG_H, true);
}

static void alu_xor(GB *gb, uint8_t value)
{
    gb->cpu.a ^= value;
    gb->cpu.f = 0;
    set_flag(gb, GB_FLAG_Z, gb->cpu.a == 0);
}

static void alu_or(GB *gb, uint8_t value)
{
    gb->cpu.a |= value;
    gb->cpu.f = 0;
    set_flag(gb, GB_FLAG_Z, gb->cpu.a == 0);
}

static void alu_daa(GB *gb)
{
    uint8_t a = gb->cpu.a;
    uint8_t correction = 0;
    bool carry = FLAG(gb, GB_FLAG_C) != 0;

    if (!FLAG(gb, GB_FLAG_N)) {
        /* After an addition, nudge the result back into BCD form. */
        if (carry || a > 0x99)
            correction |= 0x60;
        if (FLAG(gb, GB_FLAG_H) || (a & 0x0F) > 0x09)
            correction |= 0x06;
        gb->cpu.a = (uint8_t)(a + correction);
        if (correction & 0x60)
            carry = true;
    } else {
        /* After a subtraction, take the correction back off. */
        if (carry)
            correction |= 0x60;
        if (FLAG(gb, GB_FLAG_H))
            correction |= 0x06;
        gb->cpu.a = (uint8_t)(a - correction);
    }

    set_flag(gb, GB_FLAG_C, carry);
    set_flag(gb, GB_FLAG_H, false);
    set_flag(gb, GB_FLAG_Z, gb->cpu.a == 0);
}

static void alu_add_hl(GB *gb, uint16_t value)
{
    uint16_t hl = HL(gb);
    uint32_t result = (uint32_t)hl + value;

    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, ((hl & 0xFFF) + (value & 0xFFF)) > 0xFFF);
    set_flag(gb, GB_FLAG_C, result > 0xFFFF);
    set_hl(gb, (uint16_t)result);
}

/* ADD SP,r8 / LD HL,SP+r8 -- fetches the operand and sets the flags. */
static uint16_t alu_add_sp_e(GB *gb)
{
    uint8_t offset = fetch8(gb);
    uint16_t sp = gb->cpu.sp;
    uint16_t result = (uint16_t)(sp + (int8_t)offset);

    /* The addend is treated as unsigned for the half carry / carry checks. */
    set_flag(gb, GB_FLAG_Z, false);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, ((sp & 0xF) + (offset & 0xF)) > 0xF);
    set_flag(gb, GB_FLAG_C, ((sp & 0xFF) + (offset & 0xFF)) > 0xFF);
    return result;
}

/* INC/DEC only need the old value to compute their flags; carry is kept. */
static inline void flags_inc(GB *gb, uint8_t before)
{
    set_flag(gb, GB_FLAG_Z, (uint8_t)(before + 1) == 0);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, (before & 0xF) == 0xF);
}

static inline void flags_dec(GB *gb, uint8_t before)
{
    set_flag(gb, GB_FLAG_Z, (uint8_t)(before - 1) == 0);
    set_flag(gb, GB_FLAG_N, true);
    set_flag(gb, GB_FLAG_H, (before & 0xF) == 0);
}

/* ------------------------------------------------------------------ */
/* Rotate / shift primitives                                           */
/* ------------------------------------------------------------------ */

static uint8_t op_rlc(GB *gb, uint8_t v)
{
    bool msb = (v & 0x80) != 0;
    v = (uint8_t)((v << 1) | (v >> 7));
    set_flag(gb, GB_FLAG_C, msb);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, false);
    return v;
}

static uint8_t op_rrc(GB *gb, uint8_t v)
{
    bool lsb = (v & 0x01) != 0;
    v = (uint8_t)((v >> 1) | (v << 7));
    set_flag(gb, GB_FLAG_C, lsb);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, false);
    return v;
}

static uint8_t op_rl(GB *gb, uint8_t v)
{
    bool msb = (v & 0x80) != 0;
    v = (uint8_t)((v << 1) | (FLAG(gb, GB_FLAG_C) ? 1 : 0));
    set_flag(gb, GB_FLAG_C, msb);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, false);
    return v;
}

static uint8_t op_rr(GB *gb, uint8_t v)
{
    bool lsb = (v & 0x01) != 0;
    v = (uint8_t)((v >> 1) | (FLAG(gb, GB_FLAG_C) ? 0x80 : 0));
    set_flag(gb, GB_FLAG_C, lsb);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, false);
    return v;
}

static uint8_t op_sla(GB *gb, uint8_t v)
{
    bool msb = (v & 0x80) != 0;
    v = (uint8_t)(v << 1);
    set_flag(gb, GB_FLAG_C, msb);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, false);
    return v;
}

static uint8_t op_sra(GB *gb, uint8_t v)
{
    bool lsb = (v & 0x01) != 0;
    v = (uint8_t)((v >> 1) | (v & 0x80)); /* sign preserving */
    set_flag(gb, GB_FLAG_C, lsb);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, false);
    return v;
}

static uint8_t op_srl(GB *gb, uint8_t v)
{
    bool lsb = (v & 0x01) != 0;
    v = (uint8_t)(v >> 1);
    set_flag(gb, GB_FLAG_C, lsb);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, false);
    return v;
}

static uint8_t op_swap(GB *gb, uint8_t v)
{
    v = (uint8_t)((v >> 4) | (v << 4));
    set_flag(gb, GB_FLAG_C, false);
    set_flag(gb, GB_FLAG_N, false);
    set_flag(gb, GB_FLAG_H, false);
    return v;
}

/* ------------------------------------------------------------------ */
/* $CB prefix operands                                                 */
/* ------------------------------------------------------------------ */

static inline uint8_t reg_get(GB *gb, int idx)
{
    switch (idx) {
    case 0: return gb->cpu.b;
    case 1: return gb->cpu.c;
    case 2: return gb->cpu.d;
    case 3: return gb->cpu.e;
    case 4: return gb->cpu.h;
    case 5: return gb->cpu.l;
    case 6: return gb_read(gb, HL(gb));
    default: return gb->cpu.a;
    }
}

static inline void reg_set(GB *gb, int idx, uint8_t v)
{
    switch (idx) {
    case 0: gb->cpu.b = v; break;
    case 1: gb->cpu.c = v; break;
    case 2: gb->cpu.d = v; break;
    case 3: gb->cpu.e = v; break;
    case 4: gb->cpu.h = v; break;
    case 5: gb->cpu.l = v; break;
    case 6: gb_write(gb, HL(gb), v); break;
    default: gb->cpu.a = v; break;
    }
}

static int execute_cb(GB *gb)
{
    uint8_t op = fetch8(gb);
    int idx = op & 0x07;
    int kind = op >> 3;
    uint8_t value;

    if (kind < 8) { /* rotates and shifts, $00..$3F */
        value = reg_get(gb, idx);
        switch (kind) {
        case 0: value = op_rlc(gb, value); break;
        case 1: value = op_rrc(gb, value); break;
        case 2: value = op_rl(gb, value); break;
        case 3: value = op_rr(gb, value); break;
        case 4: value = op_sla(gb, value); break;
        case 5: value = op_sra(gb, value); break;
        case 6: value = op_swap(gb, value); break;
        default: value = op_srl(gb, value); break;
        }
        set_flag(gb, GB_FLAG_Z, value == 0);
        reg_set(gb, idx, value);
        return (idx == 6) ? 16 : 8;
    }

    if (kind < 16) { /* BIT b,r -- $40..$7F */
        value = reg_get(gb, idx);
        set_flag(gb, GB_FLAG_Z, (value & (1u << (kind & 7))) == 0);
        set_flag(gb, GB_FLAG_N, false);
        set_flag(gb, GB_FLAG_H, true);
        return (idx == 6) ? 12 : 8;
    }

    if (kind < 24) { /* RES b,r -- $80..$BF */
        reg_set(gb, idx, (uint8_t)(reg_get(gb, idx) & ~(1u << (kind & 7))));
        return (idx == 6) ? 16 : 8;
    }

    /* SET b,r -- $C0..$FF */
    reg_set(gb, idx, (uint8_t)(reg_get(gb, idx) | (1u << (kind & 7))));
    return (idx == 6) ? 16 : 8;
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

static inline bool has_pending_interrupt(GB *gb)
{
    return (gb->iflag & gb->ie & 0x1F) != 0;
}

/* Takes the highest priority pending interrupt.  Called only with IME set. */
static int service_interrupt(GB *gb)
{
    uint8_t pending = (uint8_t)(gb->iflag & gb->ie & 0x1F);
    int bit = 0;
    /*
     * Indexed by interrupt flag bit.  Each vector is $0040 + 8 * bit, so
     * VBlank (bit 0) is at $0040 and LCD status (bit 1) at $0048.
     */
    static const uint16_t vectors[5] = {
        0x0040, /* 0: VBlank    */
        0x0048, /* 1: LCD STAT  */
        0x0050, /* 2: timer     */
        0x0058, /* 3: serial    */
        0x0060, /* 4: joypad    */
    };

    while (bit < 5 && !(pending & (1u << bit)))
        bit++;
    if (bit >= 5)
        return 0;

    gb->cpu.ime = false;
    gb->iflag &= (uint8_t)~(1u << bit);
    push16(gb, gb->cpu.pc);
    gb->cpu.pc = vectors[bit];
    return 20;
}

/* ------------------------------------------------------------------ */
/* Instruction helpers                                                 */
/* ------------------------------------------------------------------ */

static inline int do_call(GB *gb, uint16_t addr)
{
    push16(gb, gb->cpu.pc);
    gb->cpu.pc = addr;
    return 24;
}

static inline int do_rst(GB *gb, uint8_t addr)
{
    push16(gb, gb->cpu.pc);
    gb->cpu.pc = addr;
    return 16;
}

static inline int do_ret(GB *gb)
{
    gb->cpu.pc = pop16(gb);
    return 16;
}

/* Conditional relative jump; the operand is always consumed. */
static inline int do_jr(GB *gb, bool take)
{
    int8_t offset = (int8_t)fetch8(gb);
    if (take)
        gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
    return take ? 12 : 8;
}

/* Conditional absolute jump. */
static inline int do_jp(GB *gb, bool take)
{
    uint16_t addr = fetch16(gb);
    if (take)
        gb->cpu.pc = addr;
    return take ? 16 : 12;
}

static inline int do_call_cc(GB *gb, bool take)
{
    uint16_t addr = fetch16(gb);
    return take ? do_call(gb, addr) : 12;
}

static inline int do_ret_cc(GB *gb, bool take)
{
    return take ? do_ret(gb) + 4 : 8; /* conditional RET is 20 cycles */
}

/* $A0..$BF holds no load forms at all: every opcode in that window is an
 * ALU operation.  The $E0/$F0 family is what covers the high page. */

/* ------------------------------------------------------------------ */
/* Instruction dispatch                                                */
/* ------------------------------------------------------------------ */

static int step_normal(GB *gb)
{
    uint8_t op;

    /* The HALT bug makes the CPU read the same byte one extra time. */
    if (gb->cpu.halt_bug) {
        gb->cpu.halt_bug = false;
        op = gb_read(gb, gb->cpu.pc);
    } else {
        op = fetch8(gb);
    }

    switch (op) {
    /* ---- $00..$3F : 8 bit loads, ALU and misc -------------------- */
    case 0x00: return 4;                                /* NOP         */
    case 0x01: set_bc(gb, fetch16(gb)); return 12;       /* LD BC,d16   */
    case 0x02: gb_write(gb, BC(gb), gb->cpu.a); return 8;
    case 0x03: set_bc(gb, (uint16_t)(BC(gb) + 1)); return 8;
    case 0x04: flags_inc(gb, gb->cpu.b); gb->cpu.b++; return 4;
    case 0x05: flags_dec(gb, gb->cpu.b); gb->cpu.b--; return 4;
    case 0x06: gb->cpu.b = fetch8(gb); return 8;
    case 0x07: gb->cpu.a = op_rlc(gb, gb->cpu.a);
               set_flag(gb, GB_FLAG_Z, false); return 4;   /* RLCA      */
    case 0x08: {                                          /* LD (a16),SP */
        uint16_t addr = fetch16(gb);
        gb_write(gb, addr, (uint8_t)(gb->cpu.sp & 0xFF));
        gb_write(gb, (uint16_t)(addr + 1), (uint8_t)(gb->cpu.sp >> 8));
        return 20;
    }
    case 0x09: alu_add_hl(gb, BC(gb)); return 8;
    case 0x0A: gb->cpu.a = gb_read(gb, BC(gb)); return 8;
    case 0x0B: set_bc(gb, (uint16_t)(BC(gb) - 1)); return 8;
    case 0x0C: flags_inc(gb, gb->cpu.c); gb->cpu.c++; return 4;
    case 0x0D: flags_dec(gb, gb->cpu.c); gb->cpu.c--; return 4;
    case 0x0E: gb->cpu.c = fetch8(gb); return 8;
    case 0x0F: gb->cpu.a = op_rrc(gb, gb->cpu.a);
               set_flag(gb, GB_FLAG_Z, false); return 4;   /* RRCA      */
    case 0x10:                                           /* STOP       */
        fetch8(gb);
        gb->cpu.stopped = true;
        gb->timer.div = 0; /* STOP resets DIV on real hardware */
        return 4;
    case 0x11: set_de(gb, fetch16(gb)); return 12;
    case 0x12: gb_write(gb, DE(gb), gb->cpu.a); return 8;
    case 0x13: set_de(gb, (uint16_t)(DE(gb) + 1)); return 8;
    case 0x14: flags_inc(gb, gb->cpu.d); gb->cpu.d++; return 4;
    case 0x15: flags_dec(gb, gb->cpu.d); gb->cpu.d--; return 4;
    case 0x16: gb->cpu.d = fetch8(gb); return 8;
    case 0x17: gb->cpu.a = op_rl(gb, gb->cpu.a);
               set_flag(gb, GB_FLAG_Z, false); return 4;   /* RLA       */
    case 0x18: {                                         /* JR r8      */
        int8_t off = (int8_t)fetch8(gb);
        gb->cpu.pc = (uint16_t)(gb->cpu.pc + off);
        return 12;
    }
    case 0x19: alu_add_hl(gb, DE(gb)); return 8;
    case 0x1A: gb->cpu.a = gb_read(gb, DE(gb)); return 8;
    case 0x1B: set_de(gb, (uint16_t)(DE(gb) - 1)); return 8;
    case 0x1C: flags_inc(gb, gb->cpu.e); gb->cpu.e++; return 4;
    case 0x1D: flags_dec(gb, gb->cpu.e); gb->cpu.e--; return 4;
    case 0x1E: gb->cpu.e = fetch8(gb); return 8;
    case 0x1F: gb->cpu.a = op_rr(gb, gb->cpu.a);
               set_flag(gb, GB_FLAG_Z, false); return 4;   /* RRA       */
    case 0x20: return do_jr(gb, !FLAG(gb, GB_FLAG_Z));
    case 0x21: set_hl(gb, fetch16(gb)); return 12;
    case 0x22: gb_write(gb, HL(gb), gb->cpu.a);
               set_hl(gb, (uint16_t)(HL(gb) + 1)); return 8;   /* LD (HL+),A */
    case 0x23: set_hl(gb, (uint16_t)(HL(gb) + 1)); return 8;
    case 0x24: flags_inc(gb, gb->cpu.h); gb->cpu.h++; return 4;
    case 0x25: flags_dec(gb, gb->cpu.h); gb->cpu.h--; return 4;
    case 0x26: gb->cpu.h = fetch8(gb); return 8;
    case 0x27: alu_daa(gb); return 4;
    case 0x28: return do_jr(gb, FLAG(gb, GB_FLAG_Z) != 0);
    case 0x29: alu_add_hl(gb, HL(gb)); return 8;
    case 0x2A: gb->cpu.a = gb_read(gb, HL(gb));
               set_hl(gb, (uint16_t)(HL(gb) + 1)); return 8;   /* LD A,(HL+) */
    case 0x2B: set_hl(gb, (uint16_t)(HL(gb) - 1)); return 8;
    case 0x2C: flags_inc(gb, gb->cpu.l); gb->cpu.l++; return 4;
    case 0x2D: flags_dec(gb, gb->cpu.l); gb->cpu.l--; return 4;
    case 0x2E: gb->cpu.l = fetch8(gb); return 8;
    case 0x2F:                                             /* CPL */
        gb->cpu.a = (uint8_t)~gb->cpu.a;
        set_flag(gb, GB_FLAG_N, true);
        set_flag(gb, GB_FLAG_H, true);
        return 4;
    case 0x30: return do_jr(gb, !FLAG(gb, GB_FLAG_C));
    case 0x31: gb->cpu.sp = fetch16(gb); return 12;
    case 0x32: gb_write(gb, HL(gb), gb->cpu.a);
               set_hl(gb, (uint16_t)(HL(gb) - 1)); return 8;   /* LD (HL-),A */
    case 0x33: gb->cpu.sp++; return 8;
    case 0x34: {                                            /* INC (HL) */
        uint8_t v = gb_read(gb, HL(gb));
        flags_inc(gb, v);
        gb_write(gb, HL(gb), (uint8_t)(v + 1));
        return 12;
    }
    case 0x35: {                                            /* DEC (HL) */
        uint8_t v = gb_read(gb, HL(gb));
        flags_dec(gb, v);
        gb_write(gb, HL(gb), (uint8_t)(v - 1));
        return 12;
    }
    case 0x36: gb_write(gb, HL(gb), fetch8(gb)); return 12;
    case 0x37:                                             /* SCF */
        set_flag(gb, GB_FLAG_N, false);
        set_flag(gb, GB_FLAG_H, false);
        set_flag(gb, GB_FLAG_C, true);
        return 4;
    case 0x38: return do_jr(gb, FLAG(gb, GB_FLAG_C) != 0);
    case 0x39: alu_add_hl(gb, gb->cpu.sp); return 8;
    case 0x3A: gb->cpu.a = gb_read(gb, HL(gb));
               set_hl(gb, (uint16_t)(HL(gb) - 1)); return 8;   /* LD A,(HL-) */
    case 0x3B: gb->cpu.sp--; return 8;
    case 0x3C: flags_inc(gb, gb->cpu.a); gb->cpu.a++; return 4;
    case 0x3D: flags_dec(gb, gb->cpu.a); gb->cpu.a--; return 4;
    case 0x3E: gb->cpu.a = fetch8(gb); return 8;
    case 0x3F:                                             /* CCF */
        set_flag(gb, GB_FLAG_C, !FLAG(gb, GB_FLAG_C));
        set_flag(gb, GB_FLAG_N, false);
        set_flag(gb, GB_FLAG_H, false);
        return 4;

    /* ---- $76 : HALT --------------------------------------------- */
    case 0x76:
        if (!gb->cpu.ime && has_pending_interrupt(gb)) {
            /* HALT with IME=0 and interrupts pending does not halt at all,
             * and the next opcode is fetched twice. */
            gb->cpu.halt_bug = true;
        } else {
            gb->cpu.halted = true;
        }
        return 4;

    /* ---- $C0..$FF : stack, jumps, calls, control ----------------- */
    case 0xC0: return do_ret_cc(gb, !FLAG(gb, GB_FLAG_Z));
    case 0xC1: set_bc(gb, pop16(gb)); return 12;
    case 0xC2: return do_jp(gb, !FLAG(gb, GB_FLAG_Z));
    case 0xC3: gb->cpu.pc = fetch16(gb); return 16;
    case 0xC4: return do_call_cc(gb, !FLAG(gb, GB_FLAG_Z));
    case 0xC5: push16(gb, BC(gb)); return 16;
    case 0xC6: alu_add(gb, fetch8(gb), false); return 8;
    case 0xC7: return do_rst(gb, 0x00);
    case 0xC8: return do_ret_cc(gb, FLAG(gb, GB_FLAG_Z) != 0);
    case 0xC9: return do_ret(gb);
    case 0xCA: return do_jp(gb, FLAG(gb, GB_FLAG_Z) != 0);
    case 0xCB: return execute_cb(gb);
    case 0xCC: return do_call_cc(gb, FLAG(gb, GB_FLAG_Z) != 0);
    case 0xCD: return do_call(gb, fetch16(gb));
    case 0xCE: alu_add(gb, fetch8(gb), true); return 8;
    case 0xCF: return do_rst(gb, 0x08);
    case 0xD0: return do_ret_cc(gb, !FLAG(gb, GB_FLAG_C));
    case 0xD1: set_de(gb, pop16(gb)); return 12;
    case 0xD2: return do_jp(gb, !FLAG(gb, GB_FLAG_C));
    case 0xD4: return do_call_cc(gb, !FLAG(gb, GB_FLAG_C));
    case 0xD5: push16(gb, DE(gb)); return 16;
    case 0xD6: alu_sub(gb, fetch8(gb), false, true); return 8;
    case 0xD7: return do_rst(gb, 0x10);
    case 0xD8: return do_ret_cc(gb, FLAG(gb, GB_FLAG_C) != 0);
    case 0xD9:                                             /* RETI */
        do_ret(gb);
        gb->cpu.ime = true;
        gb->cpu.ime_delay = 0;
        return 16;
    case 0xDA: return do_jp(gb, FLAG(gb, GB_FLAG_C) != 0);
    case 0xDC: return do_call_cc(gb, FLAG(gb, GB_FLAG_C) != 0);
    case 0xDE: alu_sub(gb, fetch8(gb), true, true); return 8;
    case 0xDF: return do_rst(gb, 0x18);
    case 0xE0: gb_write(gb, (uint16_t)(0xFF00 + fetch8(gb)), gb->cpu.a); return 12;
    case 0xE1: set_hl(gb, pop16(gb)); return 12;
    case 0xE2: gb_write(gb, (uint16_t)(0xFF00 + gb->cpu.c), gb->cpu.a); return 8;
    case 0xE5: push16(gb, HL(gb)); return 16;
    case 0xE6: alu_and(gb, fetch8(gb)); return 8;
    case 0xE7: return do_rst(gb, 0x20);
    case 0xE8: gb->cpu.sp = alu_add_sp_e(gb); return 16;
    case 0xE9: gb->cpu.pc = HL(gb); return 4;              /* JP (HL)   */
    case 0xEA: gb_write(gb, fetch16(gb), gb->cpu.a); return 16;
    case 0xEE: alu_xor(gb, fetch8(gb)); return 8;
    case 0xEF: return do_rst(gb, 0x28);
    case 0xF0: gb->cpu.a = gb_read(gb, (uint16_t)(0xFF00 + fetch8(gb))); return 12;
    case 0xF1: set_af(gb, pop16(gb)); return 12;
    case 0xF2: gb->cpu.a = gb_read(gb, (uint16_t)(0xFF00 + gb->cpu.c)); return 8;
    case 0xF3:                                             /* DI */
        gb->cpu.ime = false;
        gb->cpu.ime_delay = 0;
        return 4;
    case 0xF5:                                             /* PUSH AF */
        gb->cpu.sp--;
        gb_write(gb, gb->cpu.sp, gb->cpu.a);
        gb->cpu.sp--;
        gb_write(gb, gb->cpu.sp, (uint8_t)(gb->cpu.f & 0xF0));
        return 16;
    case 0xF6: alu_or(gb, fetch8(gb)); return 8;
    case 0xF7: return do_rst(gb, 0x30);
    case 0xF8: set_hl(gb, alu_add_sp_e(gb)); return 12;
    case 0xF9: gb->cpu.sp = HL(gb); return 8;
    case 0xFA: gb->cpu.a = gb_read(gb, fetch16(gb)); return 16;
    case 0xFB: gb->cpu.ime_delay = 2; return 4;            /* EI */
    case 0xFE: alu_sub(gb, fetch8(gb), false, false); return 8; /* CP */
    case 0xFF: return do_rst(gb, 0x38);

    default:
        break;
    }

    /* ---- $40..$7F : LD r,r' (HALT handled above) ------------------ */
    if (op >= 0x40 && op <= 0x7F) {
        int dst = (op >> 3) & 7;
        int src = op & 7;
        reg_set(gb, dst, reg_get(gb, src));
        return (dst == 6 || src == 6) ? 8 : 4;
    }

    /* ---- $80..$BF : ALU A,r -------------------------------------- */
    if (op >= 0x80 && op <= 0xBF) {
        int idx = op & 7;
        int group = (op >> 3) & 7;
        uint8_t value = reg_get(gb, idx);

        /* $80 ADD, $88 ADC, $90 SUB, $98 SBC, $A0 AND, $A8 XOR, $B0 OR,
         * $B8 CP. */
        switch (group) {
        case 0: alu_add(gb, value, false); break;        /* ADD */
        case 1: alu_add(gb, value, true); break;         /* ADC */
        case 2: alu_sub(gb, value, false, true); break;  /* SUB */
        case 3: alu_sub(gb, value, true, true); break;   /* SBC */
        case 4: alu_and(gb, value); break;               /* AND */
        case 5: alu_xor(gb, value); break;               /* XOR */
        case 6: alu_or(gb, value); break;                /* OR  */
        default: alu_sub(gb, value, false, false); break; /* CP */
        }
        if (idx != 6)
            return 4;
        /* A memory operand costs one extra M-cycle whatever the operation.
         * AND, OR, XOR and CP take just as long as ADD, because they all
         * have to fetch the byte from outside the register file. */
        return 8;
    }

    /* Locked opcodes behave as NOP on real hardware. */
    return 4;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

int cpu_step(GB *gb)
{
    int cycles;

    gb->cpu.instructions++;

    if (gb->cpu.stopped) {
        gb->cpu.idle_cycles++;
        return 4;
    }

    /* A pending flag always wakes the CPU, whether or not IME lets the
     * interrupt actually be taken. */
    if (gb->cpu.halted && has_pending_interrupt(gb))
        gb->cpu.halted = false;

    if (gb->cpu.ime) {
        cycles = service_interrupt(gb);
        if (cycles)
            return cycles;
    }

    if (gb->cpu.halted) {
        gb->cpu.idle_cycles++;
        return 4;
    }

    cycles = step_normal(gb);

    /* EI arms a two step countdown, so interrupts only become possible one
     * instruction after the following one has finished.  This is what makes
     * "EI, DI" block every interrupt. */
    if (gb->cpu.ime_delay) {
        if (--gb->cpu.ime_delay == 0)
            gb->cpu.ime = true;
    }

    return cycles;
}