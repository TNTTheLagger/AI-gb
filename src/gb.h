/*
 * gb.h -- Public interface of the Game Boy (DMG) emulator core.
 *
 * Everything in here is plain C99: no platform headers, no SDL, no
 * dependencies of any kind.  The frontend layer (src/frontend*.c) is the only
 * place allowed to touch the operating system.
 */
#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Machine constants                                                   */
/* ------------------------------------------------------------------ */

#define GB_SCREEN_W 160
#define GB_SCREEN_H 144
#define GB_CYCLES_PER_FRAME 70224 /* 154 * 456, i.e. 59.727 Hz */
#define GB_CLOCK_HZ 4194304u

/* Memory sizes */
#define GB_ROM_BANK_SIZE 0x4000u
#define GB_CART_RAM_SIZE 0x2000u
#define GB_VRAM_SIZE 0x2000u
#define GB_WRAM_SIZE 0x2000u
#define GB_OAM_SIZE 0x00A0u
#define GB_HRAM_SIZE 0x007Fu

/* Memory map */
#define GB_MEM_ROM0 0x0000u
#define GB_MEM_ROMX 0x4000u
#define GB_MEM_VRAM 0x8000u
#define GB_MEM_CART 0xA000u
#define GB_MEM_WRAM 0xC000u
#define GB_MEM_WRAM_ECHO 0xE000u
#define GB_MEM_OAM 0xFE00u
#define GB_MEM_UNUSABLE 0xFEA0u
#define GB_MEM_IO 0xFF00u
#define GB_MEM_HRAM 0xFF80u
#define GB_MEM_IE 0xFFFFu

/* Interrupt bits (IE / IF) */
#define GB_INT_VBLANK 0x01
#define GB_INT_LCDSTAT 0x02
#define GB_INT_TIMER 0x04
#define GB_INT_SERIAL 0x08
#define GB_INT_JOYPAD 0x10

/* CPU status flags */
#define GB_FLAG_Z 0x80u
#define GB_FLAG_N 0x40u
#define GB_FLAG_H 0x20u
#define GB_FLAG_C 0x10u

typedef struct GB GB;

/* ------------------------------------------------------------------ */
/* Cartridge / mapper                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    MAPPER_ROM = 0,   /* no MBC                                  */
    MAPPER_MBC1,      /* MBC1        (+ optional RAM, + battery) */
    MAPPER_MBC2,      /* MBC2        (512 x 4 bit built in RAM) */
    MAPPER_MBC3,      /* MBC3        (+ optional RAM, + RTC)     */
    MAPPER_MBC5,      /* MBC5        (+ optional RAM, + rumble)  */
    MAPPER_UNKNOWN
} Mapper;

typedef struct {
    /* Cartridge storage */
    uint8_t *rom;
    size_t rom_size;
    uint8_t *ram; /* cartridge RAM, may be NULL */
    size_t ram_size;

    Mapper mapper;
    bool has_ram;
    bool has_battery;
    bool has_rtc;

    /* MBC state */
    bool ram_enabled;
    uint8_t bank1; /* $2000-$3FFF low 5 bits  */
    uint8_t bank2; /* $4000-$5FFF 8 bits       */
    uint8_t bank1_mbc5; /* $2000-$2FFF 8 bits (MBC5) */
    uint8_t bank2_mbc5; /* $3000-$3FFF 9th bit (MBC5) */
    uint8_t mode;   /* MBC1 banking mode 0/1  */

    /* MBC3 real time clock */
    uint8_t rtc_latch;
    uint8_t rtc_reg_latched[5];
    uint8_t rtc_sel;
    uint32_t rtc_sec;
    uint32_t rtc_min;
    uint32_t rtc_hour;
    uint32_t rtc_day;
    uint32_t rtc_day_high;
    double rtc_last;

    /* Header fields */
    char title[17];
    bool is_color;
    bool is_sgb;
    uint8_t type_code;
    uint8_t rom_size_code;
    uint8_t ram_size_code;
} Cart;

/* ------------------------------------------------------------------ */
/* CPU                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp;
    uint16_t pc;

    bool ime;           /* interrupt master enable                   */
    int ime_delay;      /* EI countdown; IME turns on when it hits 0  */
    bool halted;        /* HALT state, waiting for an interrupt      */
    bool stopped;       /* STOP state, waiting for a joypad press   */
    bool halt_bug;      /* next opcode fetch reads the byte twice    */
    int idle_cycles;    /* instructions burned while halted          */

    uint64_t instructions;
} CPU;

