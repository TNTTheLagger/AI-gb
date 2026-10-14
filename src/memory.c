/*
 * memory.c -- the CPU memory bus: ROM cartridge, VRAM, WRAM, OAM, HRAM
 * and the I/O register file.
 */
#include <string.h>

#include "gb.h"

/* ------------------------------------------------------------------ */
/* Cartridge / memory bank control                                     */
/* ------------------------------------------------------------------ */

/* Total number of ROM banks the cartridge image actually provides. */
static inline uint32_t cart_rom_banks(const Cart *c)
{
    return (uint32_t)(c->rom_size / GB_ROM_BANK_SIZE);
}

/* Resolve the bank visible at $0000-$3FFF. */
static uint32_t bank_at_zero(const Cart *c)
{
    if (c->mapper == MAPPER_MBC1 && c->mode == 1)
        return (uint32_t)((c->bank2 & 0x03) << 5) & (cart_rom_banks(c) - 1);
    return 0;
}

/* Resolve the bank visible at $4000-$7FFF. */
static uint32_t bank_at_switch(const Cart *c)
{
    uint32_t bank;

    switch (c->mapper) {
    case MAPPER_MBC1:
        if (c->mode == 1) {
            bank = ((uint32_t)(c->bank2 & 0x03) << 5) | (c->bank1 & 0x1F);
            if ((c->bank1 & 0x1F) == 0)
                bank |= 1; /* the low register can never hold zero */
        } else {
            bank = c->bank1 & 0x1F;
            if (bank == 0)
                bank = 1;
        }
        break;

    case MAPPER_MBC2:
        /* The single register holds 4 bits and lives at $2000-$3FFF. */
        bank = (uint32_t)(c->bank1 & 0x0F);
        if (bank == 0)
            bank = 1;
        break;

    case MAPPER_MBC3:
        bank = c->bank1 & 0x7F;
        if (bank == 0)
            bank = 1;
        break;

    case MAPPER_MBC5:
        bank = (uint32_t)c->bank1_mbc5 |
               ((uint32_t)(c->bank2_mbc5 & 0x01) << 8);
        break;

    default: /* plain ROM, or a mapper we do not model */
        bank = c->bank1 & 0x1F;
        if (bank == 0)
            bank = 1;
        break;
    }

    return bank & (cart_rom_banks(c) - 1);
}

static void rom_byte(GB *gb, uint16_t addr, uint8_t *out)
{
    uint32_t offset = addr & 0x3FFF;
    uint32_t bank = (addr < 0x4000) ? bank_at_zero(&gb->cart)
                                    : bank_at_switch(&gb->cart);
    size_t pos = (size_t)bank * GB_ROM_BANK_SIZE + offset;

    if (pos >= gb->cart.rom_size)
        pos %= gb->cart.rom_size ? gb->cart.rom_size : 1;
    *out = gb->cart.rom ? gb->cart.rom[pos] : 0xFF;
}

static uint8_t cart_ram_read(GB *gb, uint16_t addr)
{
    Cart *c = &gb->cart;
    size_t offset = addr & 0x1FFF;

    if (!c->ram || !c->ram_enabled || !c->has_ram)
        return 0xFF;

    if (c->mapper == MAPPER_MBC2) {
        /* MBC2 ships 512 nibbles; the unused upper bits read back as 1. */
        if (offset >= 0x0200)
            return 0xFF;
        return c->ram[offset] | 0xF0;
    }

    if (c->mapper == MAPPER_MBC3 && c->has_rtc && c->rtc_sel >= 0x08 &&
        c->rtc_sel <= 0x0C) {
        switch (c->rtc_sel) {
        case 0x08: return (uint8_t)c->rtc_reg_latched[0];            /* seconds */
        case 0x09: return (uint8_t)c->rtc_reg_latched[1];            /* minutes */
        case 0x0A: return (uint8_t)c->rtc_reg_latched[2];            /* hours   */
        case 0x0B: return (uint8_t)c->rtc_reg_latched[3];            /* days    */
        case 0x0C: return (uint8_t)(c->rtc_reg_latched[4] | 0x80);   /* ctrl    */
        default: return 0xFF;
        }
    }

    {
        uint32_t bank = (c->mapper == MAPPER_MBC1 && c->mode == 0)
                            ? 0u
                            : (uint32_t)(c->bank2 & 0x03);
        size_t pos = (size_t)bank * GB_CART_RAM_SIZE + offset;
        if (pos >= c->ram_size)
            pos = offset;
        if (pos >= c->ram_size)
            return 0xFF;
        return c->ram[pos];
    }
}

