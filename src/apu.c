/*
 * apu.c -- the DMG sound hardware: two square channels with sweep and
 * envelope, the wave channel and the noise channel, plus the frame
 * sequencer that clocks length counters, sweep and envelope.
 *
 * Samples are mixed down to stereo into a ring buffer that the frontend
 * drains whenever it wants audio.
 */
#include <string.h>
#include <math.h>

#include "gb.h"

/* Period of one frame sequencer step, in T-cycles (8192 Hz). */
#define FS_PERIOD 512u

/* Scales the filtered signal onto the 16 bit range. */
#define APU_GAIN 28.0

/* Corner frequency of the DC blocking capacitor, in Hz. */
#define APU_CAP_HZ 20.0

static const uint8_t duty_table[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 }
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void ch1_sweep_calculate(APU *apu);

/* ------------------------------------------------------------------ */
/* Channel triggers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Loading the envelope is separate from triggering: a write to NRx2 puts
 * the new envelope in force immediately, while a trigger additionally
 * reloads the length counter, the period and the duty position.  Without
 * the split, a game that zeroes NR12 to silence a channel would keep
 * playing it until the next trigger.
 */
static void load_envelope_square(APU *apu, int channel, uint8_t nrx2)
{
    uint8_t volume = (uint8_t)(nrx2 >> 4);
    uint8_t period = (uint8_t)(nrx2 & 7);

    if (channel == 1) {
        apu->ch1_vol = volume;
        apu->ch1_env_initial = volume;
        apu->ch1_env_dir = (uint8_t)((nrx2 >> 3) & 1);
        apu->ch1_env_period = period;
        apu->ch1_env_timer = period;
    } else {
        apu->ch2_vol = volume;
        apu->ch2_env_initial = volume;
        apu->ch2_env_dir = (uint8_t)((nrx2 >> 3) & 1);
        apu->ch2_env_period = period;
        apu->ch2_env_timer = period;
    }
}

static void load_envelope_noise(APU *apu, uint8_t nr42)
{
    uint8_t volume = (uint8_t)(nr42 >> 4);
    uint8_t period = (uint8_t)(nr42 & 7);

    apu->ch4_vol = volume;
    apu->ch4_env_initial = volume;
    apu->ch4_env_dir = (uint8_t)((nr42 >> 3) & 1);
    apu->ch4_env_period = period;
    apu->ch4_env_timer = period;
}

static void trigger_square(APU *apu, int channel)
{
    if (channel == 1) {
        apu->ch1_on = (apu->nr12 & 0xF8) != 0;
        apu->ch1_len = (uint8_t)(64 - (apu->nr11 & 0x3F));
        apu->ch1_timer = 0;
        apu->ch1_duty_pos = 0;
        load_envelope_square(apu, 1, apu->nr12);

        apu->ch1_sweep_period = (uint16_t)((apu->nr10 >> 4) & 7);
        apu->ch1_sweep_negate = (apu->nr10 & 0x08) != 0;
        apu->ch1_sweep_shift = (uint8_t)(apu->nr10 & 7);
        apu->ch1_sweep_enabled =
            apu->ch1_sweep_period != 0 || apu->ch1_sweep_shift != 0;
        apu->ch1_sweep_timer =
            (uint8_t)(apu->ch1_sweep_period ? apu->ch1_sweep_period : 8);
        apu->ch1_shadow =
            (uint16_t)(apu->nr13 | ((apu->nr14 & 0x07) << 8));
        if (apu->ch1_sweep_shift)
            ch1_sweep_calculate(apu);
    } else {
        apu->ch2_on = (apu->nr22 & 0xF8) != 0;
        apu->ch2_len = (uint8_t)(64 - (apu->nr21 & 0x3F));
        apu->ch2_timer = 0;
        apu->ch2_duty_pos = 0;
        load_envelope_square(apu, 2, apu->nr22);
    }
}

static void trigger_wave(APU *apu)
{
    apu->ch3_on = (apu->nr30 & 0x80) != 0;
    apu->ch3_dac = (apu->nr30 & 0x80) != 0;
    apu->ch3_len = (uint8_t)(256 - apu->nr31);
    apu->ch3_timer = 0;
    apu->ch3_pos = 0;
}

static void trigger_noise(APU *apu)
{
    apu->ch4_on = (apu->nr42 & 0xF8) != 0;
    apu->ch4_len = (uint8_t)(64 - (apu->nr41 & 0x3F));
    apu->ch4_timer = 0;
    load_envelope_noise(apu, apu->nr42);
    apu->ch4_lfsr = 0x7FFF;
}

