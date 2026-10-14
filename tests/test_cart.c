/*
 * test_cart.c -- cartridge headers and the memory bank controllers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "platform.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Stamps every 16 KiB bank with its own number, low byte first, so a test
 * can tell exactly which bank is mapped even above 256. */
static void stamp_banks(Fixture *f)
{
    size_t banks = f->cart.rom_size / GB_ROM_BANK_SIZE;

    for (size_t b = 0; b < banks; b++) {
        f->cart.rom[b * GB_ROM_BANK_SIZE] = (uint8_t)(b & 0xFF);
        f->cart.rom[b * GB_ROM_BANK_SIZE + 1] = (uint8_t)(b >> 8);
    }
}

/* Reads the 16 bit bank stamp that is currently mapped at $4000. */
static uint32_t read_bank_id(Fixture *f)
{
    uint32_t lo = gb_read(&f->gb, 0x4000);
    uint32_t hi = gb_read(&f->gb, 0x4001);
    return lo | (hi << 8);
}

/* Same idea for the fixed window at $0000. */
static uint32_t read_zero_id(Fixture *f)
{
    uint32_t lo = gb_read(&f->gb, 0x0000);
    uint32_t hi = gb_read(&f->gb, 0x0001);
    return lo | (hi << 8);
}

/* ------------------------------------------------------------------ */
/* Plain ROM                                                           */
/* ------------------------------------------------------------------ */

static void t_cart_rom_only_never_banks(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    const uint8_t code[] = { 0x01, 0x05, 0x00 }; /* LD BC,$0005 */

    stamp_banks(&f);
    fixture_write(&f, 0x4000, code, sizeof(code));

    /* Writes to the bank registers are ignored without an MBC. */
    gb_write(&f.gb, 0x2000, 0x05);
    CHECK_EQ(f.gb.cart.bank1, 5); /* the register still latches */
    CHECK_EQ(gb_read(&f.gb, 0x4000), 1); /* but nothing is really mapped */
    CHECK_EQ(gb_read(&f.gb, 0x0000), 0);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* MBC1                                                                */
/* ------------------------------------------------------------------ */

static void t_cart_mbc1_banks_low_rom(void)
{
    Fixture f = fixture_create(0x20000, 0x01); /* 128 KiB, 16 banks */
    int seen[8];
    int ok = 1;

    stamp_banks(&f);
    memset(seen, 0, sizeof(seen));

    for (int bank = 1; bank < 8; bank++) {
        gb_write(&f.gb, 0x2000, (uint8_t)bank);
        seen[gb_read(&f.gb, 0x4000)] = 1;
    }
    for (int i = 1; i < 8; i++)
        if (!seen[i])
            ok = 0;

    CHECK_MSG(ok, "MBC1 did not reach every requested ROM bank");
    CHECK_EQ(read_zero_id(&f), 0); /* $0000 stays on bank 0 in mode 0 */
    fixture_destroy(&f);
}

static void t_cart_mbc1_bank_zero_becomes_one(void)
{
    Fixture f = fixture_create(0x20000, 0x01);

    stamp_banks(&f);
    gb_write(&f.gb, 0x2000, 0x00);
    CHECK_EQ(read_bank_id(&f), 1);
    fixture_destroy(&f);
}

static void t_cart_mbc1_masks_the_five_bit_register(void)
{
    Fixture f = fixture_create(0x40000, 0x01); /* 256 KiB, 32 banks */

    stamp_banks(&f);
    gb_write(&f.gb, 0x2000, 0x25); /* only the low five bits count */
    CHECK_EQ(read_bank_id(&f), 0x05);
    fixture_destroy(&f);
}

static void t_cart_mbc1_mode_one_uses_the_second_register(void)
{
    Fixture f = fixture_create(0x100000, 0x01); /* 1 MiB, 64 banks */

    stamp_banks(&f);
    gb_write(&f.gb, 0x6000, 0x01); /* advanced banking */
    gb_write(&f.gb, 0x4000, 0x01); /* second register = 1 */
    gb_write(&f.gb, 0x2000, 0x01); /* first register  = 1 */

    /* bank = (1 << 5) | 1 = 33 */
    CHECK_EQ(read_bank_id(&f), 33);
    /* and bank 0 also moves, which is the point of mode 1 */
    CHECK_EQ(read_zero_id(&f), 32);
    fixture_destroy(&f);
}

static void t_cart_mbc1_mode_zero_pins_bank_zero(void)
{
    Fixture f = fixture_create(0x100000, 0x01);

    stamp_banks(&f);
    gb_write(&f.gb, 0x6000, 0x01);
    gb_write(&f.gb, 0x4000, 0x01);
    gb_write(&f.gb, 0x2000, 0x01);
    CHECK_EQ(read_zero_id(&f), 32);

    gb_write(&f.gb, 0x6000, 0x00); /* back to simple banking */
    CHECK_EQ(read_zero_id(&f), 0);
    CHECK_EQ(read_bank_id(&f), 1);
    fixture_destroy(&f);
}

static void t_cart_mbc1_ram_is_gated(void)
{
    Fixture f = fixture_create(0x8000, 0x03); /* MBC1 + RAM + battery */

    /* Reads are open bus while the RAM is disabled. */
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0xFF);

    gb_write(&f.gb, 0x0000, 0x0A); /* enable */
    gb_write(&f.gb, 0xA000, 0x42);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x42);

    gb_write(&f.gb, 0x0000, 0x00); /* disable again */
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0xFF);

    /* Re-enabling must not have lost the contents. */
    gb_write(&f.gb, 0x0000, 0x0A);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x42);
    fixture_destroy(&f);
}

