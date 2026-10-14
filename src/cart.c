/*
 * cart.c -- reading a ROM image off disk and decoding its header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb.h"
#include "platform.h"

static Mapper mapper_from_code(uint8_t type_code, bool *has_ram, bool *has_battery,
                               bool *has_rtc)
{
    *has_ram = false;
    *has_battery = false;
    *has_rtc = false;

    switch (type_code) {
    case 0x00: return MAPPER_ROM;   /* ROM only                       */
    case 0x01: return MAPPER_MBC1;  /* MBC1                           */
    case 0x02: *has_ram = true; return MAPPER_MBC1;   /* MBC1 + RAM          */
    case 0x03: *has_ram = true; *has_battery = true; return MAPPER_MBC1;
    case 0x05: return MAPPER_MBC2;  /* MBC2                           */
    case 0x06: *has_ram = true; *has_battery = true; return MAPPER_MBC2;
    case 0x08: *has_ram = true; return MAPPER_ROM;    /* ROM + RAM           */
    case 0x09: *has_ram = true; *has_battery = true; return MAPPER_ROM;
    case 0x0F: *has_battery = true; *has_rtc = true; return MAPPER_MBC3;
    case 0x10:
        *has_ram = true;
        *has_battery = true;
        *has_rtc = true;
        return MAPPER_MBC3;
    case 0x11: return MAPPER_MBC3;  /* MBC3                           */
    case 0x12: *has_ram = true; return MAPPER_MBC3;   /* MBC3 + RAM          */
    case 0x13: *has_ram = true; *has_battery = true; return MAPPER_MBC3;
    case 0x19: return MAPPER_MBC5;  /* MBC5                           */
    case 0x1A: *has_ram = true; return MAPPER_MBC5;   /* MBC5 + RAM          */
    case 0x1B: *has_ram = true; *has_battery = true; return MAPPER_MBC5;
    case 0x1C: *has_battery = true; return MAPPER_MBC5; /* MBC5 + rumble      */
    case 0x1D: *has_ram = true; *has_battery = true; return MAPPER_MBC5;
    case 0x1E: *has_ram = true; *has_battery = true; return MAPPER_MBC5;
    default: break;
    }
    return MAPPER_UNKNOWN;
}

static size_t ram_size_from_code(uint8_t code, Mapper mapper, bool has_rtc)
{
    if (mapper == MAPPER_MBC2)
        return 512; /* MBC2 carries 512 nibbles of built in RAM */

    if (mapper == MAPPER_MBC3 && has_rtc && code == 0x03)
        return 0; /* the RAM slot is taken by the real time clock */

    switch (code) {
    case 0x00: return 0;
    case 0x01: return 2048;
    case 0x02: return 8192;
    case 0x03: return 32768;
    case 0x04: return 131072;
    case 0x05: return 65536;
    default: return 0;
    }
}

/* Cartridge titles are padded with 0x00, 0xFF or spaces; trim whichever
 * filler is there and null terminate in place. */
static void trim_title(char *title)
{
    int end = 16;

    while (end > 0) {
        unsigned char c = (unsigned char)title[end - 1];
        if (c == 0x00 || c == 0xFF || c == ' ')
            end--;
        else
            break;
    }
    title[end] = '\0';
}

/* Bank masking in the bus assumes a power of two bank count, so round the
 * image up to one and pad it out. */
static uint8_t *pad_rom_to_power_of_two(uint8_t *rom, size_t *size)
{
    size_t banks_needed = (*size + GB_ROM_BANK_SIZE - 1) / GB_ROM_BANK_SIZE;
    size_t banks = 1;
    uint8_t *padded;

    while (banks < banks_needed)
        banks <<= 1;

    if (banks * GB_ROM_BANK_SIZE == *size)
        return rom;

    padded = (uint8_t *)realloc(rom, banks * GB_ROM_BANK_SIZE);
    if (!padded) {
        free(rom);
        return NULL;
    }
    memset(padded + *size, 0x00, banks * GB_ROM_BANK_SIZE - *size);
    *size = banks * GB_ROM_BANK_SIZE;
    return padded;
}

void gb_free_cart(Cart *cart)
{
    if (!cart)
        return;
    free(cart->rom);
    free(cart->ram);
    cart->rom = NULL;
    cart->ram = NULL;
    cart->rom_size = 0;
    cart->ram_size = 0;
}

bool gb_load_rom(const char *path, Cart *out_cart, char *err, size_t errlen)
{
    FILE *f;
    long file_size;
    size_t size;
    size_t got;
    uint8_t *rom;
    Cart cart;
    uint8_t ram_code;

    if (err && errlen)
        err[0] = '\0';

    f = platform_fopen(path, "rb");
    if (!f) {
        if (err && errlen)
            snprintf(err, errlen, "cannot open '%s'", path);
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0) {
        fclose(f);
        if (err && errlen)
            snprintf(err, errlen, "cannot measure '%s'", path);
        return false;
    }
    rewind(f);

    size = (size_t)file_size;

    if (size < 0x150) {
        fclose(f);
        if (err && errlen)
            snprintf(err, errlen, "'%s' is too small to be a Game Boy ROM", path);
        return false;
    }

    rom = (uint8_t *)malloc(size);
    if (!rom) {
        fclose(f);
        if (err && errlen)
            snprintf(err, errlen, "out of memory loading '%s'", path);
        return false;
    }

    got = fread(rom, 1, size, f);
    fclose(f);

    if (got != size) {
        free(rom);
        if (err && errlen)
            snprintf(err, errlen, "short read on '%s'", path);
        return false;
    }

    rom = pad_rom_to_power_of_two(rom, &size);
    if (!rom) {
        if (err && errlen)
            snprintf(err, errlen, "out of memory loading '%s'", path);
        return false;
    }

    /* ---- header ---- */
    memset(&cart, 0, sizeof(cart));
    memcpy(cart.title, rom + 0x134, 16);
    trim_title(cart.title);

    cart.is_color = (rom[0x143] & 0x80) != 0;
    cart.is_sgb = rom[0x146] == 0x03;
    cart.type_code = rom[0x147];
    cart.rom_size_code = rom[0x148];
    ram_code = rom[0x149];

    cart.mapper = mapper_from_code(cart.type_code, &cart.has_ram,
                                   &cart.has_battery, &cart.has_rtc);

    /* MBC3's timer is declared by the cartridge type, not by the RAM size. */
    if (cart.type_code == 0x0F || cart.type_code == 0x10)
        cart.has_rtc = true;

    cart.rom = rom;
    cart.rom_size = (size_t)size;
    cart.ram_size = ram_size_from_code(ram_code, cart.mapper, cart.has_rtc);
    cart.has_ram = cart.ram_size > 0;

    if (cart.has_ram) {
        cart.ram = (uint8_t *)calloc(1, cart.ram_size);
        if (!cart.ram) {
            free(rom);
            if (err && errlen)
                snprintf(err, errlen, "out of memory for cartridge RAM");
            return false;
        }
    }

    cart.bank1 = 1;
    cart.bank2 = 0;
    cart.mode = 0;
    cart.ram_enabled = false;
    cart.rtc_latch = 0;

    *out_cart = cart;
    return true;
}