/* ------------------------------------------------------------------ */
/* PPU                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t lcdc, stat;
    uint8_t scy, scx;
    uint8_t ly, lyc, dma;
    uint8_t bgp, obp0, obp1;
    uint8_t wy, wx;

    int dot;        /* 0..455 position inside the current scanline */
    int mode;       /* 0 HBlank, 2 OAM scan, 3 pixel transfer       */
    int mode3_len;  /* length of the current mode 3 in dots        */
    bool frame_done;/* set once LY wraps back to 0                */
    uint32_t frame; /* completed frame counter                    */

    bool stat_line; /* previous level of the STAT interrupt line  */

    /*
     * Writing STAT is delayed by one M-cycle on a monochrome Game Boy, with
     * every interrupt source briefly selected in the meantime.  The value
     * that was really written waits here until the CPU moves on.
     */
    uint8_t stat_pending;
    int stat_delay;

    int window_line; /* internal window line counter               */
    bool window_wy_hit; /* LY has matched WY at some point this frame */
    int sprites_fetched; /* sprites drawn on the current line     */

    /*
     * Working out how long the pixel transfer takes needs the window to be
     * known before the objects are visited, and it needs to remember which
     * background tiles an earlier object already paid for.  Only the tile
     * column varies along a line, so the row is implicit.
     */
    bool window_drawn;       /* the window was rendered on this line    */
    int window_start;        /* first screen pixel the window covered   */
    bool tile_considered[21];/* background tiles already charged        */

    uint8_t line[GB_SCREEN_W]; /* colour indices, before palettes?  */
    uint32_t fb[GB_SCREEN_W * GB_SCREEN_H];
    uint32_t palette[4];       /* shades used by the framebuffer    */
} PPU;

/* ------------------------------------------------------------------ */
/* Timer                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t div;  /* internal divider (DIV register is div >> 8) */
    uint8_t tima;
    uint8_t tma;
    uint8_t tac;
    uint32_t sample; /* last sampled TAC-selected bit             */
    bool overflow_pending;
} Timer;

/* ------------------------------------------------------------------ */
/* Joypad                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t keys;      /* currently held buttons, see GB_KEY_*      */
    uint8_t sel;       /* which row P1 is pointing at              */
    uint8_t prev_line; /* previous level of the four column lines    */
} Joypad;

/* ------------------------------------------------------------------ */
/* Serial port                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t sb, sc;
    int counter;
    bool internal_clock;
} Serial;

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

#define APU_RING_SIZE 8192

typedef struct {
    /* Channel 1: square + sweep */
    bool ch1_on;
    uint16_t ch1_freq;
    uint32_t ch1_timer;
    uint8_t ch1_duty_pos;
    uint8_t ch1_duty;
    uint8_t ch1_len, ch1_len_enable;
    uint8_t ch1_vol, ch1_env_initial, ch1_env_dir, ch1_env_period,
        ch1_env_timer;
    uint16_t ch1_sweep_period, ch1_shadow, ch1_sweep_timer;
    uint8_t ch1_sweep_shift;
    bool ch1_sweep_enabled, ch1_sweep_negate;

    /* Channel 2: square */
    bool ch2_on;
    uint16_t ch2_freq;
    uint32_t ch2_timer;
    uint8_t ch2_duty_pos;
    uint8_t ch2_duty;
    uint8_t ch2_len, ch2_len_enable;
    uint8_t ch2_vol, ch2_env_initial, ch2_env_dir, ch2_env_period,
        ch2_env_timer;

    /* Channel 3: wave */
    bool ch3_on;
    bool ch3_dac;
    uint16_t ch3_freq;
    uint32_t ch3_timer;
    uint8_t ch3_pos;
    uint8_t ch3_len, ch3_len_enable;
    uint8_t ch3_vol_code;
    uint8_t ch3_wave[16];

    /* Channel 4: noise */
    bool ch4_on;
    uint16_t ch4_timer;
    uint8_t ch4_len, ch4_len_enable;
    uint8_t ch4_vol, ch4_env_initial, ch4_env_dir, ch4_env_period,
        ch4_env_timer;
    uint8_t ch4_shift, ch4_width;
    uint16_t ch4_lfsr;

    /* Registers */
    uint8_t nr10, nr11, nr12, nr13, nr14;
    uint8_t nr21, nr22, nr23, nr24;
    uint8_t nr30, nr31, nr32, nr33, nr34;
    uint8_t nr41, nr42, nr43, nr44;
    uint8_t nr50, nr51, nr52;
    uint8_t wave[16];

    bool powered;

    /* Frame sequencer */
    uint32_t fs_timer;
    uint8_t fs_step;

    /* Output stage */
    double cap_left;  /* DC blocking capacitor state, one per side */
    double cap_right;
    double hp_coeff;  /* per sample decay factor of that capacitor */
    uint64_t acc;    /* fractional sample accumulator */
    uint64_t period; /* fixed point period, in 1/65536 T-cycles */
    int32_t rate;

    /* Ring buffer handed over to the frontend, interleaved L/R. */
    int16_t ring[APU_RING_SIZE * 2];
    uint32_t ring_read, ring_write;
    uint32_t dropped;
} APU;