static void cart_ram_write(GB *gb, uint16_t addr, uint8_t value)
{
    Cart *c = &gb->cart;
    size_t offset = addr & 0x1FFF;

    if (!c->ram || !c->ram_enabled || !c->has_ram)
        return;

    if (c->mapper == MAPPER_MBC2) {
        if (offset < 0x0200)
            c->ram[offset] = value & 0x0F; /* only a nibble of storage */
        return;
    }

    if (c->mapper == MAPPER_MBC3 && c->has_rtc && c->rtc_sel >= 0x08 &&
        c->rtc_sel <= 0x0C) {
        switch (c->rtc_sel) {
        case 0x08: c->rtc_sec = (value % 60); break;
        case 0x09: c->rtc_min = (value % 60); break;
        case 0x0A: c->rtc_hour = (value % 24); break;
        case 0x0B: c->rtc_day = value; break;
        case 0x0C: c->rtc_day_high = value & 0xC1;
                   if (!(value & 0x40))
                       c->rtc_latch = 0;
                   break;
        default: break;
        }
        return;
    }

    {
        uint32_t bank = (c->mapper == MAPPER_MBC1 && c->mode == 0)
                            ? 0u
                            : (uint32_t)(c->bank2 & 0x03);
        size_t pos = (size_t)bank * GB_CART_RAM_SIZE + offset;
        if (pos >= c->ram_size)
            pos = offset;
        if (pos < c->ram_size)
            c->ram[pos] = value;
    }
}

/* ------------------------------------------------------------------ */
/* Bus reads / writes                                                  */
/* ------------------------------------------------------------------ */

/*
 * The source side of an OAM transfer.
 *
 * The DMA controller reads the memory map much as the CPU does, with two
 * differences: it is not locked out of VRAM by the PPU, and sources from
 * $E000 up read the work RAM mirror rather than OAM or I/O.  Work RAM at
 * $C000-$DFFF is the usual source, and it reads exactly as you would expect.
 */
static uint8_t oamdma_source(GB *gb, uint16_t addr)
{
    if (addr < 0x8000) {
        uint8_t value;

        rom_byte(gb, addr, &value);
        return value;
    }
    if (addr < 0xA000)
        return gb->vram[addr & 0x1FFF];
    if (addr < 0xC000)
        return cart_ram_read(gb, addr);

    return gb->wram[(addr - 0xC000) & 0x1FFF];
}

void oamdma_tick(GB *gb, int cycles)
{
    OAMDMA *dma = &gb->oam_dma;

    if (!dma->active)
        return;

    dma->cycles -= cycles;

    while (dma->cycles < 0) {
        dma->cycles += 4;
        if (dma->index >= GB_OAM_SIZE)
            break;
        gb->oam[dma->index] =
            oamdma_source(gb, (uint16_t)(dma->src + dma->index));
        dma->index++;
    }

    if (dma->index >= GB_OAM_SIZE) {
        dma->active = false;
        dma->index = 0;
    }
}

uint8_t gb_read(GB *gb, uint16_t addr)
{
    switch (addr & 0xF000) {
    case 0x0000:
    case 0x1000:
    case 0x2000:
    case 0x3000:
    case 0x4000:
    case 0x5000:
    case 0x6000:
    case 0x7000: {
        uint8_t v;
        rom_byte(gb, addr, &v);
        return v;
    }

    case 0x8000:
    case 0x9000:
        /* Open bus while the PPU owns VRAM, which is mode 3 only. */
        if (!ppu_cpu_can_read_vram(gb))
            return 0xFF;
        return gb->vram[addr & 0x1FFF];

    case 0xA000:
    case 0xB000:
        return cart_ram_read(gb, addr);

    case 0xC000:
    case 0xD000:
        return gb->wram[addr & 0x1FFF];

    case 0xE000:
    case 0xF000:
        /* $E000-$FDFF mirrors $C000-$DDFF; $FE00+ has its own devices. */
        if (addr < GB_MEM_OAM)
            return gb->wram[addr & 0x1FFF];
        break;

    default:
        break;
    }

    if (addr >= GB_MEM_OAM && addr < GB_MEM_OAM + GB_OAM_SIZE) {
        /* Open bus whenever the PPU owns OAM: the object scan and the
         * transfer, that is modes 2 and 3. */
        if (!ppu_cpu_can_read_oam(gb))
            return 0xFF;
        return gb->oam[addr - GB_MEM_OAM];
    }

    if (addr < GB_MEM_IO)
        return 0xFF; /* $FEA0-$FEFF does not exist */
    if (addr < GB_MEM_HRAM)
        return io_read(gb, addr);

    if (addr == GB_MEM_IE)
        return gb->ie;
    return gb->hram[addr - GB_MEM_HRAM];
}