/* ------------------------------------------------------------------ */
/* Channel 1 frequency sweep                                           */
/* ------------------------------------------------------------------ */

static uint16_t ch1_sweep_new_freq(const APU *apu)
{
    uint16_t delta = (uint16_t)(apu->ch1_shadow >> apu->ch1_sweep_shift);

    if (apu->ch1_sweep_negate)
        return (uint16_t)(apu->ch1_shadow - delta);
    return (uint16_t)(apu->ch1_shadow + delta);
}

static void ch1_sweep_calculate(APU *apu)
{
    uint16_t next = ch1_sweep_new_freq(apu);

    if (next > 2047) {
        apu->ch1_on = false; /* a sweep past the limit kills the channel */
        return;
    }
    if (apu->ch1_sweep_shift)
        apu->ch1_shadow = next;
}

static void ch1_sweep_step(APU *apu)
{
    if (apu->ch1_sweep_timer > 0)
        apu->ch1_sweep_timer--;
    if (apu->ch1_sweep_timer)
        return;

    if (apu->ch1_sweep_enabled) {
        ch1_sweep_calculate(apu);
        apu->ch1_sweep_timer =
            (uint8_t)(apu->ch1_sweep_period ? apu->ch1_sweep_period : 8);
    }
}

/* ------------------------------------------------------------------ */
/* Envelope                                                            */
/* ------------------------------------------------------------------ */

static void envelope_step(uint8_t *vol, uint8_t *timer, uint8_t direction,
                          uint8_t period)
{
    if (period == 0)
        return;
    if (*timer > 0)
        (*timer)--;
    if (*timer)
        return;

    *timer = period;
    if (direction) {
        if (*vol < 15)
            (*vol)++;
    } else if (*vol > 0) {
        (*vol)--;
    }
}

/* ------------------------------------------------------------------ */
/* Frame sequencer                                                     */
/* ------------------------------------------------------------------ */

static void length_step(uint8_t *len, uint8_t *len_enable, bool *on)
{
    if (*len_enable && *len > 0) {
        (*len)--;
        if (*len == 0)
            *on = false;
    }
}

static void frame_sequencer_step(GB *gb)
{
    APU *apu = &gb->apu;
    int step = apu->fs_step & 7;

    /* Squares clock their length counters on steps 0, 2, 4 and 6. */
    if ((step & 1) == 0) {
        length_step(&apu->ch1_len, &apu->ch1_len_enable, &apu->ch1_on);
        length_step(&apu->ch2_len, &apu->ch2_len_enable, &apu->ch2_on);
    }
    if (step == 7) {
        length_step(&apu->ch3_len, &apu->ch3_len_enable, &apu->ch3_on);
        length_step(&apu->ch4_len, &apu->ch4_len_enable, &apu->ch4_on);
    }
    if (step == 2 || step == 6)
        ch1_sweep_step(apu);
    if (step == 7) {
        envelope_step(&apu->ch1_vol, &apu->ch1_env_timer, apu->ch1_env_dir,
                      apu->ch1_env_period);
        envelope_step(&apu->ch2_vol, &apu->ch2_env_timer, apu->ch2_env_dir,
                      apu->ch2_env_period);
        envelope_step(&apu->ch4_vol, &apu->ch4_env_timer, apu->ch4_env_dir,
                      apu->ch4_env_period);
    }

    apu->fs_step = (uint8_t)((apu->fs_step + 1) & 7);
}

/* ------------------------------------------------------------------ */
/* Channel output                                                      */
/* ------------------------------------------------------------------ */

/* The duty pattern lives in bits 7-6 of NR11 / NR21. */
static uint8_t square_output(uint8_t duty_reg, uint8_t pos, uint8_t vol)
{
    return duty_table[(duty_reg >> 6) & 3][pos & 7] ? vol : 0;
}

static uint8_t wave_output(const APU *apu)
{
    uint8_t byte = apu->ch3_wave[apu->ch3_pos >> 1];
    uint8_t sample = (apu->ch3_pos & 1) ? (uint8_t)(byte & 0x0F)
                                        : (uint8_t)(byte >> 4);

    switch ((apu->nr32 & 0x60) >> 5) {
    case 0: return 0;
    case 1: return sample;
    case 2: return (uint8_t)(sample >> 1);
    default: return (uint8_t)(sample >> 2);
    }
}

static uint8_t noise_output(const APU *apu)
{
    return (apu->ch4_lfsr & 1) ? 0 : apu->ch4_vol;
}

