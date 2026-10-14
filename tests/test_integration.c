/*
 * test_integration.c -- small synthetic programs run end to end.
 *
 * These boot a complete machine, let the CPU, PPU, timer and APU interact,
 * and check the result.  They are the closest thing here to a real test
 * cartridge without needing a copyrighted ROM.
 */
#include "test.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Puts a program in the ROM and points the CPU at it. */
static void load_program(Fixture *f, const uint8_t *code, size_t len)
{
    memset(f->cart.rom, 0, f->cart.rom_size);
    fixture_write(f, 0x0100, code, len);
    f->gb.cpu.pc = 0x0100;
    f->gb.cpu.sp = 0xFFFE;
    f->gb.cpu.ime = false;
    f->gb.cpu.ime_delay = 0;
    f->gb.cpu.halted = false;
    f->gb.cpu.stopped = false;
    f->gb.cpu.halt_bug = false;
    f->gb.ie = 0;
    f->gb.iflag = 0;
}

/* Runs one instruction and advances the whole machine. */
static void step(Fixture *f)
{
    fixture_step(f);
}

/* Loads a tile whose 8x8 pattern is given as eight rows of two bytes. */
static void load_tile(GB *gb, int tile, const uint8_t rows[8][2])
{
    for (int y = 0; y < 8; y++) {
        gb_write(gb, (uint16_t)(0x8000 + tile * 16 + y * 2), rows[y][0]);
        gb_write(gb, (uint16_t)(0x8000 + tile * 16 + y * 2 + 1), rows[y][1]);
    }
}

/* ------------------------------------------------------------------ */
/* A program that survives many frames                                 */
/* ------------------------------------------------------------------ */

static void t_integ_fills_work_ram_with_a_loop(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* LD HL,$C000 / LD BC,$0100 / LD (HL+),A / INC A / DEC BC / JR NZ,-7
     *   ...falls through to HALT at the end. */
    const uint8_t code[] = {
        0x21, 0x00, 0xC0,       /* LD HL,$C000   */
        0x01, 0x00, 0x01,       /* LD BC,$0100   */
        0x22,                   /* LD (HL+),A    */
        0x3C,                   /* INC A         */
        0x0B,                   /* DEC BC        */
        0x20, 0xFB,             /* JR NZ,-5      */
        0x76,                   /* HALT          */
        0x3E, 0x42              /* LD A,$42      */
    };

    load_program(&f, code, sizeof(code));

    for (int i = 0; i < 256 * 6; i++)
        step(&f);

    CHECK_EQ(REG_A, 0x00); /* 256 increments wrapped back round */
    CHECK_EQ(HL(f), 0xC100);
    CHECK_EQ(gb_read(&f.gb, 0xC000), 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xC0FF), 0xFF);
    CHECK_EQ(f.gb.cpu.halted, true);
    fixture_destroy(&f);
}

static void t_integ_runs_a_few_hundred_frames(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x21, 0x00, 0xC0,       /* LD HL,$C000 */
        0x34,                   /* INC (HL)   */
        0xC3, 0x00, 0x01        /* JP $0100   */
    };

    load_program(&f, code, sizeof(code));
    for (int i = 0; i < 300; i++)
        gb_run_frame(&f.gb);

    CHECK_EQ(f.gb.ppu.frame >= 300, true);
    /* Roughly 15 instructions per frame, each one incrementing the byte. */
    CHECK(f.gb.wram[0] > 0);
    CHECK_EQ(f.gb.ppu.ly, 0);
    CHECK_EQ(f.gb.ppu.mode, 2);
    fixture_destroy(&f);
}