void gb_write(GB *gb, uint16_t addr, uint8_t value)
{
    switch (addr & 0xF000) {
    case 0x0000:
    case 0x1000:
    case 0x2000:
    case 0x3000: {
        /* $0000-$1FFF enables cartridge RAM; $2000-$3FFF selects the ROM
         * bank, and how wide that bank number is depends on the mapper. */
        Cart *c = &gb->cart;

        if (c->mapper == MAPPER_MBC2) {
            if (addr < 0x2000)
                c->ram_enabled = (value & 0x0F) == 0x0A;
            else
                c->bank1 = (uint8_t)(value & 0x0F);
        } else if (addr < 0x2000) {
            c->ram_enabled = (value & 0x0F) == 0x0A;
        } else if (c->mapper == MAPPER_MBC5) {
            /* MBC5 splits the bank number across two registers and, unlike
             * the older mappers, allows bank zero. */
            if (addr < 0x3000)
                c->bank1_mbc5 = value;
            else
                c->bank2_mbc5 = (uint8_t)(value & 0x01);
        } else if (c->mapper == MAPPER_MBC3) {
            c->bank1 = (uint8_t)(value & 0x7F);
            if (c->bank1 == 0)
                c->bank1 = 1;
        } else {
            /* MBC1, and anything we do not recognise, keeps five bits. */
            c->bank1 = (uint8_t)(value & 0x1F);
            if (c->bank1 == 0)
                c->bank1 = 1;
        }
        return;
    }

    case 0x4000:
    case 0x5000:
    case 0x6000:
    case 0x7000: {
        Cart *c = &gb->cart;
        if (addr < 0x6000) { /* $4000-$5FFF secondary register */
            switch (c->mapper) {
            case MAPPER_MBC1:
            case MAPPER_MBC2:
                c->bank2 = (uint8_t)(value & 0x03);
                break;
            case MAPPER_MBC3:
                if (c->has_rtc && value >= 0x08 && value <= 0x0C)
                    c->rtc_sel = (uint8_t)value;
                else
                    c->bank2 = (uint8_t)(value & 0x03);
                break;
            case MAPPER_MBC5:
                c->bank2 = (uint8_t)(value & 0x0F);
                break;
            default:
                c->bank2 = (uint8_t)(value & 0x03);
                break;
            }
        } else { /* $6000-$7FFF MBC1 mode select, MBC3 RTC latch */
            switch (c->mapper) {
            case MAPPER_MBC1:
                c->mode = (uint8_t)(value & 0x01);
                break;
            case MAPPER_MBC3:
                if (c->has_rtc && value == 0x00 && c->rtc_latch == 0x01) {
                    /* Latch the live clock into the shadow registers. */
                    c->rtc_reg_latched[0] = (uint8_t)c->rtc_sec;
                    c->rtc_reg_latched[1] = (uint8_t)c->rtc_min;
                    c->rtc_reg_latched[2] = (uint8_t)c->rtc_hour;
                    c->rtc_reg_latched[3] = (uint8_t)(c->rtc_day & 0xFF);
                    c->rtc_reg_latched[4] = (uint8_t)(c->rtc_day_high & 0x01);
                    c->rtc_latch = 0x02;
                } else if (c->has_rtc && value == 0x01) {
                    c->rtc_latch = 0x01;
                }
                break;
            default:
                break;
            }
        }
        return;
    }

    case 0x8000:
    case 0x9000:
        /* The write is lost rather than delayed. */
        if (!ppu_cpu_can_read_vram(gb))
            return;
        gb->vram[addr & 0x1FFF] = value;
        return;

    case 0xA000:
    case 0xB000:
        cart_ram_write(gb, addr, value);
        return;

    case 0xC000:
    case 0xD000:
        gb->wram[addr & 0x1FFF] = value;
        return;

    case 0xE000:
    case 0xF000:
        /* $E000-$FDFF mirrors $C000-$DDFF; $FE00 onwards has its own
         * devices, so fall through to the dispatch below. */
        if (addr < GB_MEM_OAM)
            gb->wram[addr & 0x1FFF] = value;
        break;

    default:
        break;
    }

    if (addr >= GB_MEM_OAM && addr < GB_MEM_OAM + GB_OAM_SIZE) {
        /* Lost during an active DMA transfer, and whenever the PPU has OAM
         * to itself. */
        if (gb->oam_dma.active || !ppu_cpu_can_read_oam(gb))
            return;
        gb->oam[addr - GB_MEM_OAM] = value;
        return;
    }

    if (addr < GB_MEM_IO)
        return; /* $FEA0-$FEFF: writes are discarded */
    if (addr < GB_MEM_HRAM) {
        io_write(gb, addr, value);
        return;
    }

    if (addr == GB_MEM_IE) {
        gb->ie = value;
        return;
    }
    gb->hram[addr - GB_MEM_HRAM] = value;
}