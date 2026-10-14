/*
 * test_memory.c -- the CPU bus and the I/O register file.
 */
#include "test.h"

static void t_mem_rom_is_read_only(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t patch[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

    fixture_write(&f, 0x0100, patch, sizeof(patch));
    CHECK_EQ(gb_read(&f.gb, 0x0100), 0xDE);

    /* Writes into ROM space land in the mapper, not the program. */
    gb_write(&f.gb, 0x0100, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0x0100), 0xDE);
    fixture_destroy(&f);
}

static void t_mem_vram_round_trip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int ok = 1;

    for (int addr = 0x8000; addr < 0xA000; addr += 7)
        gb_write(&f.gb, (uint16_t)addr, (uint8_t)(addr & 0xFF));
    for (int addr = 0x8000; addr < 0xA000; addr += 7)
        if (gb_read(&f.gb, (uint16_t)addr) != (uint8_t)(addr & 0xFF))
            ok = 0;

    CHECK_MSG(ok, "VRAM did not read back what was written");
    fixture_destroy(&f);
}

static void t_mem_wram_round_trip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int ok = 1;

    for (int i = 0; i < 0x2000; i += 13)
        gb_write(&f.gb, (uint16_t)(0xC000 + i), (uint8_t)(i & 0xFF));
    for (int i = 0; i < 0x2000; i += 13)
        if (gb_read(&f.gb, (uint16_t)(0xC000 + i)) != (uint8_t)(i & 0xFF))
            ok = 0;

    CHECK_MSG(ok, "WRAM did not read back what was written");
    fixture_destroy(&f);
}

static void t_mem_echo_ram_mirrors_wram(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xC123, 0x7E);
    CHECK_EQ(gb_read(&f.gb, 0xE123), 0x7E);

    /* The other direction works too. */
    gb_write(&f.gb, 0xE456, 0x5A);
    CHECK_EQ(gb_read(&f.gb, 0xC456), 0x5A);

    /* And the shadow region really is the same storage. */
    gb_write(&f.gb, 0xE123, 0x11);
    CHECK_EQ(gb_read(&f.gb, 0xC123), 0x11);
    fixture_destroy(&f);
}

static void t_mem_oam_round_trip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);
    int ok = 1;

    for (unsigned i = 0; i < GB_OAM_SIZE; i++)
        gb_write(&f.gb, (uint16_t)(GB_MEM_OAM + i), (uint8_t)(i * 3));
    for (unsigned i = 0; i < GB_OAM_SIZE; i++)
        if (gb_read(&f.gb, (uint16_t)(GB_MEM_OAM + i)) != (uint8_t)(i * 3))
            ok = 0;

    CHECK_MSG(ok, "OAM did not read back what was written");
    fixture_destroy(&f);
}

static void t_mem_unusable_region(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFEA0, 0x42);
    CHECK_EQ(gb_read(&f.gb, 0xFEA0), 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0xFEFF), 0xFF);
    fixture_destroy(&f);
}

static void t_mem_echo_ram_stops_before_oam(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    /* $E000-$FDFF is a window onto $C000-$DDFF only. */
    gb_write(&f.gb, 0xE000, 0x33);
    CHECK_EQ(gb_read(&f.gb, 0xC000), 0x33);

    /* The last mirrored byte is $FDFF, which pairs with $DDFF. */
    gb_write(&f.gb, 0xFDFF, 0x11);
    CHECK_EQ(gb_read(&f.gb, 0xDDFF), 0x11);

    /* $DE00 and $DFFF are ordinary work RAM, not part of the window. */
    gb_write(&f.gb, 0xDE00, 0x77);
    CHECK_EQ(gb_read(&f.gb, 0xDE00), 0x77);
    CHECK_EQ(gb_read(&f.gb, 0xC000), 0x33); /* unchanged by the $DE00 write */

    /* And $FE00 onwards is OAM, which no echo address reaches. */
    gb_write(&f.gb, 0xFE00, 0x88);
    CHECK_EQ(gb_read(&f.gb, 0xFE00), 0x88);
    fixture_destroy(&f);
}

static void t_mem_hram_round_trip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF80, 0x12);
    gb_write(&f.gb, 0xFFFE, 0x34);
    CHECK_EQ(gb_read(&f.gb, 0xFF80), 0x12);
    CHECK_EQ(gb_read(&f.gb, 0xFFFE), 0x34);
    fixture_destroy(&f);
}