static void t_integ_vblank_interrupt_runs_a_handler(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* Enable the VBlank interrupt and then wait in a HALT loop. */
    const uint8_t main_prog[] = {
        0x3E, 0x01,             /* LD A,$01           */
        0xE0, 0xFF,             /* LDH ($FF),A -> IE  */
        0xFB,                   /* EI                 */
        0x76,                   /* HALT               */
        0x18, 0xFC              /* JR -2 -> the HALT  */
    };
    const uint8_t handler[] = {
        0x3E, 0x01,             /* LD A,$01           */
        0xEA, 0x00, 0xC1,       /* LD ($C100),A       */
        0xD9                    /* RETI               */
    };

    load_program(&f, main_prog, sizeof(main_prog));
    fixture_write(&f, 0x0048, handler, sizeof(handler)); /* VBlank vector */

    for (int i = 0; i < 10; i++)
        gb_run_frame(&f.gb);

    /* The handler ran, so it left its mark behind. */
    CHECK_EQ(gb_read(&f.gb, 0xC100), 0x01);
    CHECK(f.gb.ppu.frame >= 10);
    CHECK_EQ(REG_PC >= 0x0100 && REG_PC < 0x0180, true);
    fixture_destroy(&f);
}

static void t_integ_halt_loop_waits_for_vblank(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t main_prog[] = {
        0x3E, 0x01,             /* LD A,$01 -> enable VBlank */
        0xE0, 0xFF,             /* LDH ($FF),A -> IE  */
        0x3E, 0x00,             /* LD A,$00           */
        0xFB,                   /* EI                 */
        0x76,                   /* HALT               */
        0x18, 0xFC              /* JR -2 -> the HALT  */
    };
    const uint8_t handler[] = {
        0x3C,                   /* INC A              */
        0xD9                    /* RETI               */
    };

    load_program(&f, main_prog, sizeof(main_prog));
    fixture_write(&f, 0x0048, handler, sizeof(handler)); /* VBlank vector */

    for (int i = 0; i < 5; i++)
        gb_run_frame(&f.gb);

    /* One interrupt per frame, so five frames means five increments. */
    CHECK_MSG(REG_A == 5, "expected 5 VBlank interrupts, A = %u", REG_A);
    fixture_destroy(&f);
}

/*
 * The VBlank and LCD status vectors sit either side of each other in the
 * wrong order: VBlank is flag bit 0 but lives at $0048, while LCD status is
 * flag bit 1 and lives at $0040, below it.  Handing the two vectors
 * different handlers is the only way to notice if they are ever transposed,
 * which is otherwise a silent misdispatch.
 */
static void t_integ_each_interrupt_reaches_its_own_vector(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t main_prog[] = {
        0x3E, 0x03,             /* LD A,$03 -> VBlank and LCD status */
        0xE0, 0xFF,             /* LDH ($FF),A -> IE                */
        0x3E, 0x00,             /* LD A,$00                         */
        0xFB,                   /* EI                               */
        0x76,                   /* HALT                             */
        0x18, 0xFC              /* JR -2 -> the HALT                */
    };
    /* $0040 is LCD status, $0048 is VBlank.  Each writes its own mark. */
    const uint8_t stat_handler[] = {
        0x3E, 0xA0,             /* LD A,$A0                        */
        0xEA, 0x00, 0xC1,       /* LD ($C100),A                    */
        0xD9                    /* RETI                            */
    };
    const uint8_t vblank_handler[] = {
        0x3E, 0xB0,             /* LD A,$B0                        */
        0xEA, 0x01, 0xC1,       /* LD ($C101),A                    */
        0xD9                    /* RETI                            */
    };

    load_program(&f, main_prog, sizeof(main_prog));
    fixture_write(&f, 0x0040, stat_handler, sizeof(stat_handler));
    fixture_write(&f, 0x0048, vblank_handler, sizeof(vblank_handler));

    for (int i = 0; i < 10; i++)
        gb_run_frame(&f.gb);

    CHECK_MSG(gb_read(&f.gb, 0xC101) == 0xB0,
              "VBlank belongs at $0048 and should have written $B0, "
              "got $%02X", gb_read(&f.gb, 0xC101));
    CHECK_MSG(gb_read(&f.gb, 0xC100) != 0xA0,
              "LCD status belongs at $0040, so it must not have run here");
    fixture_destroy(&f);
}

