/*
 * test_apu.c -- the sound hardware: registers, the frame sequencer and the
 * four channels.
 */
#include <math.h>
#include <stdlib.h>

#include "test.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Winds the machine forward by whole T-cycles. */
static void spin(GB *gb, int cycles)
{
    apu_tick(gb, cycles);
}

/* Drains the sample ring and returns the largest magnitude seen. */
static int peak(GB *gb)
{
    int16_t buf[2 * 4096];
    int max = 0;

    apu_drain(&gb->apu, buf, 4096);
    for (int i = 0; i < 4096; i++) {
        int v = buf[i * 2] < 0 ? -buf[i * 2] : buf[i * 2];
        if (v > max)
            max = v;
    }
    return max;
}

/* Runs the APU forward and reports the peak level. */
static int play(GB *gb, int cycles)
{
    apu_drain(&gb->apu, (int16_t[64]){ 0 }, 32);
    spin(gb, cycles);
    return peak(gb);
}

/* Mean absolute level, which reflects the duty cycle and the wave volume
 * shift far better than the peak does, because the peak is dominated by
 * the filter's very first step. */
static int level(GB *gb, int cycles)
{
    int16_t buf[2 * 4096];
    size_t queued, i;
    long total = 0;

    apu_drain(&gb->apu, (int16_t[64]){ 0 }, 32);
    spin(gb, cycles);

    queued = apu_available(&gb->apu);
    if (queued > 4096)
        queued = 4096;
    apu_drain(&gb->apu, buf, queued);

    for (i = 0; i < queued; i++)
        total += buf[i * 2] < 0 ? -buf[i * 2] : buf[i * 2];

    return queued ? (int)(total / (long)queued) : 0;
}

/* Starts channel 1 with a square wave at the given frequency. */
static void start_square1(GB *gb, uint16_t freq, uint8_t volume, uint8_t duty)
{
    gb_write(gb, 0xFF10, 0x00);      /* no sweep        */
    gb_write(gb, 0xFF11, (uint8_t)((duty & 3) << 6));
    gb_write(gb, 0xFF12, (uint8_t)(0xF0 | (volume & 0x0F)));
    gb_write(gb, 0xFF13, (uint8_t)(freq & 0xFF));
    gb_write(gb, 0xFF14, (uint8_t)(0x80 | ((freq >> 8) & 0x07)));
    apu_write(gb, 0xFF14, gb->apu.nr14);
}

static void set_master(GB *gb)
{
    gb_write(gb, 0xFF24, 0x77); /* full volume on both sides */
    gb_write(gb, 0xFF25, 0xFF); /* every channel to both sides */
}

/* ------------------------------------------------------------------ */
/* Register file                                                       */
/* ------------------------------------------------------------------ */

static void t_apu_nr52_power_bit(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x80, 0x80); /* powered after reset */

    gb_write(&f.gb, 0xFF26, 0x00);
    CHECK_EQ(f.gb.apu.powered, false);
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x80, 0x00);
    /* Every other register reads as zero while the APU is off. */
    CHECK_EQ(gb_read(&f.gb, 0xFF24), 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xFF25), 0x00);

    gb_write(&f.gb, 0xFF26, 0x80);
    CHECK_EQ(f.gb.apu.powered, true);
    /* The status bits track running channels, and none are running yet. */
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x0F, 0x00);
    fixture_destroy(&f);
}

static void t_apu_channel_status_bits(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* Post-boot NR52 is $F1: the first square channel's DAC is enabled,
     * so its status bit already reads set before anything is triggered. */
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x80, 0x80);
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x0F, 0x01);

    start_square1(&f.gb, 0x0600, 0x0F, 2);
    apu_tick(&f.gb, 4);
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x0F, 0x01);

    /* Channels two to four start with their DACs off. */
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x0E, 0x00);

    /* Turning the DAC off stops the channel again. */
    gb_write(&f.gb, 0xFF12, 0x00);
    apu_write(&f.gb, 0xFF14, 0x80);
    apu_tick(&f.gb, 4);
    CHECK_EQ(gb_read(&f.gb, 0xFF26) & 0x0F, 0x00);
    fixture_destroy(&f);
}