static void t_mem_interrupt_enable_register(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFFFF, 0x1F);
    CHECK_EQ(gb_read(&f.gb, 0xFFFF), 0x1F);
    CHECK_EQ(f.gb.ie, 0x1F);
    fixture_destroy(&f);
}

static void t_mem_interrupt_flag_top_bits_read_high(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF0F, 0x03);
    CHECK_EQ(gb_read(&f.gb, 0xFF0F), 0xE3);
    CHECK_EQ(f.gb.iflag, 0x03); /* only the low five bits are stored */
    fixture_destroy(&f);
}

static void t_mem_open_bus_registers_read_ff(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    CHECK_EQ(gb_read(&f.gb, 0xFF03), 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0xFF08), 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0xFF4F), 0xFF); /* CGB VRAM bank */
    CHECK_EQ(gb_read(&f.gb, 0xFF70), 0xFF); /* CGB SVBK         */
    CHECK_EQ(gb_read(&f.gb, 0xFF4C), 0xFF); /* CGB KEY1         */
    fixture_destroy(&f);
}

static void t_mem_ly_is_read_only(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.ppu.ly = 42;
    gb_write(&f.gb, 0xFF44, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xFF44), 42);
    fixture_destroy(&f);
}

static void t_mem_wx_masks_the_top_bit(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF4B, 0xFF);
    CHECK_EQ(f.gb.ppu.wx, 0x7F);
    CHECK_EQ(gb_read(&f.gb, 0xFF4B), 0x7F);
    fixture_destroy(&f);
}

static void t_mem_stat_keeps_only_the_select_bits(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF41, 0xFF);
    /* Bits 0-2 are driven by the hardware, bit 7 always reads back as 1. */
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x80, 0x80);
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x78, 0x78);
    fixture_destroy(&f);
}

static void t_mem_timer_registers_read_back_masked(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF07, 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0xFF07), 0xFF);
    CHECK_EQ(f.gb.timer.tac, 0x07);

    gb_write(&f.gb, 0xFF05, 0xAB);
    CHECK_EQ(gb_read(&f.gb, 0xFF05), 0xAB);
    gb_write(&f.gb, 0xFF06, 0xCD);
    CHECK_EQ(gb_read(&f.gb, 0xFF06), 0xCD);
    fixture_destroy(&f);
}