static void t_cart_mbc1_ram_banks(void)
{
    Fixture f = fixture_create(0x8000, 0x03);

    gb_write(&f.gb, 0x0000, 0x0A);
    gb_write(&f.gb, 0xA000, 0x11);

    gb_write(&f.gb, 0x4000, 0x01);
    gb_write(&f.gb, 0x6000, 0x01); /* mode 1 so bank2 selects the RAM bank */
    gb_write(&f.gb, 0xA000, 0x22);
    gb_write(&f.gb, 0xA000, 0x22);

    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x22);
    gb_write(&f.gb, 0x4000, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x11);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* MBC2                                                                */
/* ------------------------------------------------------------------ */

static void t_cart_mbc2_uses_a_four_bit_register(void)
{
    Fixture f = fixture_create(0x40000, 0x05); /* 256 KiB, 16 banks */

    stamp_banks(&f);

    /* $0000-$3FFF selects the bank when bit 8 of the address is set. */
    gb_write(&f.gb, 0x2000, 0x0A);
    CHECK_EQ(read_bank_id(&f), 10);

    /* Writing 0x1E keeps the low four bits, so bank 14 is mapped. */
    gb_write(&f.gb, 0x2000, 0x1E);
    CHECK_EQ(read_bank_id(&f), 14);
    fixture_destroy(&f);
}

static void t_cart_mbc2_ram_is_a_nibble(void)
{
    Fixture f = fixture_create(0x8000, 0x05);

    gb_write(&f.gb, 0x0000, 0x0A);
    gb_write(&f.gb, 0xA000, 0xFF);
    /* Only the low nibble is stored; the top bits read back as ones. */
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0xFF);

    gb_write(&f.gb, 0xA000, 0x3C);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0xFC);

    /* Only 512 nibbles exist, so $A200 and above is open bus. */
    CHECK_EQ(gb_read(&f.gb, 0xA200), 0xFF);
    fixture_destroy(&f);
}

static void t_cart_mbc2_ram_gate_uses_bit_eight(void)
{
    Fixture f = fixture_create(0x8000, 0x05);

    /* $0000-$1FFF is the enable register, $2000+ is the ROM bank. */
    gb_write(&f.gb, 0x0000, 0x0A);
    CHECK_EQ(f.gb.cart.ram_enabled, true);
    gb_write(&f.gb, 0x0000, 0x00);
    CHECK_EQ(f.gb.cart.ram_enabled, false);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* MBC3                                                                */
/* ------------------------------------------------------------------ */

static void t_cart_mbc3_banks_seven_bits(void)
{
    Fixture f = fixture_create(0x100000, 0x11); /* 1 MiB, 64 banks */

    stamp_banks(&f);
    gb_write(&f.gb, 0x2000, 0x3F); /* 63 is inside the seven bit range */
    CHECK_EQ(read_bank_id(&f), 63);

    gb_write(&f.gb, 0x2000, 0x7F);
    CHECK_EQ(read_bank_id(&f), 63); /* 0x7F masks down to 63 banks */

    gb_write(&f.gb, 0x2000, 0x00);
    CHECK_EQ(read_bank_id(&f), 1); /* bank 0 is remapped to 1 */
    fixture_destroy(&f);
}

static void t_cart_mbc3_ram_banks(void)
{
    Fixture f = fixture_create(0x8000, 0x13);

    gb_write(&f.gb, 0x0000, 0x0A);
    gb_write(&f.gb, 0xA000, 0x5A);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x5A);

    gb_write(&f.gb, 0x4000, 0x01);
    gb_write(&f.gb, 0xA000, 0xA5);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0xA5);

    gb_write(&f.gb, 0x4000, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x5A);
    fixture_destroy(&f);
}