static void t_apu_read_masks(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* NR11 reads with the low six bits open. */
    gb_write(&f.gb, 0xFF11, 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0xFF11), 0xFF);

    /* NR13 is write only and always reads $FF. */
    gb_write(&f.gb, 0xFF13, 0x5A);
    CHECK_EQ(gb_read(&f.gb, 0xFF13), 0xFF);

    /* NR12 is fully writable: initial volume, direction and period. */
    gb_write(&f.gb, 0xFF12, 0xFF);
    CHECK_EQ(f.gb.apu.nr12, 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0xFF12), 0x00); /* reads back as open bus */

    /* NR30 only holds the DAC power bit. */
    gb_write(&f.gb, 0xFF1A, 0xFF);
    CHECK_EQ(f.gb.apu.nr30, 0x80);
    CHECK_EQ(gb_read(&f.gb, 0xFF1A) & 0x80, 0x80);
    gb_write(&f.gb, 0xFF1A, 0x00);
    CHECK_EQ(gb_read(&f.gb, 0xFF1A) & 0x80, 0x00);
    fixture_destroy(&f);
}

static void t_apu_wave_ram_round_trip(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    for (int i = 0; i < 16; i++)
        gb_write(&f.gb, (uint16_t)(0xFF30 + i), (uint8_t)(i * 0x11));

    for (int i = 0; i < 16; i++)
        CHECK_EQ(gb_read(&f.gb, (uint16_t)(0xFF30 + i)), (uint8_t)(i * 0x11));
    fixture_destroy(&f);
}

static void t_apu_unmapped_sound_registers(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    CHECK_EQ(gb_read(&f.gb, 0xFF15), 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0xFF1F), 0xFF);
    CHECK_EQ(gb_read(&f.gb, 0xFF27), 0xFF);
    fixture_destroy(&f);
}

/*
 * A freshly reset machine already has channel one's DAC enabled, because
 * NR12 is $F3 after the boot ROM.  Tests that care about what starts from
 * silence turn it off first, rather than assuming a blank slate.
 */
static void make_silent(GB *gb)
{
    gb_write(gb, 0xFF12, 0x00);   /* channel 1 DAC off   */
    gb_write(gb, 0xFF17, 0x00);   /* channel 2 DAC off   */
    gb_write(gb, 0xFF1A, 0x00);   /* wave DAC off        */
    gb_write(gb, 0xFF21, 0x00);   /* noise DAC off       */
    apu_write(gb, 0xFF26, 0x80);  /* power stays on      */
    /* The DAC state is reworked on the next tick, not by the write. */
    apu_tick(gb, 4);
}

static void t_apu_trigger_starts_the_channel(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    make_silent(&f.gb);
    CHECK_EQ(f.gb.apu.ch1_on, false);
    start_square1(&f.gb, 0x0600, 0x0F, 0);
    CHECK_EQ(f.gb.apu.ch1_on, true);
    CHECK_EQ(f.gb.apu.ch1_len, 64); /* nothing was set in NR11 */
    fixture_destroy(&f);
}

static void t_apu_zero_volume_nr12_leaves_the_channel_off(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    /* The top five bits of NR12 are the DAC enable; all zero means off. */
    gb_write(&f.gb, 0xFF10, 0x00);
    gb_write(&f.gb, 0xFF11, 0x00);
    gb_write(&f.gb, 0xFF12, 0x00);
    gb_write(&f.gb, 0xFF13, 0x00);
    gb_write(&f.gb, 0xFF14, 0x00);
    apu_write(&f.gb, 0xFF14, 0x80);
    CHECK_EQ(f.gb.apu.ch1_on, false);
    fixture_destroy(&f);
}