static void t_mem_boot_rom_switch(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.boot_rom_disabled = false;
    gb_write(&f.gb, 0xFF50, 0x01);
    CHECK_EQ(f.gb.boot_rom_disabled, true);

    f.gb.boot_rom_disabled = true;
    gb_write(&f.gb, 0xFF50, 0x00);
    CHECK_EQ(f.gb.boot_rom_disabled, false);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* OAM DMA                                                             */
/* ------------------------------------------------------------------ */

static void t_mem_oam_dma_copies_a_page(void)
{
    /* Cartridge RAM at $A000 is the one place a transfer can reliably read
     * from, because that is the only part of the $A0-$FF range the DMA
     * address decoding treats as cartridge RAM. */
    Fixture f = fixture_create(0x8000, 0x08);

    gb_write(&f.gb, 0x0000, 0x0A); /* enable cartridge RAM */
    for (unsigned i = 0; i < GB_OAM_SIZE; i++)
        gb_write(&f.gb, (uint16_t)(0xA000 + i), (uint8_t)(0xA0 + i));

    gb_write(&f.gb, 0xFF46, 0xA0);
    CHECK_EQ(f.gb.oam_dma.active, true);

    /* The copy takes 640 T-cycles, one byte every four. */
    for (int i = 0; i < 160 * 4; i++)
        oamdma_tick(&f.gb, 1);

    CHECK_EQ(f.gb.oam_dma.active, false);
    CHECK_EQ(f.gb.oam[0], 0xA0);
    CHECK_EQ(f.gb.oam[1], 0xA1);
    CHECK_EQ(f.gb.oam[GB_OAM_SIZE - 1], (uint8_t)(0xA0 + GB_OAM_SIZE - 1));
    fixture_destroy(&f);
}

static void t_mem_oam_dma_takes_640_cycles(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int elapsed = 0;

    gb_write(&f.gb, 0xFF46, 0xC0);
    while (f.gb.oam_dma.active && elapsed < 2000) {
        oamdma_tick(&f.gb, 4);
        elapsed += 4;
    }

    CHECK_EQ(elapsed, 640);
    fixture_destroy(&f);
}

static void t_mem_oam_dma_blocks_direct_writes(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    /* The PPU owns video memory while it fetches, so set the screen up
     * with it switched off: that is what real code does too. */
    fixture_video_accessible(&f);

    gb_write(&f.gb, 0xFE00, 0x11);
    CHECK_EQ(f.gb.oam[0], 0x11);

    gb_write(&f.gb, 0xFF46, 0xA0);
    /* While the transfer runs the CPU cannot disturb the destination. */
    gb_write(&f.gb, 0xFE00, 0x99);
    CHECK_EQ(f.gb.oam[0], 0x11);

    for (int i = 0; i < 160; i++)
        oamdma_tick(&f.gb, 4);
    CHECK_EQ(f.gb.oam[0], 0xFF); /* and it has been overwritten from the cart */
    fixture_destroy(&f);
}

/*
 * The transfer does not use the CPU's address decoding.  It puts the source
 * address out on its own bus, where everything from $A0 up reads as cartridge
 * RAM, so a transfer nominally sourced from work RAM does not in fact see
 * work RAM, and one nominally sourced from $FF00 does not see the joypad.
 */
static void t_mem_oam_dma_ignores_the_cpu_memory_map(void)
{
    Fixture f = fixture_create(0x8000, 0x08);

    gb_write(&f.gb, 0x0000, 0x0A); /* enable cartridge RAM */

    /* Work RAM, HRAM and the joypad register all hold recognisable values,
     * and cartridge RAM holds something else entirely. */
    for (unsigned i = 0; i < GB_OAM_SIZE; i++)
        gb_write(&f.gb, (uint16_t)(0xC000 + i), 0x11);
    gb_write(&f.gb, 0xFF80, 0x22);
    gb_write(&f.gb, 0xA000, 0x7E);
    for (unsigned i = 0; i < GB_OAM_SIZE; i++)
        gb_write(&f.gb, (uint16_t)(0xA000 + i), 0x33);

    gb_write(&f.gb, 0xFF46, 0xC0); /* nominally from work RAM */
    for (int i = 0; i < 160 * 4; i++)
        oamdma_tick(&f.gb, 1);

    CHECK_MSG(f.gb.oam[0] == 0x33 && f.gb.oam[79] == 0x33,
              "a transfer from $C000 must read cartridge RAM, got $%02X and $%02X",
              f.gb.oam[0], f.gb.oam[79]);

    /* What matters about the high range is that the CPU's own devices are
     * invisible to a transfer: $FF80 is high RAM holding $22, and $FF00 is
     * the joypad matrix.  Neither may come through. */
    gb_write(&f.gb, 0xFF46, 0xFF);
    for (int i = 0; i < 160 * 4; i++)
        oamdma_tick(&f.gb, 1);

    CHECK_MSG(f.gb.oam[0] != 0x22,
              "a transfer must not read high RAM, got $%02X", f.gb.oam[0]);
    CHECK_MSG((f.gb.oam[0] & 0x0F) != (0xCF & 0x0F),
              "a transfer must not read the joypad matrix, got $%02X",
              f.gb.oam[0]);

    fixture_destroy(&f);
}

static void t_mem_dma_register_reads_back(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF46, 0xC0);
    CHECK_EQ(gb_read(&f.gb, 0xFF46), 0xC0);
    fixture_destroy(&f);
}

/*
 * The register values the DMG boot ROM leaves behind, as tabulated in
 * Pan Docs.  A game may read any of these before writing to it, so the
 * machine has to come up looking like a real one rather than like zeroes.
 *
 * Only STAT departs, and only in its mode field: the reference has the
 * CPU arriving at $0100 in the middle of a frame, where the mode reads
 * 1.  Starting a frame at line 0 in VBlank would step straight past line
 * 0, so the mode starts at 2 and the rest of the register is exact.
 */
typedef struct {
    uint16_t addr;
    uint8_t want;
    const char *name;
} BootReg;

