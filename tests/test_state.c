/*
 * test_state.c -- saving and restoring a snapshot.
 */
#include <stdio.h>

#include <stdio.h>

#include "test.h"
#include "platform.h"



static void t_state_round_trip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const char *path = test_temp_path("state.bin");
    GB restored;

    { const uint8_t boot[3] = { 0x3C, 0x00, 0x00 };
      fixture_write(&f, 0x0150, boot, sizeof(boot)); }
    f.gb.cpu.pc = 0x0150;
    f.gb.cpu.a = 0x5A;
    f.gb.cpu.f = 0xC0;
    f.gb.cpu.sp = 0xFF00;
    f.gb.ie = 0x1F;
    f.gb.iflag = 0x0A;
    f.gb.wram[0x1234] = 0x77;
    f.gb.vram[0x0ABC] = 0x44;
    f.gb.oam[10] = 0x99;
    f.gb.hram[0x20] = 0x33;
    f.gb.ppu.scy = 0x7F;
    f.gb.ppu.window_line = 3;
    f.gb.timer.tima = 0x12;
    f.gb.timer.div = 0x3456;

    CHECK(gb_save_state(&f.gb, path));

    memset(&restored, 0, sizeof(restored));
    restored.cart = f.cart;
    CHECK(gb_load_state(&restored, path));

    CHECK_EQ(restored.cpu.pc, 0x0150);
    CHECK_EQ(restored.cpu.a, 0x5A);
    CHECK_EQ(restored.cpu.f, 0xC0);
    CHECK_EQ(restored.cpu.sp, 0xFF00);
    CHECK_EQ(restored.ie, 0x1F);
    CHECK_EQ(restored.iflag, 0x0A);
    CHECK_EQ(restored.wram[0x1234], 0x77);
    CHECK_EQ(restored.vram[0x0ABC], 0x44);
    CHECK_EQ(restored.oam[10], 0x99);
    CHECK_EQ(restored.hram[0x20], 0x33);
    CHECK_EQ(restored.ppu.scy, 0x7F);
    CHECK_EQ(restored.ppu.window_line, 3);
    CHECK_EQ(restored.timer.tima, 0x12);
    CHECK_EQ(restored.timer.div, 0x3456);

    restored.cart.rom = NULL; /* borrowed from the fixture */
    restored.cart.ram = NULL;
    fixture_destroy(&f);
    remove(path);
}

static void t_state_continues_identically(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const char *path = test_temp_path("state.bin");
    GB restored;
    const uint8_t code[] = {
        0x3E, 0x11,             /* LD A,$11 */
        0x06, 0x22,             /* LD B,$22 */
        0x78,                   /* LD A,B   */
        0x00, 0x00              /* spin     */
    };
    int direct_trace[64];
    int restored_trace[64];

    fixture_load(&f, code, sizeof(code));
    for (int i = 0; i < 4; i++)
        fixture_step(&f);
    CHECK(gb_save_state(&f.gb, path));

    /* Run a while on the original. */
    for (int i = 0; i < 64; i++) {
        direct_trace[i] = f.gb.cpu.a;
        fixture_step(&f);
    }

    memset(&restored, 0, sizeof(restored));
    restored.cart = f.cart;
    CHECK(gb_load_state(&restored, path));

    for (int i = 0; i < 64; i++) {
        restored_trace[i] = restored.cpu.a;
        {
            int cycles = cpu_step(&restored);
            gb_tick(&restored, cycles > 0 ? cycles : 4);
        }
    }

    for (int i = 0; i < 64; i++)
        CHECK_MSG(direct_trace[i] == restored_trace[i],
                  "step %d diverged: %02X vs %02X", i, direct_trace[i],
                  restored_trace[i]);

    restored.cart.rom = NULL; /* borrowed from the fixture */
    restored.cart.ram = NULL;
    fixture_destroy(&f);
    remove(path);
}

static void t_state_saves_cartridge_ram(void)
{
    Fixture f = fixture_create(0x8000, 0x03); /* MBC1 + RAM */
    const char *path = test_temp_path("state.bin");
    GB restored;

    gb_write(&f.gb, 0x0000, 0x0A);
    gb_write(&f.gb, 0xA000, 0x11);
    gb_write(&f.gb, 0xA001, 0x22);

    CHECK(gb_save_state(&f.gb, path));

    memset(&restored, 0, sizeof(restored));
    restored.cart = f.cart;
    CHECK(gb_load_state(&restored, path));

    CHECK_EQ(restored.cart.ram[0], 0x11);
    CHECK_EQ(restored.cart.ram[1], 0x22);
    CHECK_EQ(restored.cart.ram_enabled, true);

    restored.cart.rom = NULL; /* borrowed from the fixture */
    restored.cart.ram = NULL;
    fixture_destroy(&f);
    remove(path);
}

static void t_state_saves_mapper_registers(void)
{
    Fixture f = fixture_create(0x100000, 0x1B); /* MBC5 */
    const char *path = test_temp_path("state.bin");
    GB restored;

    gb_write(&f.gb, 0x2000, 0x34);
    gb_write(&f.gb, 0x3000, 0x01);
    gb_write(&f.gb, 0x4000, 0x03);
    CHECK(gb_save_state(&f.gb, path));

    memset(&restored, 0, sizeof(restored));
    restored.cart = f.cart;
    CHECK(gb_load_state(&restored, path));

    CHECK_EQ(restored.cart.bank1_mbc5, 0x34);
    CHECK_EQ(restored.cart.bank2_mbc5, 0x01);
    CHECK_EQ(restored.cart.bank2, 0x03);

    /* Bank 0x134 masks down to 52 in a 1 MiB (64 bank) image, so that is
     * the byte that should now be visible at $4000. */
    CHECK_EQ(gb_read(&restored, 0x4000), f.cart.rom[52 * GB_ROM_BANK_SIZE]);

    restored.cart.rom = NULL; /* borrowed from the fixture */
    restored.cart.ram = NULL;
    fixture_destroy(&f);
    remove(path);
}

