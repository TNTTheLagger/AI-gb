/*
 * io.c -- the $FF00-$FF7F register file: it decodes writes into the
 * individual devices and builds the read-back image of each register.
 */
#include <string.h>

#include "gb.h"

/* Interrupt request helper used by every device. */
void gb_request_interrupt(GB *gb, uint8_t bit)
{
    gb->iflag |= (uint8_t)(1u << bit);
}

uint8_t io_read(GB *gb, uint16_t addr)
{
    switch (addr) {
    /* ---- joypad -------------------------------------------------- */
    case 0xFF00:
        return joypad_read(gb);

    /* ---- serial -------------------------------------------------- */
    case 0xFF01:
        return gb->serial.sb;
    case 0xFF02:
        return (uint8_t)(0x7E | (gb->serial.sc & 0x81));

    /* ---- timer ---------------------------------------------------- */
    case 0xFF04:
        return (uint8_t)(gb->timer.div >> 8);
    case 0xFF05:
        return gb->timer.tima;
    case 0xFF06:
        return gb->timer.tma;
    case 0xFF07:
        return (uint8_t)(0xF8 | (gb->timer.tac & 0x07));

    /* ---- interrupts ----------------------------------------------- */
    case 0xFF0F:
        return (uint8_t)(0xE0 | (gb->iflag & 0x1F));

    /* ---- sound ----------------------------------------------------- */
    case 0xFF10: return (uint8_t)(0x80 | gb->apu.nr10);
    case 0xFF11: return (uint8_t)(0x3F | gb->apu.nr11);
    case 0xFF12: return 0x00; /* write only */
    case 0xFF13: return (uint8_t)(0xFF | gb->apu.nr13);
    case 0xFF14: return (uint8_t)(0xBF | gb->apu.nr14);
    case 0xFF15: return 0xFF;
    case 0xFF16: return (uint8_t)(0x3F | gb->apu.nr21);
    case 0xFF17: return 0x00; /* write only */
    case 0xFF18: return (uint8_t)(0xFF | gb->apu.nr23);
    case 0xFF19: return (uint8_t)(0xBF | gb->apu.nr24);
    case 0xFF1A: return (uint8_t)(0x7F | gb->apu.nr30);
    case 0xFF1B: return (uint8_t)(0xFF | gb->apu.nr31);
    case 0xFF1C: return (uint8_t)(0x9F | gb->apu.nr32);
    case 0xFF1D: return (uint8_t)(0xFF | gb->apu.nr33);
    case 0xFF1E: return (uint8_t)(0xBF | gb->apu.nr34);
    case 0xFF1F: return 0xFF;              /* unused */
    case 0xFF20: return 0xFF;              /* NR41 is write only */
    case 0xFF21: return (uint8_t)(0x00 | gb->apu.nr42);
    case 0xFF22: return 0x00;              /* NR43 is write only */
    case 0xFF23: return (uint8_t)(0xBF | gb->apu.nr44);
    case 0xFF24: return (uint8_t)(0x00 | gb->apu.nr50);
    case 0xFF25: return (uint8_t)(0x00 | gb->apu.nr51);
    case 0xFF26: return (uint8_t)(0x70 | (gb->apu.powered ? 0x80 : 0x00) |
                                 (gb->apu.nr52 & 0x0F));

    /* ---- video ------------------------------------------------------ */
    case 0xFF40: return gb->ppu.lcdc;
    case 0xFF41:
        /*
         * Bit 7 always reads as 1, bits 6 to 3 are the interrupt enables
         * as written, bit 2 is a pure status bit that is set while LY and
         * LYC agree, and bits 1 to 0 report the real mode.
         */
        return (uint8_t)(0x80 | (gb->ppu.stat & 0x78) |
                         (gb->ppu.ly == gb->ppu.lyc ? 0x04 : 0x00) |
                         (gb->ppu.mode & 0x03));
    case 0xFF42: return gb->ppu.scy;
    case 0xFF43: return gb->ppu.scx;
    case 0xFF44: return gb->ppu.ly;
    case 0xFF45: return gb->ppu.lyc;
    case 0xFF46: return gb->ppu.dma;
    case 0xFF47: return gb->ppu.bgp;
    case 0xFF48: return gb->ppu.obp0;
    case 0xFF49: return gb->ppu.obp1;
    case 0xFF4A: return gb->ppu.wy;
    case 0xFF4B: return (uint8_t)(gb->ppu.wx & 0x7F);

    /* ---- CGB only registers, reported as open bus ------------------- */
    case 0xFF4F: return 0xFF;
    case 0xFF51: case 0xFF52: case 0xFF53: case 0xFF54: case 0xFF55:
    case 0xFF56: case 0xFF57: case 0xFF58: case 0xFF59: case 0xFF5A:
    case 0xFF5B: case 0xFF5C: case 0xFF5D: case 0xFF5E: case 0xFF5F:
    case 0xFF60: case 0xFF61: case 0xFF62: case 0xFF63: case 0xFF64:
    case 0xFF65: case 0xFF66: case 0xFF67: case 0xFF68: case 0xFF69:
    case 0xFF6A: case 0xFF6B:
        return 0xFF;
    case 0xFF70: return 0xFF;

    /* ---- interrupt enable ------------------------------------------ */
    case 0xFFFF:
        return gb->ie;

    default:
        break;
    }

    if (addr >= 0xFF30 && addr <= 0xFF3F)
        return gb->apu.wave[addr - 0xFF30];

    return 0xFF;
}