static const BootReg boot_regs[] = {
    { 0xFF00, 0xCF, "P1" },   { 0xFF01, 0x00, "SB" },   { 0xFF02, 0x7E, "SC" },
    { 0xFF04, 0xAB, "DIV" },  { 0xFF05, 0x00, "TIMA" }, { 0xFF06, 0x00, "TMA" },
    { 0xFF07, 0xF8, "TAC" },  { 0xFF0F, 0xE1, "IF" },
    { 0xFF10, 0x80, "NR10" },  { 0xFF11, 0xBF, "NR11" },
    { 0xFF13, 0xFF, "NR13" },  { 0xFF14, 0xBF, "NR14" },
    { 0xFF16, 0x3F, "NR21" },
    { 0xFF18, 0xFF, "NR23" },  { 0xFF19, 0xBF, "NR24" },
    { 0xFF1A, 0x7F, "NR30" },  { 0xFF1B, 0xFF, "NR31" }, { 0xFF1C, 0x9F, "NR32" },
    { 0xFF1D, 0xFF, "NR33" },  { 0xFF1E, 0xBF, "NR34" },
    { 0xFF24, 0x77, "NR50" },  { 0xFF25, 0xF3, "NR51" }, { 0xFF26, 0xF1, "NR52" },
    { 0xFF40, 0x91, "LCDC" },  { 0xFF42, 0x00, "SCY" },  { 0xFF43, 0x00, "SCX" },
    { 0xFF44, 0x00, "LY" },    { 0xFF45, 0x00, "LYC" },  { 0xFF46, 0xFF, "DMA" },
    { 0xFF4A, 0x00, "WY" },    { 0xFF4B, 0x00, "WX" },
    { 0xFFFF, 0x00, "IE" },
};

static void t_memory_post_boot_register_values(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* The fixture clears the pending interrupts so that tests start from a
     * quiet machine; here the whole point is to see what a real power-up
     * leaves behind, so put it back. */
    gb_reset(&f.gb);

    for (size_t i = 0; i < sizeof(boot_regs) / sizeof(boot_regs[0]); i++) {
        uint8_t got = gb_read(&f.gb, boot_regs[i].addr);

        CHECK_MSG(got == boot_regs[i].want,
                  "%s ($%04X): the reference says $%02X, got $%02X",
                  boot_regs[i].name, boot_regs[i].addr, boot_regs[i].want, got);
    }

    fixture_destroy(&f);
}

/* STAT keeps bit 2 set while LY and LYC agree, which is the case when the
 * machine comes up, and the unused bit 7 always reads as one. */
static void t_memory_stat_reports_the_lyc_compare(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF45, 0x05); /* LYC = 5, so LY 0 no longer matches */
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x04, 0x00);

    gb_write(&f.gb, 0xFF45, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x04, 0x04);
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x80, 0x80);

    /* The interrupt enables are bits 6 to 3, and they survive a read. */
    gb_write(&f.gb, 0xFF41, 0x78);
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x78, 0x78);

    fixture_destroy(&f);
}

/* NR12 is $F3 after the boot ROM, so the first channel's DAC is powered
 * and NR52 reports it as on. */
static void t_memory_post_boot_audio_state(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_reset(&f.gb);
    CHECK_EQ(f.gb.apu.nr12, 0xF3);
    CHECK_EQ(f.gb.apu.powered, true);
    CHECK_EQ(gb_read(&f.gb, 0xFF26), 0xF1);
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x80, 0x80); /* powered */
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x01, 0x01); /* channel 1 DAC on */
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x0E, 0x00); /* the rest are off */

    fixture_destroy(&f);
}

/*
 * The PPU is the bus master for video memory while it fetches, and a CPU
 * access that arrives at the wrong moment is lost rather than delayed:
 * the write goes nowhere and the read comes back as open bus.  VRAM is
 * reachable in modes 0, 1 and 2; OAM only in modes 0 and 1.
 */
static void t_memory_vram_is_locked_only_during_the_transfer(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int seen[4] = { 0, 0, 0, 0 };
    int locked_in_mode3_seen = 0, locked_in_mode2_seen = 0;
    int open_in_mode2 = 0;

    gb_write(&f.gb, 0xFF40, 0x91); /* LCD on, background on */

    for (int i = 0; i < 20000; i++) {
        int mode = f.gb.ppu.mode;
        uint8_t read;

        if (mode >= 0 && mode < 4)
            seen[mode] = 1;

        gb_write(&f.gb, 0x8000, 0x5A);
        read = gb_read(&f.gb, 0x8000);

        if (mode == 3 && read == 0xFF)
            locked_in_mode3_seen = 1;
        if (mode == 2 && read == 0xFF)
            locked_in_mode2_seen = 1;
        if (mode == 2 && read == 0x5A)
            open_in_mode2 = 1;

        gb_tick(&f.gb, 4);
    }

    /* The four modes were all visited, so the check was meaningful. */
    CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
    CHECK_MSG(locked_in_mode3_seen,
              "mode 3 owns VRAM, so a write there must not read back");
    CHECK_MSG(!locked_in_mode2_seen,
              "VRAM is reachable during the object scan, so mode 2 must not "
              "return open bus");
    CHECK_MSG(open_in_mode2,
              "a write to VRAM during mode 2 must land");

    fixture_destroy(&f);
}