static void mix_sample(GB *gb, int16_t *dst)
{
    APU *apu = &gb->apu;
    uint8_t out[4];
    int left = 0, right = 0;
    double rvol = (double)(apu->nr50 & 0x07);
    double lvol = (double)((apu->nr50 >> 4) & 0x07);

    out[0] = apu->ch1_on ? square_output(apu->nr11, apu->ch1_duty_pos, apu->ch1_vol) : 0;
    out[1] = apu->ch2_on ? square_output(apu->nr21, apu->ch2_duty_pos, apu->ch2_vol) : 0;
    out[2] = (apu->ch3_on && apu->ch3_dac) ? wave_output(apu) : 0;
    out[3] = apu->ch4_on ? noise_output(apu) : 0;

    for (int i = 0; i < 4; i++) {
        if (apu->nr51 & (0x10u << i))
            right += out[i];
        if (apu->nr51 & (0x01u << i))
            left += out[i];
    }

    /* Each side has its own output stage, so panning really does move the
     * signal between them. */
    {
        double raw_l = (double)left * lvol;
        double raw_r = (double)right * rvol;
        double hp_l = raw_l - apu->cap_left;
        double hp_r = raw_r - apu->cap_right;

        /* The DMG's output capacitor blocks the DC offset. */
        apu->cap_left = raw_l - hp_l * apu->hp_coeff;
        apu->cap_right = raw_r - hp_r * apu->hp_coeff;

        dst[0] = (int16_t)(hp_l * APU_GAIN);
        dst[1] = (int16_t)(hp_r * APU_GAIN);
    }
}

/* NR52 bits 0-3 mirror which channels are currently running. */
static void update_status(APU *apu)
{
    uint8_t status = 0;

    if (apu->ch1_on) status |= 0x01;
    if (apu->ch2_on) status |= 0x02;
    if (apu->ch3_on) status |= 0x04;
    if (apu->ch4_on) status |= 0x08;

    apu->nr52 = (uint8_t)((apu->nr52 & 0x70) | status);
}

static void ring_push(APU *apu, int16_t left, int16_t right)
{
    uint32_t next = (apu->ring_write + 1) % APU_RING_SIZE;

    if (next == apu->ring_read) {
        apu->dropped++; /* never overwrite a sample that is still queued */
        return;
    }
    apu->ring[apu->ring_write * 2 + 0] = left;
    apu->ring[apu->ring_write * 2 + 1] = right;
    apu->ring_write = next;
}

/* ------------------------------------------------------------------ */
/* Register writes with side effects                                   */
/* ------------------------------------------------------------------ */