static void t_apu_silence_when_the_apu_is_off(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    start_square1(&f.gb, 0x0600, 0x0F, 2);
    CHECK(play(&f.gb, 20000) > 0);

    gb_write(&f.gb, 0xFF26, 0x00);
    CHECK_EQ(play(&f.gb, 4000), 0);
    fixture_destroy(&f);
}

static void t_apu_nr50_zero_mutes(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF25, 0xFF);
    gb_write(&f.gb, 0xFF24, 0x00); /* volume bits zero on both sides */
    start_square1(&f.gb, 0x0600, 0x0F, 0);

    CHECK_EQ(play(&f.gb, 4000), 0);
    fixture_destroy(&f);
}

static void t_apu_nr51_routing(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF24, 0x77);
    gb_write(&f.gb, 0xFF25, 0x01); /* channel 1 to the left only */
    start_square1(&f.gb, 0x0600, 0x0F, 2);

    apu_drain(&f.gb.apu, (int16_t[64]){ 0 }, 32);
    spin(&f.gb, 20000);
    {
        int16_t buf[2 * 256];
        int left = 0, right = 0;

        apu_drain(&f.gb.apu, buf, 256);
        for (int i = 0; i < 256; i++) {
            int l = abs(buf[i * 2]);
            int r = abs(buf[i * 2 + 1]);
            if (l > left) left = l;
            if (r > right) right = r;
        }
        CHECK_MSG(left > 0, "the left channel should carry signal");
        CHECK_EQ(right, 0);
    }
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Channel 1 and 2                                                      */
/* ------------------------------------------------------------------ */

static void t_apu_square_produces_sound(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    CHECK(play(&f.gb, 4000) == 0); /* nothing playing yet */

    start_square1(&f.gb, 0x0600, 0x0F, 2);
    CHECK_MSG(play(&f.gb, 20000) > 0, "a running square channel must be audible");
    fixture_destroy(&f);
}

static void t_apu_duty_changes_the_waveform(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int peaks[4];

    set_master(&f.gb);
    for (int duty = 0; duty < 4; duty++) {
        start_square1(&f.gb, 0x0400, 0x0F, (uint8_t)duty);
        peaks[duty] = level(&f.gb, 20000);
    }

    /* A 12.5% duty wave is quiet and a 50% one is loud. */
    CHECK_MSG(peaks[0] < peaks[2], "12.5%% duty (%d) should be quieter than 50%% (%d)",
              peaks[0], peaks[2]);
    fixture_destroy(&f);
}

static void t_apu_envelope_decays_the_volume(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    /* Volume 15 stepping down every tick, with a short period. */
    gb_write(&f.gb, 0xFF10, 0x00);
    gb_write(&f.gb, 0xFF11, 0x00);
    gb_write(&f.gb, 0xFF12, 0xF1);
    gb_write(&f.gb, 0xFF13, 0x00);
    gb_write(&f.gb, 0xFF14, 0x00);
    apu_write(&f.gb, 0xFF14, 0x80);

    CHECK_EQ(f.gb.apu.ch1_vol, 15);
    /* The frame sequencer runs at 8192 Hz and the envelope moves on its
     * final step, so give it a few of those. */
    spin(&f.gb, 512 * 80);
    CHECK(f.gb.apu.ch1_vol < 15);
    fixture_destroy(&f);
}

static void t_apu_length_counter_stops_the_channel(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    /* Length 1, with the length counter enabled. */
    gb_write(&f.gb, 0xFF10, 0x00);
    gb_write(&f.gb, 0xFF11, 0x3F);
    gb_write(&f.gb, 0xFF12, 0xF0);
    gb_write(&f.gb, 0xFF13, 0x00);
    apu_write(&f.gb, 0xFF14, 0xC0); /* trigger plus length enable */
    CHECK_EQ(f.gb.apu.ch1_on, true);
    CHECK_EQ(f.gb.apu.ch1_len, 1);
    CHECK_EQ(f.gb.apu.ch1_len_enable, true);

    /* One frame sequencer length step happens every 2048 T-cycles. */
    spin(&f.gb, 512 * 12);
    CHECK_EQ(f.gb.apu.ch1_on, false);
    CHECK_EQ(play(&f.gb, 20000), 0);
    fixture_destroy(&f);
}

