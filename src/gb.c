/*
 * gb.c -- assembling the machine, running frames and saving state.
 */
#include <stdio.h>
#include <string.h>

#include "gb.h"
#include "platform.h"

/* Post boot register values the DMG boot ROM leaves behind. */
#define BOOT_AF 0x01B0
#define BOOT_BC 0x0013
#define BOOT_DE 0x00D8
#define BOOT_HL 0x014D
#define BOOT_SP 0xFFFE

void gb_reset(GB *gb)
{
    memset(&gb->cpu, 0, sizeof(gb->cpu));
    memset(gb->vram, 0, sizeof(gb->vram));
    memset(gb->wram, 0, sizeof(gb->wram));
    memset(gb->oam, 0, sizeof(gb->oam));
    memset(gb->hram, 0, sizeof(gb->hram));

    gb->cpu.a = (uint8_t)(BOOT_AF >> 8);
    gb->cpu.f = (uint8_t)(BOOT_AF & 0xF0);
    gb->cpu.b = (uint8_t)(BOOT_BC >> 8);
    gb->cpu.c = (uint8_t)(BOOT_BC & 0xFF);
    gb->cpu.d = (uint8_t)(BOOT_DE >> 8);
    gb->cpu.e = (uint8_t)(BOOT_DE & 0xFF);
    gb->cpu.h = (uint8_t)(BOOT_HL >> 8);
    gb->cpu.l = (uint8_t)(BOOT_HL & 0xFF);
    gb->cpu.sp = BOOT_SP;
    gb->cpu.pc = 0x0100; /* the boot ROM jumps here once it is done */

    gb->ie = 0x00;
    gb->iflag = 0xE1;
    gb->boot_rom_disabled = true;
    gb->cycles = 0;

    memset(&gb->joypad, 0, sizeof(gb->joypad));
    memset(&gb->serial, 0, sizeof(gb->serial));
    memset(&gb->oam_dma, 0, sizeof(gb->oam_dma));
    gb->serial.counter = 8 * 512;
    /* No buttons are held, so all four column lines start out high. */
    gb->joypad.prev_line = 0x0F;

    timer_reset(gb);
    ppu_reset(gb);
    apu_reset(gb);

    gb->cart.ram_enabled = false;
    gb->cart.bank1 = 1;
    gb->cart.mode = 0;
    if (gb->cart.mapper == MAPPER_MBC5) {
        gb->cart.bank1_mbc5 = 1;
        gb->cart.bank2_mbc5 = 0;
    }
}

bool gb_init(GB *gb, Cart *cart)
{
    memset(gb, 0, sizeof(*gb));
    gb->cart = *cart;
    gb_reset(gb);
    return true;
}

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

void gb_tick(GB *gb, int cycles)
{
    gb->cycles += (uint64_t)cycles;

    /* A joypad press releases the CPU from STOP. */
    if (gb->cpu.stopped && (gb->iflag & GB_INT_JOYPAD))
        gb->cpu.stopped = false;

    timer_tick(gb, cycles);
    apu_tick(gb, cycles);
    ppu_tick(gb, cycles);
    serial_tick(gb, cycles);
    oamdma_tick(gb, cycles);
}

void gb_run_frame(GB *gb)
{
    /* Cap the work done per call so a runaway PC cannot hang the host. */
    uint64_t budget = GB_CYCLES_PER_FRAME * 4;

    gb->ppu.frame_done = false;

    while (!gb->ppu.frame_done && budget > 0) {
        int cycles = cpu_step(gb);

        if (cycles <= 0)
            cycles = 4;
        gb_tick(gb, cycles);
        budget -= (uint64_t)cycles;
    }

    /* Drain audio that piled up while we were busy so the frontend never
     * falls behind the machine. */
    while (apu_available(&gb->apu) > APU_RING_SIZE / 2) {
        int16_t scratch[256];
        apu_drain(&gb->apu, scratch, 128);
    }
}

/* ------------------------------------------------------------------ */
/* Save states                                                         */
/* ------------------------------------------------------------------ */

#define STATE_MAGIC 0x47425354u /* "GBST" */
#define STATE_VERSION 3u

