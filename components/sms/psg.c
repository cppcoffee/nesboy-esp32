/* SN76489 PSG: 3 tone channels + 1 noise channel.
 *
 * Sample generation is driven by psg_tick from the frame loop; the internal
 * phase accumulators run at the Z80 clock rate and output at the requested
 * sample rate.
 */
#include <string.h>

#include "sms_internal.h"
#include "psg.h"

#define PSG_CLOCK 3579545 /* NTSC SMS master clock / 16 for PSG; we use the divided rate */

struct psg_channel {
    uint16_t tone;  /* 10-bit tone period */
    uint8_t volume; /* 4-bit attenuation */
    uint32_t phase; /* 0..tone*16 counter in master-clock units */
    int8_t output;  /* current square level */
};

struct psg_state {
    struct psg_channel tone[3];
    struct psg_channel noise;
    uint8_t latch; /* latched channel/type */
    uint8_t noise_shift;
    uint16_t noise_tap;
    int noise_mode; /* 0=periodic, 1=white */
    uint32_t noise_period;
    uint32_t noise_counter;

    /* sample-rate conversion */
    uint64_t sample_accum;
    uint64_t cycles_per_sample_q16;
    int16_t *buffer;
    size_t capacity;
    size_t pos;
};

static struct psg_state ps;

static const uint8_t volume_table[16] = {
    255, 203, 161, 128, 102, 81, 64, 51, 40, 32, 25, 20, 16, 13, 10, 0,
};

static const uint16_t noise_periods[4] = {0x100 >> 6, 0x100 >> 7, 0x100 >> 8, 0};

void psg_reset(void)
{
    memset(&ps, 0, sizeof(ps));
    ps.noise_shift = 0x0F;
    ps.noise_tap = 0x0009; /* white: XOR bits 0 and 3 */
    ps.cycles_per_sample_q16 = ((uint64_t)PSG_CLOCK << 16) / (uint64_t)sms.sample_rate;
}

void psg_write(uint8_t value)
{
    int channel = value & 0x03;
    bool is_volume = (value & 0x10) != 0;
    struct psg_channel *ch = channel < 3 ? &ps.tone[channel] : &ps.noise;

    if (is_volume) {
        ch->volume = value & 0x0F;
        return;
    }

    if (value & 0x80) {
        /* latch data */
        ps.latch = value & 0x70;
        if (channel < 3) {
            ch->tone = (uint16_t)((ch->tone & ~0x0F) | (value & 0x0F));
        } else {
            ps.noise_mode = value & 0x04;
            int period_sel = value & 0x03;
            ps.noise_period = (period_sel == 3) ? ps.tone[2].tone * 16 : noise_periods[period_sel] << 6;
        }
        return;
    }

    /* data byte */
    switch (ps.latch) {
    case 0x00: /* tone 0 low */
    case 0x10:
    case 0x20:
        ch->tone = (uint16_t)((ch->tone & 0x3F0) | (value & 0x0F));
        break;
    case 0x30: /* noise control */
        ps.noise_mode = value & 0x04;
        break;
    default: /* tone high bits */
        ch->tone = (uint16_t)((ch->tone & 0x00F) | ((uint16_t)(value & 0x3F) << 4));
        break;
    }
}

static int16_t sample_channel(const struct psg_channel *ch)
{
    return ch->output ? (int16_t)volume_table[ch->volume] : (int16_t)-volume_table[ch->volume];
}

static void advance_channel(struct psg_channel *ch, int cycles)
{
    uint32_t period = (uint32_t)ch->tone * 16;
    if (period == 0) {
        period = 1;
    }
    ch->phase += (uint32_t)cycles;
    while (ch->phase >= period) {
        ch->phase -= period;
        ch->output = !ch->output;
    }
}

static void advance_noise(int cycles)
{
    uint32_t period = ps.noise_period ? ps.noise_period * 16 : (uint32_t)ps.tone[2].tone * 16;
    if (period == 0) {
        period = 16;
    }
    ps.noise_counter += (uint32_t)cycles;
    while (ps.noise_counter >= period) {
        ps.noise_counter -= period;
        int feedback;
        if (ps.noise_mode) {
            feedback = ((ps.noise_shift ^ (ps.noise_shift >> 3)) & 1);
        } else {
            feedback = (ps.noise_shift & 1);
        }
        ps.noise_shift = (uint8_t)((ps.noise_shift >> 1) | (feedback << 7));
        ps.noise.output = ps.noise_shift & 1;
    }
}

void psg_tick(int cycles)
{
    for (int i = 0; i < 3; i++) {
        advance_channel(&ps.tone[i], cycles);
    }
    advance_noise(cycles);

    /* sample-rate conversion */
    ps.sample_accum += (uint64_t)cycles << 16;
    while (ps.sample_accum >= ps.cycles_per_sample_q16) {
        ps.sample_accum -= ps.cycles_per_sample_q16;

        if (!ps.buffer || ps.pos + 2 > ps.capacity) {
            continue;
        }
        int l = sample_channel(&ps.tone[0]) + sample_channel(&ps.tone[1]) + sample_channel(&ps.tone[2]) +
                sample_channel(&ps.noise);
        int r = l; /* mono source; the amp mixes L/2+R/2 anyway */
        ps.buffer[ps.pos++] = (int16_t)(l / 4);
        ps.buffer[ps.pos++] = (int16_t)(r / 4);
    }
}

/* expose the buffer pointer to sms.c via internal state */
void psg_set_buffer(int16_t *buffer, size_t capacity)
{
    ps.buffer = buffer;
    ps.capacity = capacity;
    ps.pos = 0;
}

size_t psg_samples_ready(void)
{
    return ps.pos;
}

const int16_t *psg_buffer(void)
{
    return ps.buffer;
}

/* Serialized PSG state for save states. */
size_t psg_state_size(void)
{
    return sizeof(ps);
}

void psg_state_save(void *out)
{
    memcpy(out, &ps, sizeof(ps));
}

void psg_state_load(const void *in)
{
    int16_t *buffer = ps.buffer;
    size_t capacity = ps.capacity;
    memcpy(&ps, in, sizeof(ps));
    ps.buffer = buffer;
    ps.capacity = capacity;
    ps.pos = 0;
}