void apu_write(GB *gb, uint16_t addr, uint8_t value)
{
    APU *apu = &gb->apu;

    switch (addr) {
    case 0xFF14: /* NR14 -- channel 1 length and trigger */
        apu->nr14 = (uint8_t)(value & 0x47);
        apu->ch1_len_enable = (value & 0x40) != 0;
        if (value & 0x80)
            trigger_square(apu, 1);
        break;

    case 0xFF19: /* NR24 -- channel 2 */
        apu->nr24 = (uint8_t)(value & 0x47);
        apu->ch2_len_enable = (value & 0x40) != 0;
        if (value & 0x80)
            trigger_square(apu, 2);
        break;

    case 0xFF26: /* NR52 -- master power switch */
        if (!(value & 0x80)) {
            apu->powered = false;
            apu->nr50 = 0;
            apu->nr51 = 0;
            apu->nr52 = 0;
            apu->ch1_on = false;
            apu->ch2_on = false;
            apu->ch3_on = false;
            apu->ch4_on = false;
            update_status(apu);
        } else if (!apu->powered) {
            apu->powered = true;
            update_status(apu);
        }
        break;

    case 0xFF1A: /* NR30 -- wave DAC power */
        apu->nr30 = (uint8_t)(value & 0x80);
        apu->ch3_dac = (value & 0x80) != 0;
        if (!apu->ch3_dac)
            apu->ch3_on = false;
        break;

    case 0xFF34: /* NR34 -- wave length and trigger */
        apu->nr34 = (uint8_t)(value & 0x47);
        apu->ch3_len_enable = (value & 0x40) != 0;
        if (value & 0x80)
            trigger_wave(apu);
        break;

    /*
     * The top five bits of NRx2 are the DAC enable.  Writing zero there
     * switches the DAC off straight away, which silences the channel
     * without waiting for the next trigger; writing a non-zero value turns
     * it on again.  The channel does not start playing until it is
     * triggered, but the DAC gate is live from the write.
     */
    case 0xFF12: /* NR12 -- channel 1 envelope */
        apu->nr12 = value;
        apu->ch1_on = (value & 0xF8) != 0;
        load_envelope_square(apu, 1, value);
        break;

    case 0xFF17: /* NR22 -- channel 2 envelope */
        apu->nr22 = value;
        apu->ch2_on = (value & 0xF8) != 0;
        load_envelope_square(apu, 2, value);
        break;

    case 0xFF21: /* NR42 -- noise envelope */
        apu->nr42 = value;
        apu->ch4_on = (value & 0xF8) != 0;
        load_envelope_noise(apu, value);
        break;

    case 0xFF23: /* NR44 -- noise length and trigger */
        apu->nr44 = (uint8_t)(value & 0x47);
        apu->ch4_len_enable = (value & 0x40) != 0;
        if (value & 0x80)
            trigger_noise(apu);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void apu_set_sample_rate(APU *apu, int32_t rate)
{
    if (rate <= 0)
        rate = 44100;
    apu->rate = rate;
    apu->period = ((uint64_t)GB_CLOCK_HZ << 16) / (uint64_t)rate;
    if (apu->period == 0)
        apu->period = 1;
    /* Decay factor of the output capacitor at this sample rate. */
    apu->hp_coeff = exp(-2.0 * 3.14159265358979 * APU_CAP_HZ / (double)rate);
}

void apu_reset(GB *gb)
{
    APU *apu = &gb->apu;

    if (apu->rate == 0)
        apu_set_sample_rate(apu, 44100);

    apu->powered = true;
    apu->nr50 = 0x77;
    apu->nr51 = 0xF3;
    /* $F1: powered, with the first square channel's DAC enabled.  This is
     * the value the DMG boot ROM leaves behind, so the channel status bits
     * of NR52 have to match it rather than being cleared. */
    apu->nr52 = 0xF1;
    apu->nr10 = 0x80;
    apu->nr11 = 0xBF;
    apu->nr12 = 0xF3;
    apu->nr13 = 0xFF;
    apu->nr14 = 0xBF;
    apu->nr21 = 0x3F;
    apu->nr22 = 0x00;
    apu->nr23 = 0xFF;
    apu->nr24 = 0xBF;
    apu->nr30 = 0x7F;
    apu->nr31 = 0xFF;
    apu->nr32 = 0x9F;
    apu->nr33 = 0xFF;
    apu->nr34 = 0xBF;
    apu->nr41 = 0xFF;
    apu->nr42 = 0x00;
    apu->nr43 = 0x00;
    apu->nr44 = 0xBF;
    apu->fs_step = 0;
    apu->fs_timer = 0;
    apu->cap_left = 0.0;
    apu->cap_right = 0.0;
    apu->acc = 0;
    apu->dropped = 0;
    apu->ring_read = 0;
    apu->ring_write = 0;
    memset(apu->ring, 0, sizeof(apu->ring));

    /* NR12 is $F3 post-boot, so channel 1's DAC is on and NR52 reads $F1.
     * Give the channel the matching envelope and length state. */
    apu->ch1_on = true;
    apu->ch2_on = apu->ch3_on = apu->ch4_on = false;
    apu->ch1_len = apu->ch2_len = 0;
    apu->ch3_len = 0;
    apu->ch4_len = 0;
    apu->ch1_len_enable = apu->ch2_len_enable = false;
    apu->ch3_len_enable = apu->ch4_len_enable = false;
    apu->ch1_vol = apu->ch2_vol = apu->ch4_vol = 0;
    apu->ch1_duty_pos = apu->ch2_duty_pos = 0;
    apu->ch1_timer = apu->ch2_timer = apu->ch3_timer = apu->ch4_timer = 0;
    apu->ch4_lfsr = 0x7FFF;
    apu->ch3_dac = false;
    apu->ch3_pos = 0;
    apu->ch1_shadow = 0;

    update_status(apu);

    apu->ch1_freq = (uint16_t)(apu->nr13 | ((apu->nr14 & 0x07) << 8));
    apu->ch2_freq = (uint16_t)(apu->nr23 | ((apu->nr24 & 0x07) << 8));
    apu->ch3_freq = (uint16_t)(apu->nr31 | ((apu->nr34 & 0x07) << 8));
    memcpy(apu->ch3_wave, apu->wave, 16);
}

/* ------------------------------------------------------------------ */
/* Main tick                                                           */
/* ------------------------------------------------------------------ */

static void clock_squares(APU *apu, int cycles)
{
    for (int ch = 0; ch < 2; ch++) {
        uint16_t freq = ch ? apu->ch2_freq : apu->ch1_freq;
        uint32_t period = (uint32_t)(2048 - freq) * 4u;
        uint32_t *timer = ch ? &apu->ch2_timer : &apu->ch1_timer;
        uint8_t *pos = ch ? &apu->ch2_duty_pos : &apu->ch1_duty_pos;

        *timer += (uint32_t)cycles;
        while (*timer >= period) {
            *timer -= period;
            *pos = (uint8_t)((*pos + 1) & 7);
        }
    }
}

static void clock_wave(APU *apu, int cycles)
{
    uint32_t period = (uint32_t)(2048 - apu->ch3_freq) * 2u;
    uint32_t *timer = &apu->ch3_timer;

    *timer += (uint32_t)cycles;
    while (*timer >= period) {
        *timer -= period;
        apu->ch3_pos = (uint8_t)((apu->ch3_pos + 1) & 31);
    }
}

static void clock_noise(APU *apu, int cycles)
{
    uint8_t divisor = (uint8_t)(apu->nr43 & 0x07);
    uint8_t shift = (uint8_t)((apu->nr43 >> 4) & 0x0F);
    uint32_t period;

    if (divisor == 0)
        period = 8u << shift;
    else
        period = (uint16_t)(16u << divisor) << shift;

    apu->ch4_timer += (uint32_t)cycles;
    while (apu->ch4_timer >= period) {
        uint16_t lfsr = apu->ch4_lfsr;
        uint16_t feedback;

        apu->ch4_timer -= period;
        feedback = (uint16_t)((lfsr ^ (lfsr >> 1)) & 1);
        lfsr = (uint16_t)((lfsr >> 1) | (feedback << 14));
        if (apu->nr43 & 0x08)
            lfsr = (uint16_t)((lfsr & ~0x40u) | (feedback << 6));
        apu->ch4_lfsr = lfsr;
    }
}

/* One batch of time, short enough that the channels never advance further
 * than a single sample period. */
static void apu_tick_chunk(GB *gb, int cycles)
{
    APU *apu = &gb->apu;

    /* Frequency registers alias NR13/NR14 and friends. */
    apu->ch1_freq = (uint16_t)(apu->nr13 | ((apu->nr14 & 0x07) << 8));
    apu->ch2_freq = (uint16_t)(apu->nr23 | ((apu->nr24 & 0x07) << 8));
    apu->ch3_freq = (uint16_t)(apu->nr31 | ((apu->nr34 & 0x07) << 8));

    /* Frame sequencer runs at 8192 Hz. */
    apu->fs_timer += (uint32_t)cycles;
    while (apu->fs_timer >= FS_PERIOD) {
        apu->fs_timer -= FS_PERIOD;
        if (apu->powered)
            frame_sequencer_step(gb);
    }

    if (!apu->powered)
        return;

    clock_squares(apu, cycles);
    clock_wave(apu, cycles);
    clock_noise(apu, cycles);

    /* Emit samples on a fixed point accumulator. */
    apu->acc += (uint64_t)cycles << 16;
    while (apu->acc >= apu->period) {
        int16_t frame[2];
        apu->acc -= apu->period;
        mix_sample(gb, frame);
        ring_push(apu, frame[0], frame[1]);
    }

    update_status(apu);
}

void apu_tick(GB *gb, int cycles)
{
    APU *apu = &gb->apu;
    int max_chunk;

    if (cycles <= 0)
        return;

    /* A single call often covers many sample periods, and the channels must
     * not run ahead of the mixer or the whole batch would be mixed with the
     * final waveform position.  Chop the work into sample sized pieces. */
    max_chunk = (int)(apu->period >> 16) + 1;
    if (max_chunk < 1)
        max_chunk = 1;

    while (cycles > 0) {
        int chunk = cycles < max_chunk ? cycles : max_chunk;
        apu_tick_chunk(gb, chunk);
        cycles -= chunk;
    }
}

/* ------------------------------------------------------------------ */
/* Frontend interface                                                  */
/* ------------------------------------------------------------------ */

size_t apu_drain(APU *apu, int16_t *dst, size_t count)
{
    size_t written = 0;

    while (written < count) {
        if (apu->ring_read == apu->ring_write) {
            /* Underrun: hand the caller silence for the rest. */
            memset(dst + written * 2, 0, (count - written) * 2 * sizeof(int16_t));
            break;
        }
        dst[written * 2 + 0] = apu->ring[apu->ring_read * 2 + 0];
        dst[written * 2 + 1] = apu->ring[apu->ring_read * 2 + 1];
        apu->ring_read = (apu->ring_read + 1) % APU_RING_SIZE;
        written++;
    }
    return written;
}

size_t apu_available(const APU *apu)
{
    return (size_t)((apu->ring_write + APU_RING_SIZE - apu->ring_read) %
                    APU_RING_SIZE);
}