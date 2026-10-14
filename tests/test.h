/*
 * test.h -- a very small unit test harness.
 *
 * Each suite file defines its cases as static functions and exports a
 * NULL terminated table of them; tests/main.c runs every table.
 */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb.h"

typedef void (*TestFn)(void);

typedef struct {
    const char *suite;
    const char *name;
    TestFn fn;
} TestCase;

void test_fail(const char *file, int line, const char *fmt, ...);
int test_run_suite(const TestCase *cases);

/* Every suite ends with a NULL entry. */
#define TEST_SUITE_END { NULL, NULL, NULL }

#define FAIL(...) test_fail(__FILE__, __LINE__, __VA_ARGS__)

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond))                                                           \
            FAIL("expected %s", #cond);                                        \
    } while (0)

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        if (!(cond))                                                           \
            FAIL(__VA_ARGS__);                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        long long a_ = (long long)(actual);                                    \
        long long e_ = (long long)(expected);                                  \
        if (a_ != e_)                                                          \
            FAIL("%s: expected %lld (0x%llX), got %lld (0x%llX)", #actual, e_,   \
                 (unsigned long long)e_, a_, (unsigned long long)a_);          \
    } while (0)

#define CHECK_NE(actual, unexpected)                                           \
    do {                                                                       \
        if ((long long)(actual) == (long long)(unexpected))                    \
            FAIL("%s: expected anything but %lld", #actual,                     \
                 (long long)(unexpected));                                     \
    } while (0)

/* ------------------------------------------------------------------ */
/* Fixture: a machine backed by a synthetic cartridge                  */
/* ------------------------------------------------------------------ */

typedef struct {
    GB gb;
    Cart cart;
} Fixture;

/* The image is padded up to a power of two bank count, as the loader
 * does, so requesting 32 KiB gives a two bank cart. */
Fixture fixture_create(size_t rom_size, uint8_t type_code);
void fixture_destroy(Fixture *f);

/* Writes a realistic 256 byte header before the entry point is used. */
void fixture_header(Fixture *f, uint8_t type_code, uint8_t rom_code,
                    uint8_t ram_code);

void fixture_write(Fixture *f, uint16_t addr, const uint8_t *data, size_t len);
void fixture_byte(Fixture *f, uint16_t addr, uint8_t value);
void fixture_word(Fixture *f, uint16_t addr, uint16_t value);

/*
 * Switches the LCD off, which is how real code makes video memory reliably
 * reachable: with the PPU powered down it fetches nothing, so VRAM and OAM
 * belong to the CPU again.  Tests that set up tiles or objects want this,
 * because otherwise their writes land while the PPU owns the bus and are
 * correctly thrown away.
 */
void fixture_video_accessible(Fixture *f);

/* Plants a program at $0100 and resets the CPU control state, leaving the
 * registers for the test to arrange.  Then run `steps` instructions. */
void fixture_load(Fixture *f, const uint8_t *code, size_t len);
void fixture_exec(Fixture *f, int steps);

/* Shorthand for the two calls above. */
void fixture_run(Fixture *f, const uint8_t *code, size_t len, int steps);

/* Plants a single opcode at $0100 with zeroed operands, runs it once and
 * returns the number of T-cycles it took.  Handy for timing tables. */
int fixture_run_op(Fixture *f, uint8_t opcode);

/* Single instruction step, returns the T-cycles it consumed. */
int fixture_step(Fixture *f);

/* Register accessors.  `f` is always a Fixture value, not a pointer. */
#define AF(f)  ((uint16_t)(((uint16_t)((f).gb.cpu.a) << 8) | (f).gb.cpu.f))
#define BC(f)  ((uint16_t)(((uint16_t)((f).gb.cpu.b) << 8) | (f).gb.cpu.c))
#define DE(f)  ((uint16_t)(((uint16_t)((f).gb.cpu.d) << 8) | (f).gb.cpu.e))
#define HL(f)  ((uint16_t)(((uint16_t)((f).gb.cpu.h) << 8) | (f).gb.cpu.l))

#define REG_A  ((f).gb.cpu.a)
#define REG_F  ((f).gb.cpu.f)
#define REG_B  ((f).gb.cpu.b)
#define REG_C  ((f).gb.cpu.c)
#define REG_D  ((f).gb.cpu.d)
#define REG_E  ((f).gb.cpu.e)
#define REG_H  ((f).gb.cpu.h)
#define REG_L  ((f).gb.cpu.l)
#define REG_PC ((f).gb.cpu.pc)
#define REG_SP ((f).gb.cpu.sp)

#define FZ (GB_FLAG_Z)
#define FN (GB_FLAG_N)
#define FH (GB_FLAG_H)
#define FC (GB_FLAG_C)

/* Builds a writable scratch path for a test, in whatever the system calls a
 * temporary directory.  A small pool of buffers is used, so the result stays
 * valid across a few more calls. */
const char *test_temp_path(const char *name);

#endif /* TEST_H */