static void t_cart_mbc3_rtc_latching(void)
{
    Fixture f = fixture_create(0x8000, 0x10); /* MBC3 + timer + RAM + battery */

    CHECK_EQ(f.gb.cart.has_rtc, true);

    gb_write(&f.gb, 0x0000, 0x0A);
    f.gb.cart.rtc_sec = 30;
    f.gb.cart.rtc_min = 45;

    /* Writing $08..$0C selects a clock register. */
    gb_write(&f.gb, 0x4000, 0x08);
    /* Until a latch happens the read shows the shadow copy, which is zero. */
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0);

    /* $6000 = 0 then 1 copies the live clock into the shadow registers. */
    gb_write(&f.gb, 0x6000, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0);
    gb_write(&f.gb, 0x6000, 0x01);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0); /* still nothing, the edge matters */
    gb_write(&f.gb, 0x6000, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 30);

    gb_write(&f.gb, 0x4000, 0x09);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 45);

    /* The clock is writable too. */
    gb_write(&f.gb, 0xA000, 12);
    gb_write(&f.gb, 0x6000, 0x00);
    gb_write(&f.gb, 0x6000, 0x01);
    gb_write(&f.gb, 0x6000, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 12);
    fixture_destroy(&f);
}

static void t_cart_mbc3_rtc_day_counter(void)
{
    Fixture f = fixture_create(0x8000, 0x10);

    gb_write(&f.gb, 0x0000, 0x0A);
    f.gb.cart.rtc_day = 300;
    f.gb.cart.rtc_day_high = 0x01; /* bit 8 of the day counter */

    gb_write(&f.gb, 0x4000, 0x0B);
    gb_write(&f.gb, 0x6000, 0x00);
    gb_write(&f.gb, 0x6000, 0x01);
    gb_write(&f.gb, 0x6000, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xA000) & 0xFF, 300 & 0xFF);


    gb_write(&f.gb, 0x4000, 0x0C);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x81); /* day bit 8 plus the halt bit */
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* MBC5                                                                */
/* ------------------------------------------------------------------ */

static void t_cart_mbc5_has_nine_bit_banking(void)
{
    Fixture f = fixture_create(0x800000, 0x1B); /* 4 MiB, 256 banks */

    stamp_banks(&f);
    gb_write(&f.gb, 0x2000, 0x34); /* low byte  */
    gb_write(&f.gb, 0x3000, 0x01); /* ninth bit */
    CHECK_EQ(read_bank_id(&f), 0x134);

    gb_write(&f.gb, 0x2000, 0x00);
    CHECK_EQ(read_bank_id(&f), 0x100); /* bank zero is allowed on MBC5 */
    fixture_destroy(&f);
}

static void t_cart_mbc5_bank_zero_is_real(void)
{
    Fixture f = fixture_create(0x800000, 0x1B);

    stamp_banks(&f);
    gb_write(&f.gb, 0x2000, 0x00);
    CHECK_EQ(read_bank_id(&f), 0);
    fixture_destroy(&f);
}

static void t_cart_mbc5_ram_banks_four_bits(void)
{
    Fixture f = fixture_create(0x8000, 0x1B);

    gb_write(&f.gb, 0x0000, 0x0A);
    gb_write(&f.gb, 0xA000, 0x01);

    gb_write(&f.gb, 0x4000, 0x01);
    gb_write(&f.gb, 0xA000, 0x02);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x02);

    gb_write(&f.gb, 0x4000, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xA000), 0x01);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Header parsing on a real file                                       */
/* ------------------------------------------------------------------ */