/* A write that loses the race is gone for good, not held back until later. */
static void t_memory_a_locked_vram_write_is_lost(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF40, 0x91);

    /* Spin until the PPU is mid-transfer, then write. */
    while (f.gb.ppu.mode != 3)
        gb_tick(&f.gb, 4);

    gb_write(&f.gb, 0x8123, 0x77);
    while (f.gb.ppu.mode == 3)
        gb_tick(&f.gb, 4);

    CHECK_MSG(gb_read(&f.gb, 0x8123) != 0x77,
              "the write happened during mode 3 and should have been dropped");
    fixture_destroy(&f);
}

/* OAM is locked for the object scan as well as the transfer. */
static void t_memory_oam_is_locked_for_the_object_scan(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int locked_in_mode2 = 0, open_in_mode0 = 0, locked_in_mode3 = 0;

    gb_write(&f.gb, 0xFF40, 0x91);

    for (int i = 0; i < 20000; i++) {
        int mode = f.gb.ppu.mode;
        uint8_t read;

        gb_write(&f.gb, GB_MEM_OAM, 0x33);
        read = gb_read(&f.gb, GB_MEM_OAM);

        if (mode == 2 && read == 0xFF)
            locked_in_mode2 = 1;
        if (mode == 3 && read == 0xFF)
            locked_in_mode3 = 1;
        if (mode == 0 && read == 0x33)
            open_in_mode0 = 1;

        gb_tick(&f.gb, 4);
    }

    CHECK_MSG(locked_in_mode2,
              "the PPU is reading OAM during the object scan, so a write "
              "must not read back");
    CHECK_MSG(locked_in_mode3, "mode 3 owns OAM too");
    CHECK_MSG(open_in_mode0, "OAM is reachable in horizontal blank");
    fixture_destroy(&f);
}

/* With the LCD off the PPU fetches nothing, so everything is reachable
 * however the CPU happens to be running. */
static void t_memory_video_memory_is_open_with_the_lcd_off(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int checked = 0;

    fixture_video_accessible(&f);

    for (int i = 0; i < 5000; i++) {
        gb_write(&f.gb, 0x8000, 0x5A);
        gb_write(&f.gb, GB_MEM_OAM, 0x33);
        if (gb_read(&f.gb, 0x8000) == 0x5A && gb_read(&f.gb, GB_MEM_OAM) == 0x33)
            checked++;
        gb_tick(&f.gb, 4);
    }

    CHECK_MSG(checked == 5000,
              "every access should work with the LCD off, %d of 5000 did",
              checked);
    fixture_destroy(&f);
}

/*
 * Writing STAT has a hardware quirk on a monochrome Game Boy.  Outside the
 * pixel transfer and outside a LY==LYC coincidence, the write is not taken
 * straight away: the register behaves as if $FF had been written for one
 * M-cycle, and only then does the real value land.  All four interrupt
 * sources are selected for that instant, which is enough to fire one.
 *
 * It is obscure, but two published games depend on it and neither runs on a
 * Game Boy Color, which does not have it.
 */
static void t_memory_writing_stat_selects_every_source_briefly(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int saw_fire = 0;

    gb_write(&f.gb, 0xFF40, 0x91);
    gb_write(&f.gb, 0xFF41, 0x00); /* no source selected, so the line is low */

    for (int i = 0; i < 2000 && !saw_fire; i++) {
        f.gb.iflag = 0;

        /* Write zero while the PPU is outside mode 3 and outside LY==LYC. */
        while (f.gb.ppu.mode == 3 || f.gb.ppu.ly == f.gb.ppu.lyc)
            gb_tick(&f.gb, 4);

        gb_write(&f.gb, 0xFF41, 0x00);

        if (f.gb.iflag & GB_INT_LCDSTAT)
            saw_fire = 1;

        gb_tick(&f.gb, 4);
    }

    CHECK_MSG(saw_fire,
              "the one M-cycle of $FF should raise a STAT interrupt");
    fixture_destroy(&f);
}