/* ------------------------------------------------------------------ */
/* OAM DMA                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    bool active;
    uint16_t src;
    uint16_t index;
    int32_t cycles;
} OAMDMA;

/* ------------------------------------------------------------------ */
/* Whole machine                                                       */
/* ------------------------------------------------------------------ */

struct GB {
    CPU cpu;
    PPU ppu;
    Timer timer;
    Joypad joypad;
    Serial serial;
    APU apu;
    OAMDMA oam_dma;

    Cart cart;

    uint8_t vram[GB_VRAM_SIZE];
    uint8_t wram[GB_WRAM_SIZE];
    uint8_t oam[GB_OAM_SIZE];
    uint8_t hram[GB_HRAM_SIZE];

    uint8_t ie;
    uint8_t iflag;
    bool boot_rom_disabled;

    uint64_t cycles;
};

/* ------------------------------------------------------------------ */
/* Core API                                                            */
/* ------------------------------------------------------------------ */

bool gb_init(GB *gb, Cart *cart);
void gb_reset(GB *gb);
bool gb_load_rom(const char *path, Cart *out_cart, char *err, size_t errlen);
void gb_free_cart(Cart *cart);

void gb_run_frame(GB *gb);

uint8_t gb_read(GB *gb, uint16_t addr);
void gb_write(GB *gb, uint16_t addr, uint8_t value);

/* Keyboard */
enum {
    GB_KEY_RIGHT = 0x01,
    GB_KEY_LEFT = 0x02,
    GB_KEY_UP = 0x04,
    GB_KEY_DOWN = 0x08,
    GB_KEY_A = 0x10,
    GB_KEY_B = 0x20,
    GB_KEY_SELECT = 0x40,
    GB_KEY_START = 0x80
};
void gb_set_keys(GB *gb, uint8_t keys);

/* ---- device entry points, all driven by the tick loop ---- */
int cpu_step(GB *gb); /* returns T-cycles consumed */
void ppu_tick(GB *gb, int cycles);
void timer_tick(GB *gb, int cycles);

/* Writing DIV, TIMA, TMA or TAC.  A write to DIV or TAC can tick TIMA on
 * its own, because either one changes what the edge detector watches. */
void timer_write(GB *gb, uint16_t addr, uint8_t value);
void apu_tick(GB *gb, int cycles);
void joypad_tick(GB *gb);
uint8_t joypad_read(const GB *gb);
void serial_tick(GB *gb, int cycles);
void oamdma_tick(GB *gb, int cycles);

/* ---- device register files ---- */
uint8_t io_read(GB *gb, uint16_t addr);
void io_write(GB *gb, uint16_t addr, uint8_t value);
void gb_request_interrupt(GB *gb, uint8_t bit);

/* ---- individual devices ---- */
void ppu_reset(GB *gb);
void ppu_lcd_on(GB *gb);
void ppu_lcd_off(GB *gb);
void ppu_update_stat(GB *gb);
void ppu_dma_start(GB *gb, uint8_t value);

/*
 * Whether the CPU can see a region's video memory right now.  The PPU owns
 * VRAM and OAM while it is fetching from them, and a CPU access that loses
 * the race is simply lost: writes go nowhere and reads return open bus.
 *
 * VRAM is reachable in modes 0, 1 and 2, and locked only during the pixel
 * transfer.  OAM is reachable in modes 0 and 1, and locked for both the
 * object scan and the transfer.  With the LCD off the PPU is not fetching,
 * so everything is reachable.
 */
bool ppu_cpu_can_read_vram(const GB *gb);
bool ppu_cpu_can_read_oam(const GB *gb);

void timer_reset(GB *gb);
uint8_t timer_div(const GB *gb);
void apu_reset(GB *gb);
void apu_write(GB *gb, uint16_t addr, uint8_t value);
void apu_set_sample_rate(APU *apu, int32_t rate);

/* ---- audio output for the frontend ---- */
size_t apu_drain(APU *apu, int16_t *dst, size_t count);
size_t apu_available(const APU *apu);

/* ---- save states ---- */
bool gb_save_state(const GB *gb, const char *path);
bool gb_load_state(GB *gb, const char *path);

/* ---- helpers shared with the tests ---- */
void gb_tick(GB *gb, int cycles);

#endif /* GB_H */