static void t_apu_length_counter_only_runs_when_enabled(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    gb_write(&f.gb, 0xFF10, 0x00);
    gb_write(&f.gb, 0xFF11, 0xBF); /* 50% duty, length 63 */
    gb_write(&f.gb, 0xFF12, 0xF0);
    gb_write(&f.gb, 0xFF13, 0x00);
    apu_write(&f.gb, 0xFF14, 0x80); /* trigger with the length enable clear */

    spin(&f.gb, 512 * 32);
    CHECK_EQ(f.gb.apu.ch1_on, true);
    CHECK_EQ(play(&f.gb, 20000) > 0, true);
    fixture_destroy(&f);
}

static void t_apu_square_frequency_bits(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    gb_write(&f.gb, 0xFF10, 0x00);
    gb_write(&f.gb, 0xFF11, 0x00);
    gb_write(&f.gb, 0xFF12, 0xF0);
    gb_write(&f.gb, 0xFF13, 0x34);
    gb_write(&f.gb, 0xFF14, 0x86);
    apu_tick(&f.gb, 4);

    CHECK_EQ(f.gb.apu.ch1_freq, 0x634);
    fixture_destroy(&f);
}

static void t_apu_square2_is_independent(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    make_silent(&f.gb);
    set_master(&f.gb);
    gb_write(&f.gb, 0xFF16, 0x80); /* 50% duty */
    gb_write(&f.gb, 0xFF17, 0xF0);
    gb_write(&f.gb, 0xFF18, 0x00);
    gb_write(&f.gb, 0xFF19, 0x07); /* high frequency bits */
    apu_write(&f.gb, 0xFF19, 0x87);

    CHECK_EQ(f.gb.apu.ch2_on, true);
    CHECK_EQ(f.gb.apu.ch1_on, false);
    CHECK(play(&f.gb, 20000) > 0);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Sweep                                                               */
/* ------------------------------------------------------------------ */

static void t_apu_sweep_off_when_period_is_zero(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF10, 0x00); /* period 0, shift 0 */
    gb_write(&f.gb, 0xFF11, 0x00);
    gb_write(&f.gb, 0xFF12, 0xF0);
    gb_write(&f.gb, 0xFF13, 0x00);
    gb_write(&f.gb, 0xFF14, 0x00);
    apu_write(&f.gb, 0xFF14, 0x80);

    CHECK_EQ(f.gb.apu.ch1_sweep_enabled, false);
    fixture_destroy(&f);
}

static void t_apu_sweep_raises_the_frequency(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    /* Period 1, shift 1: the frequency gains its own half every step. */
    gb_write(&f.gb, 0xFF10, 0x11);
    gb_write(&f.gb, 0xFF11, 0x00);
    gb_write(&f.gb, 0xFF12, 0xF0);
    gb_write(&f.gb, 0xFF13, 0x00);
    gb_write(&f.gb, 0xFF14, 0x04); /* start at $0400 */
    apu_write(&f.gb, 0xFF14, 0x84);

    /* Let the frame sequencer run so the sweep steps. */
    spin(&f.gb, 512 * 10);
    CHECK_MSG(f.gb.apu.ch1_shadow > 0x400,
              "the sweep should raise the frequency, still at %04X",
              f.gb.apu.ch1_shadow);
    fixture_destroy(&f);
}

