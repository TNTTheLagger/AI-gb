/*
 * fixture.c -- builds machines for the tests to poke at.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "platform.h"

Fixture fixture_create(size_t rom_size, uint8_t type_code)
{
    Fixture f;
    size_t banks = 1;
    size_t padded;
    bool has_ram = false, has_rtc = false;

    while (banks * GB_ROM_BANK_SIZE < rom_size)
        banks <<= 1;
    padded = banks * GB_ROM_BANK_SIZE;

    memset(&f, 0, sizeof(f));

    f.cart.rom = (uint8_t *)calloc(1, padded);
    if (!f.cart.rom)
        abort();
    f.cart.rom_size = padded;

    switch (type_code) {
    case 0x00: f.cart.mapper = MAPPER_ROM; break;
    case 0x08: case 0x09: f.cart.mapper = MAPPER_ROM; has_ram = true; break;
    case 0x01: case 0x02: case 0x03: f.cart.mapper = MAPPER_MBC1; has_ram = true; break;
    case 0x05: case 0x06: f.cart.mapper = MAPPER_MBC2; has_ram = true; break;
    case 0x0F: case 0x10:
        f.cart.mapper = MAPPER_MBC3;
        has_ram = true;
        has_rtc = true;
        break;
    case 0x11: case 0x12: case 0x13:
        f.cart.mapper = MAPPER_MBC3;
        has_ram = true;
        break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
        f.cart.mapper = MAPPER_MBC5;
        has_ram = true;
        break;
    default:
        f.cart.mapper = MAPPER_UNKNOWN;
        break;
    }

    if (!has_ram) {
        f.cart.ram_size = 0;
    } else if (f.cart.mapper == MAPPER_MBC2) {
        f.cart.ram_size = 512; /* 512 nibbles, always built in */
    } else {
        /* Four banks, so the RAM banking tests have somewhere to go. */
        f.cart.ram_size = GB_CART_RAM_SIZE * 4;
    }

    f.cart.has_ram = f.cart.ram_size > 0;
    f.cart.has_rtc = has_rtc;
    f.cart.type_code = type_code;

    if (f.cart.has_ram) {
        f.cart.ram = (uint8_t *)calloc(1, f.cart.ram_size);
        if (!f.cart.ram)
            abort();
    }

    f.cart.bank1 = 1;
    f.cart.bank2 = 0;
    f.cart.bank1_mbc5 = 1;
    f.cart.bank2_mbc5 = 0;
    f.cart.mode = 0;
    f.cart.ram_enabled = false;

    gb_init(&f.gb, &f.cart);
    f.gb.iflag = 0;
    f.gb.ie = 0;

    /* Tests should not have to care about the post boot register values. */
    f.gb.cpu.a = f.gb.cpu.f = 0;
    f.gb.cpu.b = f.gb.cpu.c = 0;
    f.gb.cpu.d = f.gb.cpu.e = 0;
    f.gb.cpu.h = f.gb.cpu.l = 0;
    return f;
}

void fixture_destroy(Fixture *f)
{
    gb_free_cart(&f->cart);
    f->cart.rom = NULL;
    f->cart.ram = NULL;
}

void fixture_header(Fixture *f, uint8_t type_code, uint8_t rom_code,
                    uint8_t ram_code)
{
    uint8_t *rom = f->cart.rom;
    const char *title = "TESTCART";

    memset(rom + 0x134, 0, 16);
    memcpy(rom + 0x134, title, strlen(title));

    rom[0x143] = 0x00; /* not a Game Boy Color cart */
    rom[0x146] = 0x00;
    rom[0x147] = type_code;
    rom[0x148] = rom_code;
    rom[0x149] = ram_code;
    rom[0x14A] = 0x01; /* Japanese */
    rom[0x14B] = 0x33; /* version */

    /* The entry point jumps over the header, as a real cartridge does. */
    rom[0x100] = 0x00; /* NOP          */
    rom[0x101] = 0xC3; /* JP $0150     */
    rom[0x102] = 0x50;
    rom[0x103] = 0x01;

    f->cart.type_code = type_code;
    f->cart.rom_size_code = rom_code;
    f->cart.ram_size_code = ram_code;
}

void fixture_write(Fixture *f, uint16_t addr, const uint8_t *data, size_t len)
{
    size_t pos = (size_t)addr;

    if (pos >= f->cart.rom_size)
        return;
    if (pos + len > f->cart.rom_size)
        len = f->cart.rom_size - pos;
    memcpy(f->cart.rom + pos, data, len);
}

void fixture_byte(Fixture *f, uint16_t addr, uint8_t value)
{
    if (addr < f->cart.rom_size)
        f->cart.rom[addr] = value;
}

void fixture_word(Fixture *f, uint16_t addr, uint16_t value)
{
    fixture_byte(f, addr, (uint8_t)(value & 0xFF));
    fixture_byte(f, (uint16_t)(addr + 1), (uint8_t)(value >> 8));
}

int fixture_step(Fixture *f)
{
    int cycles = cpu_step(&f->gb);

    if (cycles <= 0)
        cycles = 4;
    gb_tick(&f->gb, cycles);
    return cycles;
}

void fixture_load(Fixture *f, const uint8_t *code, size_t len)
{
    CPU *c = &f->gb.cpu;

    memset(f->cart.rom, 0, 0x100);
    fixture_write(f, 0x0100, code, len);

    /* Point the CPU at the program but leave the registers as the test
     * arranged them. */
    c->pc = 0x0100;
    c->sp = 0xFFFE;
    c->ime = false;
    c->ime_delay = 0;
    c->halted = false;
    c->stopped = false;
    c->halt_bug = false;
    c->idle_cycles = 0;

    f->gb.ie = 0;
    f->gb.iflag = 0;
}

void fixture_exec(Fixture *f, int steps)
{
    for (int i = 0; i < steps; i++)
        fixture_step(f);
}

void fixture_run(Fixture *f, const uint8_t *code, size_t len, int steps)
{
    fixture_load(f, code, len);
    fixture_exec(f, steps);
}

int fixture_run_op(Fixture *f, uint8_t opcode)
{
    memset(f->cart.rom, 0, 8);
    f->cart.rom[0x100] = opcode;

    f->gb.cpu.pc = 0x0100;
    f->gb.cpu.halted = false;
    f->gb.cpu.stopped = false;
    f->gb.cpu.halt_bug = false;

    return fixture_step(f);
}
/* ------------------------------------------------------------------ */
/* Scratch files                                                       */
/* ------------------------------------------------------------------ */

const char *test_temp_path(const char *name)
{
    /* A small rotating pool, so a test can hold on to two of these at once. */
    static char pool[4][1024];
    static int next;
    char *path = pool[next];
    const char *dir = platform_temp_dir();
    size_t i = 0;
    int slot = next;

    next = (next + 1) & 3;

    for (const char *p = dir; *p && i < sizeof(pool[0]) - 2; p++)
        path[i++] = *p;

    /* Normalise the separator, since Windows paths may use a backslash. */
    if (i == 0 || (path[i - 1] != '/' && path[i - 1] != '\\'))
        path[i++] = '/';

    path[i] = '\0';
    snprintf(path + i, sizeof(pool[0]) - i, "gbtest_%s", name);

    (void)slot;
    return path;
}