static void t_state_saves_mbc1_mode(void)
{
    Fixture f = fixture_create(0x100000, 0x01);
    const char *path = test_temp_path("state.bin");
    GB restored;

    gb_write(&f.gb, 0x6000, 0x01);
    CHECK(gb_save_state(&f.gb, path));

    memset(&restored, 0, sizeof(restored));
    restored.cart = f.cart;
    CHECK(gb_load_state(&restored, path));
    CHECK_EQ(restored.cart.mode, 1);

    restored.cart.rom = NULL; /* borrowed from the fixture */
    restored.cart.ram = NULL;
    fixture_destroy(&f);
    remove(path);
}

static void t_state_saves_interrupt_state(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const char *path = test_temp_path("state.bin");
    GB restored;

    f.gb.cpu.ime = true;
    f.gb.cpu.halted = true;
    f.gb.iflag = 0x05;
    f.gb.ie = 0x0F;
    CHECK(gb_save_state(&f.gb, path));

    memset(&restored, 0, sizeof(restored));
    restored.cart = f.cart;
    CHECK(gb_load_state(&restored, path));

    CHECK_EQ(restored.cpu.ime, true);
    CHECK_EQ(restored.cpu.halted, true);
    CHECK_EQ(restored.iflag, 0x05);
    CHECK_EQ(restored.ie, 0x0F);

    restored.cart.rom = NULL; /* borrowed from the fixture */
    restored.cart.ram = NULL;
    fixture_destroy(&f);
    remove(path);
}

static void t_state_saves_apu_state(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const char *path = test_temp_path("state.bin");
    GB restored;

    gb_write(&f.gb, 0xFF11, 0x80);
    gb_write(&f.gb, 0xFF12, 0xF3);
    gb_write(&f.gb, 0xFF13, 0x21);
    gb_write(&f.gb, 0xFF14, 0x86);
    apu_tick(&f.gb, 5000);

    CHECK(gb_save_state(&f.gb, path));

    memset(&restored, 0, sizeof(restored));
    restored.cart = f.cart;
    CHECK(gb_load_state(&restored, path));

    CHECK_EQ(restored.apu.ch1_on, f.gb.apu.ch1_on);
    CHECK_EQ(restored.apu.ch1_vol, f.gb.apu.ch1_vol);
    CHECK_EQ(restored.apu.ch1_freq, f.gb.apu.ch1_freq);
    CHECK_EQ(restored.apu.fs_step, f.gb.apu.fs_step);
    CHECK_EQ(restored.apu.nr12, f.gb.apu.nr12);

    restored.cart.rom = NULL; /* borrowed from the fixture */
    restored.cart.ram = NULL;
    fixture_destroy(&f);
    remove(path);
}

static void t_state_rejects_a_missing_file(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const char *absent = test_temp_path("definitely_absent.bin");

    remove(absent);
    CHECK(!gb_load_state(&f.gb, absent));
    fixture_destroy(&f);
}

static void t_state_rejects_a_bad_header(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const char *path = test_temp_path("state.bin");
    FILE *file = platform_fopen(path, "wb");
    uint8_t junk[64];

    memset(junk, 0xCD, sizeof(junk));
    if (!file) {
        FAIL("could not create the temporary state file");
        fixture_destroy(&f);
        return;
    }
    fwrite(junk, 1, sizeof(junk), file);
    fclose(file);

    CHECK(!gb_load_state(&f.gb, path));
    fixture_destroy(&f);
    remove(path);
}

static void t_state_survives_a_partial_frame(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const char *path = test_temp_path("state.bin");
    GB restored;

    /* Stop part way through a frame, which is the interesting case. */
    for (int i = 0; i < 37; i++)
        ppu_tick(&f.gb, 456);
    CHECK_EQ(f.gb.ppu.ly, 37);

    CHECK(gb_save_state(&f.gb, path));

    memset(&restored, 0, sizeof(restored));
    restored.cart = f.cart;
    CHECK(gb_load_state(&restored, path));
    CHECK_EQ(restored.ppu.ly, 37);
    CHECK_EQ(restored.ppu.mode, f.gb.ppu.mode);
    CHECK_EQ(restored.ppu.dot, f.gb.ppu.dot);

    restored.cart.rom = NULL; /* borrowed from the fixture */
    restored.cart.ram = NULL;
    fixture_destroy(&f);
    remove(path);
}

const TestCase state_tests[] = {
    { "state", "round trip", t_state_round_trip },
    { "state", "continues identically", t_state_continues_identically },
    { "state", "saves cartridge ram", t_state_saves_cartridge_ram },
    { "state", "saves mapper registers", t_state_saves_mapper_registers },
    { "state", "saves mbc1 mode", t_state_saves_mbc1_mode },
    { "state", "saves interrupt state", t_state_saves_interrupt_state },
    { "state", "saves apu state", t_state_saves_apu_state },
    { "state", "survives a partial frame", t_state_survives_a_partial_frame },
    { "state", "rejects a missing file", t_state_rejects_a_missing_file },
    { "state", "rejects a bad header", t_state_rejects_a_bad_header },
    TEST_SUITE_END
};