static void t_apu_sweep_overflow_kills_the_channel(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    /* Starting near the top of the range with a large shift means the very
     * next sweep calculation pushes the frequency past 2047. */
    gb_write(&f.gb, 0xFF10, 0x01);
    gb_write(&f.gb, 0xFF11, 0x00);
    gb_write(&f.gb, 0xFF12, 0xF0);
    gb_write(&f.gb, 0xFF13, 0xF0);
    gb_write(&f.gb, 0xFF14, 0x87);

    /* The very first calculation already pushes past 2047. */
    CHECK_EQ(f.gb.apu.ch1_on, false);
    spin(&f.gb, 512 * 4);
    CHECK_EQ(f.gb.apu.ch1_on, false);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Wave channel                                                        */
/* ------------------------------------------------------------------ */

static void t_apu_wave_needs_the_dac_enabled(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    gb_write(&f.gb, 0xFF1A, 0x00); /* DAC off */
    gb_write(&f.gb, 0xFF1B, 0x00);
    gb_write(&f.gb, 0xFF1C, 0x20);
    gb_write(&f.gb, 0xFF33, 0x00);
    gb_write(&f.gb, 0xFF34, 0x00);
    apu_write(&f.gb, 0xFF34, 0x80);

    CHECK_EQ(f.gb.apu.ch3_on, false);
    CHECK_EQ(play(&f.gb, 4000), 0);
    fixture_destroy(&f);
}

static void t_apu_wave_produces_sound(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    for (int i = 0; i < 16; i++)
        gb_write(&f.gb, (uint16_t)(0xFF30 + i), (uint8_t)(i * 0x11));

    gb_write(&f.gb, 0xFF1A, 0x80); /* DAC on  */
    gb_write(&f.gb, 0xFF1B, 0x00);
    gb_write(&f.gb, 0xFF1C, 0x20); /* 100%    */
    gb_write(&f.gb, 0xFF33, 0x00);
    gb_write(&f.gb, 0xFF34, 0x00);
    apu_write(&f.gb, 0xFF34, 0x80);

    CHECK_EQ(f.gb.apu.ch3_on, true);
    CHECK_MSG(play(&f.gb, 20000) > 0, "the wave channel must be audible");
    fixture_destroy(&f);
}

static void t_apu_wave_volume_shifts(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int levels[4];

    set_master(&f.gb);
    /* A rising ramp, so the channel has real alternating content: a
     * constant table would just be filtered away as a DC offset. */
    for (int i = 0; i < 16; i++)
        gb_write(&f.gb, (uint16_t)(0xFF30 + i), (uint8_t)((i << 4) | i));

    for (int code = 1; code < 4; code++) {
        gb_write(&f.gb, 0xFF1A, 0x80);
        gb_write(&f.gb, 0xFF1B, 0x00);
        gb_write(&f.gb, 0xFF1C, (uint8_t)(code << 5));
        gb_write(&f.gb, 0xFF33, 0x00);
        gb_write(&f.gb, 0xFF34, 0x07);
        apu_write(&f.gb, 0xFF34, 0x87);
        levels[code] = level(&f.gb, 20000);
    }

    /* 100% > 50% > 25%. */
    CHECK_MSG(levels[1] > levels[2], "100%% (%d) should beat 50%% (%d)", levels[1],
              levels[2]);
    CHECK_MSG(levels[2] > levels[3], "50%% (%d) should beat 25%% (%d)", levels[2],
              levels[3]);
    fixture_destroy(&f);
}

static void t_apu_wave_length_counter(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF1A, 0x80);
    gb_write(&f.gb, 0xFF1B, 0xFF); /* length 1 */
    gb_write(&f.gb, 0xFF1C, 0x20);
    gb_write(&f.gb, 0xFF33, 0x00);
    gb_write(&f.gb, 0xFF34, 0x00);
    apu_write(&f.gb, 0xFF34, 0xC0); /* trigger plus length enable */

    CHECK_EQ(f.gb.apu.ch3_on, true);
    spin(&f.gb, 512 * 12);
    CHECK_EQ(f.gb.apu.ch3_on, false);
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Noise channel                                                       */
/* ------------------------------------------------------------------ */

static void t_apu_noise_produces_sound(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    gb_write(&f.gb, 0xFF21, 0xF0);  /* NR42: envelope, volume 15 */
    gb_write(&f.gb, 0xFF22, 0x30);  /* NR43: shift 3, divisor 0  */
    gb_write(&f.gb, 0xFF23, 0x80);  /* NR44: length and trigger  */

    CHECK_EQ(f.gb.apu.ch4_on, true);
    CHECK_MSG(play(&f.gb, 20000) > 0, "the noise channel must be audible");
    fixture_destroy(&f);
}

static void t_apu_noise_lfsr_relaxes(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    uint16_t start = f.gb.apu.ch4_lfsr;

    CHECK_EQ(start, 0x7FFF);

    gb_write(&f.gb, 0xFF22, 0x00);
    gb_write(&f.gb, 0xFF23, 0x80);
    spin(&f.gb, 2000);
    CHECK_NE(f.gb.apu.ch4_lfsr, 0x7FFF);
    fixture_destroy(&f);
}

static void t_apu_noise_seven_bit_mode(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    bool saw_bit6_cleared = false;

    gb_write(&f.gb, 0xFF22, 0x08); /* width bit set -> 7 bit LFSR */
    gb_write(&f.gb, 0xFF23, 0x80); /* NR44: length and trigger    */

    for (int i = 0; i < 4000; i++) {
        spin(&f.gb, 1);
        if ((f.gb.apu.ch4_lfsr & 0x40) == 0) {
            saw_bit6_cleared = true;
            break;
        }
    }
    CHECK_MSG(saw_bit6_cleared, "bit 6 of the LFSR is never driven in 7 bit mode");
    fixture_destroy(&f);
}

/* ------------------------------------------------------------------ */
/* Frame sequencer and output                                          */
/* ------------------------------------------------------------------ */

static void t_apu_frame_sequencer_steps(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.apu.fs_step = 0;
    spin(&f.gb, 512);
    CHECK_EQ(f.gb.apu.fs_step, 1);
    spin(&f.gb, 512 * 7);
    CHECK_EQ(f.gb.apu.fs_step, 0); /* it wraps every eight steps */
    fixture_destroy(&f);
}

static void t_apu_frame_sequencer_stops_when_powered_off(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    gb_write(&f.gb, 0xFF26, 0x00);
    f.gb.apu.fs_step = 0;
    spin(&f.gb, 512 * 4);
    CHECK_EQ(f.gb.apu.fs_step, 0);
    fixture_destroy(&f);
}

static void t_apu_sample_rate_is_respected(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    set_master(&f.gb);
    start_square1(&f.gb, 0x0600, 0x0F, 0);

    apu_set_sample_rate(&f.gb.apu, 8000);
    apu_drain(&f.gb.apu, (int16_t[64]){ 0 }, 32);
    spin(&f.gb, GB_CYCLES_PER_FRAME);
    {
        int16_t buf[2 * 16384];
        size_t got = apu_drain(&f.gb.apu, buf, 16384);
        /* 8000 Hz over a 59.7 Hz frame is about 134 samples. */
        CHECK_MSG(got > 120 && got < 150,
                  "expected about 134 samples at 8 kHz, got %zu", got);
    }
    apu_set_sample_rate(&f.gb.apu, 44100);
    fixture_destroy(&f);
}

static void t_apu_ring_buffer_drains_in_order(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int16_t buf[2 * 16];

    set_master(&f.gb);
    apu_drain(&f.gb.apu, buf, 16); /* start empty */
    start_square1(&f.gb, 0x0600, 0x0F, 0);
    spin(&f.gb, 20000);

    CHECK(apu_available(&f.gb.apu) > 0);
    CHECK_EQ(apu_drain(&f.gb.apu, buf, 16), 16);

    /* Draining an empty ring returns silence rather than blocking. */
    {
        int16_t quiet[2 * 8];
        memset(quiet, 0, sizeof(quiet));
        apu_drain(&f.gb.apu, quiet, 8);
        for (int i = 0; i < 8; i++) {
            CHECK_EQ(quiet[i], 0);
            CHECK_EQ(quiet[i * 2 + 1], 0);
        }
    }
    fixture_destroy(&f);
}

static void t_apu_output_is_stereo_mono_when_panned_hard(void)
{
    Fixture f = fixture_create(0x8000, 0x00);
    int16_t buf[2 * 256];

    set_master(&f.gb);
    start_square1(&f.gb, 0x0600, 0x0F, 2);
    spin(&f.gb, 20000);
    apu_drain(&f.gb.apu, buf, 256);

    for (int i = 0; i < 256; i++)
        CHECK_EQ(buf[i * 2], buf[i * 2 + 1]);
    fixture_destroy(&f);
}

static void t_apu_reset_values(void)
{
    Fixture f = fixture_create(0x8000, 0x00);

    f.gb.apu.nr50 = 0x00;
    apu_reset(&f.gb);
    CHECK_EQ(f.gb.apu.powered, true);
    CHECK_EQ(f.gb.apu.nr50, 0x77);
    CHECK_EQ(f.gb.apu.nr51, 0xF3);
    /* NR12 is $F3 post-boot, so channel one's DAC is on even before
     * anything is triggered. */
    CHECK_EQ(f.gb.apu.ch1_on, true);
    CHECK_EQ(f.gb.apu.nr12, 0xF3);
    CHECK_EQ(f.gb.apu.ch4_lfsr, 0x7FFF);
    fixture_destroy(&f);
}

const TestCase apu_tests[] = {
    { "apu", "nr52 power bit", t_apu_nr52_power_bit },
    { "apu", "channel status bits", t_apu_channel_status_bits },
    { "apu", "read masks", t_apu_read_masks },
    { "apu", "wave ram round trip", t_apu_wave_ram_round_trip },
    { "apu", "unmapped sound registers", t_apu_unmapped_sound_registers },
    { "apu", "trigger starts the channel", t_apu_trigger_starts_the_channel },
    { "apu", "zero volume nr12 leaves the channel off", t_apu_zero_volume_nr12_leaves_the_channel_off },
    { "apu", "silence when the apu is off", t_apu_silence_when_the_apu_is_off },
    { "apu", "nr50 zero mutes", t_apu_nr50_zero_mutes },
    { "apu", "nr51 routing", t_apu_nr51_routing },
    { "apu", "square produces sound", t_apu_square_produces_sound },
    { "apu", "duty changes the waveform", t_apu_duty_changes_the_waveform },
    { "apu", "envelope decays the volume", t_apu_envelope_decays_the_volume },
    { "apu", "length counter stops the channel", t_apu_length_counter_stops_the_channel },
    { "apu", "length counter only runs when enabled", t_apu_length_counter_only_runs_when_enabled },
    { "apu", "square frequency bits", t_apu_square_frequency_bits },
    { "apu", "square 2 is independent", t_apu_square2_is_independent },
    { "apu", "sweep off when period is zero", t_apu_sweep_off_when_period_is_zero },
    { "apu", "sweep raises the frequency", t_apu_sweep_raises_the_frequency },
    { "apu", "sweep overflow kills the channel", t_apu_sweep_overflow_kills_the_channel },
    { "apu", "wave needs the dac enabled", t_apu_wave_needs_the_dac_enabled },
    { "apu", "wave produces sound", t_apu_wave_produces_sound },
    { "apu", "wave volume shifts", t_apu_wave_volume_shifts },
    { "apu", "wave length counter", t_apu_wave_length_counter },
    { "apu", "noise produces sound", t_apu_noise_produces_sound },
    { "apu", "noise lfsr relaxes", t_apu_noise_lfsr_relaxes },
    { "apu", "noise seven bit mode", t_apu_noise_seven_bit_mode },
    { "apu", "frame sequencer steps", t_apu_frame_sequencer_steps },
    { "apu", "frame sequencer stops when powered off", t_apu_frame_sequencer_stops_when_powered_off },
    { "apu", "sample rate is respected", t_apu_sample_rate_is_respected },
    { "apu", "ring buffer drains in order", t_apu_ring_buffer_drains_in_order },
    { "apu", "output is mono when panned hard", t_apu_output_is_stereo_mono_when_panned_hard },
    { "apu", "reset values", t_apu_reset_values },
    TEST_SUITE_END
};