/* Writes a synthetic ROM of the requested size to a temp file. */
static bool write_temp_rom(const char *path, size_t size, uint8_t type,
                           uint8_t rom_code, uint8_t ram_code,
                           const char *title)
{
    FILE *f = platform_fopen(path, "wb");
    uint8_t *image = (uint8_t *)calloc(1, size);
    bool ok = true;

    if (!image) {
        if (f) fclose(f);
        return false;
    }

    memset(image + 0x134, ' ', 16);
    if (title)
        memcpy(image + 0x134, title, strlen(title));

    image[0x143] = 0x00;
    image[0x146] = 0x00;
    image[0x147] = type;
    image[0x148] = rom_code;
    image[0x149] = ram_code;

    /* Nintendo's logo, which real hardware checks at boot. */
    memcpy(image + 0x104,
           "\xCE\xED\x66\x66\xCC\x0D\x00\x0B\x03\x73\x00\x83\x00\x0C\x00\x0D"
           "\x00\x08\x11\x1F\x88\x89\x00\x0E\xDC\xCC\x6E\xE6\xDD\xDD\xD9"
           "\x99\xBB\xBB\x67\x63\x6E\x0E\xEC\xCC\xDD\xDC\x99\x9F\xBB\xB9"
           "\x33\x3E",
           48);

    ok = fwrite(image, 1, size, f) == size;
    free(image);
    fclose(f);
    return ok;
}

static void t_cart_header_is_parsed(void)
{
    const char *path = test_temp_path("header.gb");
    Cart cart;
    char err[256];

    if (!write_temp_rom(path, 0x8000, 0x03, 0x00, 0x02, "HELLO WORLD")) {
        FAIL("could not create the temporary ROM");
        return;
    }

    if (!gb_load_rom(path, &cart, err, sizeof(err))) {
        FAIL("gb_load_rom failed: %s", err);
        return;
    }

    CHECK_EQ(strcmp(cart.title, "HELLO WORLD"), 0);
    CHECK_EQ(cart.mapper, MAPPER_MBC1);
    CHECK_EQ(cart.is_color, false);
    CHECK_EQ(cart.has_battery, true);
    CHECK_EQ(cart.has_ram, true);
    CHECK_EQ(cart.ram_size, 8192);
    CHECK_EQ(cart.rom_size, 0x8000);
    CHECK_EQ(cart.type_code, 0x03);

    gb_free_cart(&cart);
    remove(path);
}

static void t_cart_mbc_types_are_detected(void)
{
    struct {
        uint8_t type;
        Mapper mapper;
        bool rtc;
    } cases[] = {
        { 0x00, MAPPER_ROM, false },
        { 0x01, MAPPER_MBC1, false },
        { 0x03, MAPPER_MBC1, false },
        { 0x05, MAPPER_MBC2, false },
        { 0x06, MAPPER_MBC2, false },
        { 0x0F, MAPPER_MBC3, true },
        { 0x10, MAPPER_MBC3, true },
        { 0x13, MAPPER_MBC3, false },
        { 0x19, MAPPER_MBC5, false },
        { 0x1E, MAPPER_MBC5, false }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *path = test_temp_path("type.gb");
        Cart cart;
        char err[256];

        if (!write_temp_rom(path, 0x8000, cases[i].type, 0x00, 0x02, "T")) {
            FAIL("could not create the temporary ROM");
            return;
        }
        if (!gb_load_rom(path, &cart, err, sizeof(err))) {
            FAIL("gb_load_rom failed: %s", err);
            return;
        }

        CHECK_MSG(cart.mapper == cases[i].mapper,
                  "type $%02X: expected mapper %d, got %d", cases[i].type,
                  cases[i].mapper, cart.mapper);
        CHECK_MSG(cart.has_rtc == cases[i].rtc,
                  "type $%02X: rtc expected %d, got %d", cases[i].type,
                  cases[i].rtc, cart.has_rtc);

        gb_free_cart(&cart);
        remove(path);
    }
}

static void t_cart_ram_size_codes(void)
{
    struct {
        uint8_t code;
        size_t size;
    } cases[] = {
        { 0x00, 0 },        { 0x01, 2048 },   { 0x02, 8192 },
        { 0x03, 32768 },    { 0x04, 131072 }, { 0x05, 65536 }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *path = test_temp_path("ram.gb");
        Cart cart;
        char err[256];

        if (!write_temp_rom(path, 0x8000, 0x01, 0x00, cases[i].code, "T")) {
            FAIL("could not create the temporary ROM");
            return;
        }
        if (!gb_load_rom(path, &cart, err, sizeof(err))) {
            FAIL("gb_load_rom failed: %s", err);
            return;
        }

        CHECK_MSG(cart.ram_size == cases[i].size,
                  "RAM code $%02X: expected %zu, got %zu", cases[i].code,
                  cases[i].size, cart.ram_size);

        gb_free_cart(&cart);
        remove(path);
    }
}