static bool wr(FILE *f, const void *data, size_t len)
{
    return fwrite(data, 1, len, f) == len;
}

static bool rd(FILE *f, void *data, size_t len)
{
    return fread(data, 1, len, f) == len;
}

bool gb_save_state(const GB *gb, const char *path)
{
    FILE *f = platform_fopen(path, "wb");
    uint32_t magic = STATE_MAGIC;
    uint32_t version = STATE_VERSION;
    bool ok = true;

    if (!f)
        return false;

    ok &= wr(f, &magic, sizeof(magic));
    ok &= wr(f, &version, sizeof(version));

    /* Every device sits between `cpu` and `cart` in the GB struct, so one
     * block covers the CPU, PPU, timer, joypad, serial, APU and OAM DMA. */
    ok &= wr(f, &gb->cpu, offsetof(GB, cart));
    ok &= wr(f, gb->vram, sizeof(gb->vram));
    ok &= wr(f, gb->wram, sizeof(gb->wram));
    ok &= wr(f, gb->oam, sizeof(gb->oam));
    ok &= wr(f, gb->hram, sizeof(gb->hram));
    ok &= wr(f, &gb->ie, sizeof(gb->ie));
    ok &= wr(f, &gb->iflag, sizeof(gb->iflag));

    /* MBC state (no ROM image, no RAM contents). */
    ok &= wr(f, &gb->cart.bank1, sizeof(gb->cart.bank1));
    ok &= wr(f, &gb->cart.bank2, sizeof(gb->cart.bank2));
    ok &= wr(f, &gb->cart.bank1_mbc5, sizeof(gb->cart.bank1_mbc5));
    ok &= wr(f, &gb->cart.bank2_mbc5, sizeof(gb->cart.bank2_mbc5));
    ok &= wr(f, &gb->cart.mode, sizeof(gb->cart.mode));
    ok &= wr(f, &gb->cart.ram_enabled, sizeof(gb->cart.ram_enabled));

    if (gb->cart.ram && gb->cart.ram_size)
        ok &= wr(f, gb->cart.ram, gb->cart.ram_size);

    fclose(f);
    return ok;
}

bool gb_load_state(GB *gb, const char *path)
{
    FILE *f = platform_fopen(path, "rb");
    uint32_t magic = 0, version = 0;
    bool ok = true;

    if (!f)
        return false;

    ok = rd(f, &magic, sizeof(magic)) && rd(f, &version, sizeof(version));
    if (!ok || magic != STATE_MAGIC || version != STATE_VERSION) {
        fclose(f);
        return false;
    }

    ok &= rd(f, &gb->cpu, offsetof(GB, cart));
    ok &= rd(f, gb->vram, sizeof(gb->vram));
    ok &= rd(f, gb->wram, sizeof(gb->wram));
    ok &= rd(f, gb->oam, sizeof(gb->oam));
    ok &= rd(f, gb->hram, sizeof(gb->hram));
    ok &= rd(f, &gb->ie, sizeof(gb->ie));
    ok &= rd(f, &gb->iflag, sizeof(gb->iflag));

    ok &= rd(f, &gb->cart.bank1, sizeof(gb->cart.bank1));
    ok &= rd(f, &gb->cart.bank2, sizeof(gb->cart.bank2));
    ok &= rd(f, &gb->cart.bank1_mbc5, sizeof(gb->cart.bank1_mbc5));
    ok &= rd(f, &gb->cart.bank2_mbc5, sizeof(gb->cart.bank2_mbc5));
    ok &= rd(f, &gb->cart.mode, sizeof(gb->cart.mode));
    ok &= rd(f, &gb->cart.ram_enabled, sizeof(gb->cart.ram_enabled));

    if (gb->cart.ram && gb->cart.ram_size)
        ok &= rd(f, gb->cart.ram, gb->cart.ram_size);

    fclose(f);
    if (!ok)
        return false;

    /* Force the audio ring back to empty so playback resumes cleanly. */
    gb->apu.ring_read = 0;
    gb->apu.ring_write = 0;
    gb->apu.dropped = 0;
    gb->apu.acc = 0;
    gb->apu.cap_left = 0.0;
    gb->apu.cap_right = 0.0;

    return true;
}