/* The delayed value does land, a cycle later. */
static void t_memory_a_delayed_stat_write_eventually_applies(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF40, 0x91);
    gb_write(&f.gb, 0xFF41, 0x00);

    while (f.gb.ppu.mode == 3 || f.gb.ppu.ly == f.gb.ppu.lyc)
        gb_tick(&f.gb, 4);

    gb_write(&f.gb, 0xFF41, 0x68); /* modes 0, 2 and 3 */
    CHECK_MSG((gb_read(&f.gb, 0xFF41) & 0x78) != 0x68,
              "the write should not have taken effect yet");

    for (int i = 0; i < 8; i++)
        gb_tick(&f.gb, 4);

    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x78, 0x68);
    fixture_destroy(&f);
}

/* During the pixel transfer, and on a LY==LYC coincidence, there is no
 * quirk: the write lands at once. */
static void t_memory_stat_write_is_immediate_during_the_transfer(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF40, 0x91);

    while (f.gb.ppu.mode != 3)
        gb_tick(&f.gb, 4);

    gb_write(&f.gb, 0xFF41, 0x68);
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x78, 0x68);
    fixture_destroy(&f);
}

/* A powered down PPU reports mode 0 rather than its internal state. */
static void t_memory_stat_reports_mode_zero_with_the_lcd_off(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF40, 0x91);
    while (f.gb.ppu.mode != 3)
        gb_tick(&f.gb, 4);

    gb_write(&f.gb, 0xFF40, 0x00); /* LCD off */
    CHECK_EQ(gb_read(&f.gb, 0xFF41) & 0x03, 0x00);
    fixture_destroy(&f);
}

const TestCase memory_tests[] = {
    { "memory", "writing stat selects every source briefly",
      t_memory_writing_stat_selects_every_source_briefly },
    { "memory", "a delayed stat write eventually applies",
      t_memory_a_delayed_stat_write_eventually_applies },
    { "memory", "stat write immediate during the transfer",
      t_memory_stat_write_is_immediate_during_the_transfer },
    { "memory", "stat reports mode zero with the lcd off",
      t_memory_stat_reports_mode_zero_with_the_lcd_off },
    { "memory", "vram locked only during the transfer",
      t_memory_vram_is_locked_only_during_the_transfer },
    { "memory", "a locked vram write is lost",
      t_memory_a_locked_vram_write_is_lost },
    { "memory", "oam locked for the object scan",
      t_memory_oam_is_locked_for_the_object_scan },
    { "memory", "video memory open with the lcd off",
      t_memory_video_memory_is_open_with_the_lcd_off },
    { "memory", "post-boot register values",    t_memory_post_boot_register_values },
    { "memory", "stat reports the lyc compare", t_memory_stat_reports_the_lyc_compare },
    { "memory", "post-boot audio state",       t_memory_post_boot_audio_state },
    { "memory", "rom is read only", t_mem_rom_is_read_only },
    { "memory", "vram round trip", t_mem_vram_round_trip },
    { "memory", "wram round trip", t_mem_wram_round_trip },
    { "memory", "echo ram mirrors wram", t_mem_echo_ram_mirrors_wram },
    { "memory", "oam round trip", t_mem_oam_round_trip },
    { "memory", "unusable region", t_mem_unusable_region },
    { "memory", "echo ram stops before oam", t_mem_echo_ram_stops_before_oam },
    { "memory", "hram round trip", t_mem_hram_round_trip },
    { "memory", "interrupt enable register", t_mem_interrupt_enable_register },
    { "memory", "interrupt flag top bits read high", t_mem_interrupt_flag_top_bits_read_high },
    { "memory", "open bus registers read ff", t_mem_open_bus_registers_read_ff },
    { "memory", "ly is read only", t_mem_ly_is_read_only },
    { "memory", "wx masks the top bit", t_mem_wx_masks_the_top_bit },
    { "memory", "stat keeps the select bits", t_mem_stat_keeps_only_the_select_bits },
    { "memory", "timer registers read back masked", t_mem_timer_registers_read_back_masked },
    { "memory", "boot rom switch", t_mem_boot_rom_switch },
    { "memory", "oam dma copies a page", t_mem_oam_dma_copies_a_page },
    { "memory", "oam dma ignores the cpu memory map",
      t_mem_oam_dma_ignores_the_cpu_memory_map },
    { "memory", "oam dma takes 640 cycles", t_mem_oam_dma_takes_640_cycles },
    { "memory", "oam dma blocks direct writes", t_mem_oam_dma_blocks_direct_writes },
    { "memory", "dma register reads back", t_mem_dma_register_reads_back },
    TEST_SUITE_END
};