static void t_cart_rejects_a_short_file(void)
{
    const char *path = test_temp_path("tiny.gb");
    Cart cart;
    char err[256];
    FILE *f = platform_fopen(path, "wb");
    uint8_t junk[16] = { 0 };

    if (!f) {
        FAIL("could not create the temporary file");
        return;
    }
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    CHECK(!gb_load_rom(path, &cart, err, sizeof(err)));
    CHECK(err[0] != '\0');
    remove(path);
}

static void t_cart_reports_a_missing_file(void)
{
    Cart cart;
    char err[256];

    CHECK(!gb_load_rom(test_temp_path("definitely_not_here.gb"), &cart, err,
                      sizeof(err)));
    CHECK(err[0] != '\0');
}

static void t_cart_colour_flag_is_reported(void)
{
    const char *path = test_temp_path("cgb.gb");
    Cart cart;
    char err[256];
    FILE *f;
    uint8_t *image;

    image = (uint8_t *)calloc(1, 0x8000);
    if (!image) {
        FAIL("out of memory");
        return;
    }
    image[0x143] = 0x80; /* colour support */
    image[0x146] = 0x03; /* Super Game Boy */
    image[0x147] = 0x1B;

    f = platform_fopen(path, "wb");
    fwrite(image, 1, 0x8000, f);
    fclose(f);
    free(image);

    if (!gb_load_rom(path, &cart, err, sizeof(err))) {
        FAIL("gb_load_rom failed: %s", err);
        return;
    }

    CHECK_EQ(cart.is_color, true);
    CHECK_EQ(cart.is_sgb, true);
    gb_free_cart(&cart);
    remove(path);
}

const TestCase cart_tests[] = {
    { "cart", "rom only never banks", t_cart_rom_only_never_banks },
    { "cart", "mbc1 banks low rom", t_cart_mbc1_banks_low_rom },
    { "cart", "mbc1 bank zero becomes one", t_cart_mbc1_bank_zero_becomes_one },
    { "cart", "mbc1 masks the five bit register", t_cart_mbc1_masks_the_five_bit_register },
    { "cart", "mbc1 mode one uses the second register", t_cart_mbc1_mode_one_uses_the_second_register },
    { "cart", "mbc1 mode zero pins bank zero", t_cart_mbc1_mode_zero_pins_bank_zero },
    { "cart", "mbc1 ram is gated", t_cart_mbc1_ram_is_gated },
    { "cart", "mbc1 ram banks", t_cart_mbc1_ram_banks },
    { "cart", "mbc2 uses a four bit register", t_cart_mbc2_uses_a_four_bit_register },
    { "cart", "mbc2 ram is a nibble", t_cart_mbc2_ram_is_a_nibble },
    { "cart", "mbc2 ram gate uses bit eight", t_cart_mbc2_ram_gate_uses_bit_eight },
    { "cart", "mbc3 banks seven bits", t_cart_mbc3_banks_seven_bits },
    { "cart", "mbc3 ram banks", t_cart_mbc3_ram_banks },
    { "cart", "mbc3 rtc latching", t_cart_mbc3_rtc_latching },
    { "cart", "mbc3 rtc day counter", t_cart_mbc3_rtc_day_counter },
    { "cart", "mbc5 has nine bit banking", t_cart_mbc5_has_nine_bit_banking },
    { "cart", "mbc5 bank zero is real", t_cart_mbc5_bank_zero_is_real },
    { "cart", "mbc5 ram banks four bits", t_cart_mbc5_ram_banks_four_bits },
    { "cart", "header is parsed", t_cart_header_is_parsed },
    { "cart", "mapper types are detected", t_cart_mbc_types_are_detected },
    { "cart", "ram size codes", t_cart_ram_size_codes },
    { "cart", "rejects a short file", t_cart_rejects_a_short_file },
    { "cart", "reports a missing file", t_cart_reports_a_missing_file },
    { "cart", "colour flag is reported", t_cart_colour_flag_is_reported },
    TEST_SUITE_END
};