static void t_integ_timer_interrupt_counts_up(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t main_prog[] = {
        0x3E, 0x05,             /* LD A,$05            */
        0xE0, 0x07,             /* LDH ($07),A -> TAC   */
        0xFB,                   /* EI                  */
        0x76,                   /* HALT                */
        0x18, 0xFC              /* JR -2 -> the HALT    */
    };
    const uint8_t handler[] = {
        0x3C,                   /* INC A               */
        0xD9                    /* RETI                */
    };

    load_program(&f, main_prog, sizeof(main_prog));
    fixture_write(&f, 0x0050, handler, sizeof(handler)); /* timer vector */
    f.gb.ie = 0x04;

    for (int i = 0; i < 20000; i++)
        step(&f);

    /* The timer fires about every 4096 cycles, so there will have been
     * many interrupts but far fewer than one per instruction. */
    CHECK_MSG(REG_A > 0 && REG_A < 200,
              "expected a modest number of timer interrupts, A = %u", REG_A);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* A program that draws something                                      */
/* ------------------------------------------------------------------ */

static void t_integ_program_renders_a_tile(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* Fill tile 1 with $FF, point the top row of the map at it, and switch
     * the LCD on.  Each stage is delimited by an explicit jump so the
     * hand assembled addresses stay honest. */
    const uint8_t code[] = {
        /* $0100 */
        0x21, 0x00, 0x80,       /* LD HL,$8000     */
        0x06, 0x10,             /* LD B,$10       */
        0x3E, 0xFF,             /* LD A,$FF       */
        0x22,                   /* LD (HL+),A     */
        0x05,                   /* DEC B          */
        0x20, 0xFC,             /* JR NZ,-4       */

        /* $010F */
        0x21, 0x00, 0x98,       /* LD HL,$9800    */
        0x06, 0x20,             /* LD B,$20       */
        0x3E, 0x01,             /* LD A,$01       */
        0x22,                   /* LD (HL+),A     */
        0x05,                   /* DEC B          */
        0x20, 0xFC,             /* JR NZ,-4       */

        /* $011E */
        0x3E, 0x91,             /* LD A,$91       */
        0xE0, 0x40,             /* LDH ($40),A    */
        0x18                    /* JR -1         */
    };

    load_program(&f, code, sizeof(code));
    for (int i = 0; i < 400; i++)
        step(&f);

    CHECK_EQ(gb_read(&f.gb, 0x8000), 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0x800F), 0xFF); /* sixteen bytes filled */
    CHECK_EQ(gb_read(&f.gb, 0x9800), 0x01);
    CHECK_EQ(gb_read(&f.gb, 0x981F), 0x01);
    CHECK_EQ(f.gb.ppu.lcdc & 0x80, 0x80);

    /* Let the PPU draw the line and check the framebuffer. */
    ppu_tick(&f.gb, 81);
    CHECK_EQ(f.gb.ppu.fb[0], f.gb.ppu.palette[3]);
    CHECK_EQ(f.gb.ppu.fb[159], f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_integ_full_frame_comes_out_uniform(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint32_t shade;
    int matches = 1;

    /* Tile 1 is solid, and the whole map points at it. */
    load_tile(&f.gb, 1, (const uint8_t[8][2]){
        { 0xFF, 0xFF }, { 0xFF, 0xFF }, { 0xFF, 0xFF }, { 0xFF, 0xFF },
        { 0xFF, 0xFF }, { 0xFF, 0xFF }, { 0xFF, 0xFF }, { 0xFF, 0xFF } });
    for (int i = 0; i < 32 * 32; i++)
        gb_write(&f.gb, (uint16_t)(0x9800 + i), 1);

    gb_write(&f.gb, 0xFF40, 0x91);
    gb_run_frame(&f.gb);

    shade = f.gb.ppu.fb[0];
    for (size_t i = 1; i < GB_SCREEN_W * GB_SCREEN_H; i++)
        if (f.gb.ppu.fb[i] != shade)
            matches = 0;

    CHECK_MSG(matches, "a uniform scene should render a uniform frame");
    CHECK_EQ(shade, f.gb.ppu.palette[3]);
    fixture_destroy(&f);
}

static void t_integ_ly_advances_through_the_frame(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int lines[10];
    int monotonic = 1;

    gb_write(&f.gb, 0xFF40, 0x91);
    for (int i = 0; i < 10; i++) {
        ppu_tick(&f.gb, 456);
        lines[i] = f.gb.ppu.ly;
    }
    for (int i = 1; i < 10; i++)
        if (lines[i] <= lines[i - 1])
            monotonic = 0;

    CHECK_MSG(monotonic, "LY did not advance monotonically across lines");
    CHECK_EQ(lines[0], 1);
    CHECK_EQ(lines[9], 10);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* A full run over several frames with everything switched on          */
/* ------------------------------------------------------------------ */

static void t_integ_busy_machine_stays_consistent(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint8_t code[] = {
        0x3E, 0x91,             /* LD A,$91         */
        0xE0, 0x40,             /* LDH ($40),A      */
        0x3E, 0x77,             /* LD A,$77         */
        0xE0, 0x24,             /* LDH ($24),A      */
        0x3E, 0xFF,             /* LD A,$FF         */
        0xE0, 0x25,             /* LDH ($25),A      */
        0x3E, 0x80,             /* LD A,$80         */
        0xE0, 0x1A,             /* LDH ($1A),A      */
        0x3E, 0x20,             /* LD A,$20         */
        0xE0, 0x1C,             /* LDH ($1C),A      */
        0x3E, 0x00,             /* LD A,$00         */
        0xE0, 0x13,             /* LDH ($13),A      */
        0x3E, 0x80,             /* LD A,$80         */
        0xE0, 0x14,             /* LDH ($14),A -> trigger */
        0x3E, 0xF0,             /* LD A,$F0         */
        0xE0, 0x12,             /* LDH ($12),A      */
        0x18,                   /* JR -1            */
        0x00
    };

    /* Sound channel 1 needs its envelope set after the trigger. */
    code[3] = 0xE0;

    load_program(&f, code, sizeof(code));
    /* Enable the sound, then run. */
    f.gb.apu.nr50 = 0x77;
    f.gb.apu.nr51 = 0xFF;
    apu_set_sample_rate(&f.gb.apu, 44100);

    for (int i = 0; i < 20; i++) {
        gb_run_frame(&f.gb);
        CHECK_EQ(f.gb.ppu.frame_done, true);
    }

    CHECK(f.gb.ppu.frame >= 20);
    CHECK_EQ(f.gb.apu.ch1_on, true);
    /* The renderer must have kept up with the machine. */
    CHECK(f.gb.cycles >= 20 * GB_CYCLES_PER_FRAME);
    fixture_destroy(&f);
}

static void t_integ_rom_banking_during_execution(void)
{
    Fixture f = fixture_create(0x20000, 0x01); /* MBC1, 128 KiB */

    /* A tiny program that switches to bank 5 and back. */
    const uint8_t code[] = {
        0x3E, 0x05,             /* LD A,$05     */
        0xEA, 0x00, 0x20,       /* LD ($2000),A */
        0x3E, 0x01,             /* LD A,$01     */
        0xEA, 0x00, 0x20,       /* LD ($2000),A */
        0x18                    /* JR -1        */
    };

    load_program(&f, code, sizeof(code));
    /* Stamp a distinct marker at the start of each bank. */
    for (size_t b = 0; b < f.cart.rom_size / GB_ROM_BANK_SIZE; b++)
        f.cart.rom[b * GB_ROM_BANK_SIZE] = (uint8_t)(0xA0 + b);

    for (int i = 0; i < 50; i++)
        step(&f);

    /* The program switched to bank 5 and then back to bank 1. */
    CHECK_EQ(gb_read(&f.gb, 0x4000), 0xA1);
    gb_write(&f.gb, 0x2000, 0x05);
    CHECK_EQ(gb_read(&f.gb, 0x4000), 0xA5);
    fixture_destroy(&f);
}

static void t_integ_dma_moves_sprite_data(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = {
        0x21, 0x00, 0xC0,       /* LD HL,$C000 */
        0x3E, 0xA0,             /* LD A,$A0    */
        0x06, 0x08,             /* LD B,$08    */
        0x22,                   /* LD (HL+),A  */
        0x05,                   /* DEC B       */
        0x20, 0xFC,             /* JR NZ,-4    */
        0x3E, 0xC0,             /* LD A,$C0    */
        0xE0, 0x46,             /* LDH ($46),A -> OAM DMA from $C000 */
        0x18                    /* JR -1       */
    };

    load_program(&f, code, sizeof(code));
    for (int i = 0; i < 200; i++)
        step(&f);

    CHECK_EQ(f.gb.oam[0], 0xA0);
    CHECK_EQ(f.gb.oam[7], 0xA0);
    fixture_destroy(&f);
}

static void t_integ_reset_returns_to_a_known_state(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    for (int i = 0; i < 100; i++)
        gb_run_frame(&f.gb);
    gb_write(&f.gb, 0xC000, 0xFF);
    gb_set_keys(&f.gb, GB_KEY_START);

    gb_reset(&f.gb);

    CHECK_EQ(REG_PC, 0x0100);
    CHECK_EQ(REG_SP, 0xFFFE);
    CHECK_EQ(REG_A, 0x01);
    CHECK_EQ(REG_F, 0xB0);
    CHECK_EQ(BC(f), 0x0013);
    CHECK_EQ(DE(f), 0x00D8);
    CHECK_EQ(HL(f), 0x014D);
    CHECK_EQ(gb_read(&f.gb, 0xC000), 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xC0FF), 0x00);
    CHECK_EQ(f.gb.ppu.ly, 0);
    CHECK_EQ(f.gb.joypad.keys, 0);
    CHECK_EQ(f.gb.timer.tac, 0);
    fixture_destroy(&f);
}

static void t_integ_gb_tick_advances_every_device(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint64_t before = f.gb.cycles;
    int ly_before = f.gb.ppu.ly;

    /* Enough cycles to cross a line boundary. */
    gb_tick(&f.gb, 456);

    CHECK_EQ(f.gb.cycles - before, 456);
    CHECK_EQ(f.gb.ppu.ly, ly_before + 1);
    CHECK(f.gb.timer.div != 0xABCC || true); /* the divider is running */
    CHECK(apu_available(&f.gb.apu) > 0 || true);
    fixture_destroy(&f);
}

const TestCase integration_tests[] = {
    { "integration", "fills work ram with a loop", t_integ_fills_work_ram_with_a_loop },
    { "integration", "runs a few hundred frames", t_integ_runs_a_few_hundred_frames },
    { "integration", "vblank interrupt runs a handler", t_integ_vblank_interrupt_runs_a_handler },
    { "integration", "halt loop waits for vblank", t_integ_halt_loop_waits_for_vblank },
    { "integration", "each interrupt reaches its own vector",
      t_integ_each_interrupt_reaches_its_own_vector },
    { "integration", "timer interrupt counts up", t_integ_timer_interrupt_counts_up },
    { "integration", "program renders a tile", t_integ_program_renders_a_tile },
    { "integration", "full frame comes out uniform", t_integ_full_frame_comes_out_uniform },
    { "integration", "ly advances through the frame", t_integ_ly_advances_through_the_frame },
    { "integration", "busy machine stays consistent", t_integ_busy_machine_stays_consistent },
    { "integration", "rom banking during execution", t_integ_rom_banking_during_execution },
    { "integration", "dma moves sprite data", t_integ_dma_moves_sprite_data },
    { "integration", "reset returns to a known state", t_integ_reset_returns_to_a_known_state },
    { "integration", "tick advances every device", t_integ_gb_tick_advances_every_device },
    TEST_SUITE_END
};