void io_write(GB *gb, uint16_t addr, uint8_t value)
{
    switch (addr) {
    /* ---- joypad ------------------------------------------------------ */
    case 0xFF00:
        /* Only the two select bits are writable. */
        gb->joypad.sel = (uint8_t)(value & 0x30);
        joypad_tick(gb); /* changing the matrix can raise an interrupt */
        break;

    /* ---- serial ------------------------------------------------------ */
    case 0xFF01: gb->serial.sb = value; break;
    case 0xFF02:
        gb->serial.sc = (uint8_t)(value & 0x81);
        gb->serial.counter = 8 * 512; /* 8 bits at 8192 Hz */
        break;

    /* ---- timer -------------------------------------------------------- */
    case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
        timer_write(gb, addr, value);
        break;

    /* ---- interrupts ---------------------------------------------------- */
    case 0xFF0F:
        gb->iflag = (uint8_t)(value & 0x1F);
        break;

    /* ---- sound ---------------------------------------------------------- */
    case 0xFF10: gb->apu.nr10 = (uint8_t)(value & 0x7F); break;
    case 0xFF11: gb->apu.nr11 = value; break;
    case 0xFF12: apu_write(gb, 0xFF12, value); break;
    case 0xFF13: gb->apu.nr13 = value; break;
    case 0xFF14: apu_write(gb, 0xFF14, value); break;
    case 0xFF16: gb->apu.nr21 = (uint8_t)(value & 0x3F); break;
    case 0xFF17: apu_write(gb, 0xFF17, value); break;
    case 0xFF18: gb->apu.nr23 = value; break;
    case 0xFF19: apu_write(gb, 0xFF19, value); break;
    case 0xFF1A: apu_write(gb, 0xFF1A, value); break;
    case 0xFF1B: gb->apu.nr31 = value; break;
    case 0xFF1C: gb->apu.nr32 = (uint8_t)(value & 0x60); break;
    case 0xFF33: gb->apu.nr33 = value; break;
    case 0xFF34: apu_write(gb, 0xFF34, value); break;
    case 0xFF20: gb->apu.nr41 = (uint8_t)(value & 0x3F); break;
    case 0xFF21: apu_write(gb, 0xFF21, value); break;
    case 0xFF22: gb->apu.nr43 = value; break;
    case 0xFF23: apu_write(gb, 0xFF23, value); break;
    case 0xFF24: gb->apu.nr50 = value; break;
    case 0xFF25: gb->apu.nr51 = value; break;
    case 0xFF26: apu_write(gb, 0xFF26, value); break;

    /* ---- video ----------------------------------------------------------- */
    case 0xFF40:
        /* Switching the LCD off or on restarts the frame from line zero. */
        if ((value & 0x80) == 0) {
            if (gb->ppu.lcdc & 0x80)
                ppu_lcd_off(gb);
            gb->ppu.lcdc = (uint8_t)(value & 0x7F);
        } else {
            if ((gb->ppu.lcdc & 0x80) == 0)
                ppu_lcd_on(gb);
            gb->ppu.lcdc = (uint8_t)(value | 0x80);
        }
        if ((gb->ppu.lcdc & 0x20) == 0)
            gb->ppu.window_line = 0;
        break;
    case 0xFF41:
        gb->ppu.stat = (uint8_t)((value & 0x78) | (gb->ppu.stat & 0x07));
        gb->ppu.stat &= 0x7F;
        ppu_update_stat(gb); /* a new source can fire straight away */
        break;
    case 0xFF42: gb->ppu.scy = value; break;
    case 0xFF43: gb->ppu.scx = value; break;
    case 0xFF44: break; /* LY is read only */
    case 0xFF45:
        gb->ppu.lyc = value;
        ppu_update_stat(gb);
        break;
    case 0xFF46: ppu_dma_start(gb, value); break;
    case 0xFF47: gb->ppu.bgp = value; break;
    case 0xFF48: gb->ppu.obp0 = value; break;
    case 0xFF49: gb->ppu.obp1 = value; break;
    case 0xFF4A: gb->ppu.wy = value; break;
    case 0xFF4B: gb->ppu.wx = (uint8_t)(value & 0x7F); break;

    /* ---- boot ROM switch ----------------------------------------------- */
    case 0xFF50:
        gb->boot_rom_disabled = (value & 0x01) != 0;
        break;

    default:
        break;
    }

    if (addr >= 0xFF30 && addr <= 0xFF3F) {
        gb->apu.wave[addr - 0xFF30] = value;
        memcpy(gb->apu.ch3_wave, gb->apu.wave, 16);
    }
}

/* OAM DMA moves one byte every four T-cycles, so a full 160 byte transfer
 * occupies 640 T-cycles.  The CPU stays busy but can run other code. */
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
        gb->oam[dma->index] = gb_read(gb, (uint16_t)(dma->src + dma->index));
        dma->index++;
    }

    if (dma->index >= GB_OAM_SIZE) {
        dma->active = false;
        dma->index = 0;
    }
}

void ppu_dma_start(GB *gb, uint8_t value)
{
    gb->ppu.dma = value;
    gb->oam_dma.active = true;
    gb->oam_dma.src = (uint16_t)(value << 8);
    gb->oam_dma.index = 0;
    gb->oam_dma.cycles = 0; /* the first byte moves on the next tick */
}