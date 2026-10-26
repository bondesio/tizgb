/**
 * Game Boy APU emulator.
 * Copyright (c) 2019 Mahyar Koshkouei <mk@deltabeard.com>
 * Copyright (c) 2017 Alex Baines <alex@abaines.me.uk>
 * minigb_apu is released under the terms of the MIT license.
 *
 * minigb_apu emulates the audio processing unit (APU) of the Game Boy. This
 * project is based on MiniGBS by Alex Baines: https://github.com/baines/MiniGBS
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "minigb_apu.h"

#define DMG_CLOCK_FREQ_U	((unsigned)DMG_CLOCK_FREQ)
#define AUDIO_NSAMPLES		(AUDIO_SAMPLES_TOTAL)

#define MAX(a, b)		( a > b ? a : b )
#define MIN(a, b)		( a <= b ? a : b )

/* Factor in which values are multiplied to compensate for fixed-point
 * arithmetic. Some hard-coded values in this project must be recreated. */
#ifndef FREQ_INC_MULT
# define FREQ_INC_MULT		105
#endif
/* Handles time keeping for sound generation.
 * FREQ_INC_REF must be equal to, or larger than AUDIO_SAMPLE_RATE in order
 * to avoid a division by zero error.
 * Using a square of 2 simplifies calculations. */
#define FREQ_INC_REF		(AUDIO_SAMPLE_RATE * FREQ_INC_MULT)

#define MAX_CHAN_VOLUME		15

static void set_note_freq(struct chan *c)
{
	/* Lowest expected value of freq is 64. */
	uint32_t freq = (DMG_CLOCK_FREQ_U / 4) / (2048 - c->freq);
	c->freq_inc = freq * (uint32_t)(FREQ_INC_REF / AUDIO_SAMPLE_RATE);
}

static void chan_enable(struct minigb_apu_ctx *ctx,
		const uint_fast8_t i, const bool enable)
{
	uint8_t val;

	ctx->chans[i].enabled = enable;
	val = (ctx->audio_mem[0xFF26 - AUDIO_ADDR_COMPENSATION] & 0x80) |
		(ctx->chans[3].enabled << 3) | (ctx->chans[2].enabled << 2) |
		(ctx->chans[1].enabled << 1) | (ctx->chans[0].enabled << 0);

	ctx->audio_mem[0xFF26 - AUDIO_ADDR_COMPENSATION] = val;
}

static void update_env(struct chan *c)
{
	c->env.counter += c->env.inc;

	while (c->env.counter > FREQ_INC_REF) {
		if (c->env.step) {
			c->volume += c->env.up ? 1 : -1;
			if (c->volume == 0 || c->volume == MAX_CHAN_VOLUME) {
				c->env.inc = 0;
			}
			c->volume = MAX(0, MIN(MAX_CHAN_VOLUME, c->volume));
		}
		c->env.counter -= FREQ_INC_REF;
	}
}

static void update_len(struct minigb_apu_ctx *ctx, struct chan *c)
{
	if (!c->len.enabled)
		return;

	c->len.counter += c->len.inc;
	if (c->len.counter > FREQ_INC_REF) {
		chan_enable(ctx, c - ctx->chans, 0);
		c->len.counter = 0;
	}
}

static bool update_freq(struct chan *c, uint32_t *pos)
{
	uint32_t inc = c->freq_inc - *pos;
	c->freq_counter += inc;

	if (c->freq_counter > FREQ_INC_REF) {
		*pos		= c->freq_inc - (c->freq_counter - FREQ_INC_REF);
		c->freq_counter = 0;
		return true;
	} else {
		*pos = c->freq_inc;
		return false;
	}
}

/* TizGB: next sweep frequency, as the hardware calculates it. */
static uint32_t sweep_calc(const struct chan *c)
{
	uint32_t delta = (uint32_t)c->sweep.freq >> c->sweep.shift;
	if (c->sweep.down)
		return (uint32_t)c->sweep.freq - delta;   /* never below 0 */
	return (uint32_t)c->sweep.freq + delta;
}

/* TizGB: the original disabled the channel on every sweep tick whenever the
 * shift was 0, and ticked once immediately at trigger. The hardware instead
 * calculates freq + (freq >> shift) on each tick (pace != 0), disables the
 * channel only if that exceeds 2047, writes it back only when shift != 0,
 * then repeats the overflow check with the new value. */
static void update_sweep(struct chan *c)
{
	if (!c->sweep.inc)
		return;
	c->sweep.counter += c->sweep.inc;

	while (c->sweep.counter > FREQ_INC_REF) {
		uint32_t next = sweep_calc(c);
		if (next > 2047) {
			c->enabled = 0;
		} else if (c->sweep.shift) {
			c->sweep.freq = (uint16_t)next;
			c->freq = (uint16_t)next;
			set_note_freq(c);
			if (sweep_calc(c) > 2047)
				c->enabled = 0;
		}
		c->sweep.counter -= FREQ_INC_REF;
	}
}

/* TizGB: weight of the part of an output sample between two phase steps.
 * The original computed ((pos - prev_pos) / freq_inc) in integers, which is
 * always 0, so each output sample was the waveform's value at one instant
 * (point sampling, strong aliasing on high notes). This box-filters it. */
static int32_t seg(uint32_t from, uint32_t to, uint32_t total, int32_t val)
{
	return (int32_t)(((int64_t)(to - from) * val) / (int64_t)total);
}

static void update_square(struct minigb_apu_ctx *ctx, audio_sample_t *samples,
		const bool ch2, const unsigned nframes)
{
	struct chan *c = &ctx->chans[ch2];

	if (!c->powered || !c->enabled)
		return;

	set_note_freq(c);

	for (unsigned i = 0; i < nframes * 2; i += 2) {
		update_len(ctx, c);
		if (!c->enabled)
			return;

		update_env(c);
		if (!ch2) {
			update_sweep(c);
			if (!c->enabled)
				return;
		}
		if (!c->volume)
			continue;

		uint32_t pos = 0;
		uint32_t prev_pos = 0;
		int32_t sample = 0;

		while (update_freq(c, &pos)) {
			c->square.duty_counter = (c->square.duty_counter + 1) & 7;
			sample += seg(prev_pos, pos, c->freq_inc, c->val);
			c->val = (c->square.duty & (1 << c->square.duty_counter)) ?
				VOL_INIT_MAX / MAX_CHAN_VOLUME :
				VOL_INIT_MIN / MAX_CHAN_VOLUME;
			prev_pos = pos;
		}

		sample += seg(prev_pos, c->freq_inc, c->freq_inc, c->val);
		sample *= c->volume;
		sample /= 4;

		samples[i + 0] += sample * c->on_left * ctx->vol_l;
		samples[i + 1] += sample * c->on_right * ctx->vol_r;
	}
}

static uint8_t wave_sample(struct minigb_apu_ctx *ctx,
		const unsigned int pos, const unsigned int volume)
{
	uint8_t sample;

	sample =  ctx->audio_mem[(0xFF30 + pos / 2) - AUDIO_ADDR_COMPENSATION];
	if (pos & 1) {
		sample &= 0xF;
	} else {
		sample >>= 4;
	}
	return volume ? (sample >> (volume - 1)) : 0;
}

static void update_wave(struct minigb_apu_ctx *ctx, audio_sample_t *samples,
		const unsigned nframes)
{
	struct chan *c = &ctx->chans[2];

	if (!c->powered || !c->enabled || !c->volume)
		return;

	set_note_freq(c);
	c->freq_inc *= 2;

	for (unsigned i = 0; i < nframes * 2; i += 2) {
		update_len(ctx, c);
		if (!c->enabled)
			return;

		uint32_t pos = 0;
		uint32_t prev_pos = 0;
		int32_t sample = 0;

		c->wave.sample = wave_sample(ctx, c->val, c->volume);

		while (update_freq(c, &pos)) {
			c->val = (c->val + 1) & 31;
			sample += seg(prev_pos, pos, c->freq_inc,
				((int32_t)c->wave.sample - 8) * (AUDIO_SAMPLE_MAX/64));
			c->wave.sample = wave_sample(ctx, c->val, c->volume);
			prev_pos  = pos;
		}

		sample += seg(prev_pos, c->freq_inc, c->freq_inc,
			((int32_t)c->wave.sample - 8) * (AUDIO_SAMPLE_MAX/64));
		/* TizGB: wave_sample() already applies the 100/50/25 % output
		 * level as a right shift; the original divided by 1/2/4 again,
		 * making 50 % sound at 25 % and 25 % at 6 %. */

		sample /= 4;
		samples[i + 0] += sample * c->on_left * ctx->vol_l;
		samples[i + 1] += sample * c->on_right * ctx->vol_r;
	}
}

static void update_noise(struct minigb_apu_ctx *ctx, audio_sample_t *samples,
		const unsigned nframes)
{
	struct chan *c = &ctx->chans[3];
	/* TizGB: with clock shift 14 or 15 the hardware LFSR receives no
	 * clocks: the channel keeps its current output (and envelope/length)
	 * rather than being switched off. */
	const bool frozen = c->freq >= 14;

	if (!c->powered || !c->enabled)
		return;

	{
		const uint32_t lfsr_div_lut[] = {
			8, 16, 32, 48, 64, 80, 96, 112
		};
		uint32_t freq;

		freq = frozen ? 1 : DMG_CLOCK_FREQ_U / (lfsr_div_lut[c->noise.lfsr_div] << c->freq);
		c->freq_inc = freq * (uint32_t)(FREQ_INC_REF / AUDIO_SAMPLE_RATE);
	}

	for (unsigned i = 0; i < nframes * 2; i += 2) {
		update_len(ctx, c);
		if (!c->enabled)
			return;

		update_env(c);
		if (!c->volume)
			continue;

		uint32_t pos      = 0;
		uint32_t prev_pos = 0;
		int32_t sample    = 0;

		while (!frozen && update_freq(c, &pos)) {
			/* Segment before this LFSR step keeps the old output. */
			sample += seg(prev_pos, pos, c->freq_inc, c->val);
			c->noise.lfsr_reg = (c->noise.lfsr_reg << 1) |
				(c->val >= VOL_INIT_MAX/MAX_CHAN_VOLUME);

			if (c->noise.lfsr_wide) {
				c->val = !(((c->noise.lfsr_reg >> 14) & 1) ^
						((c->noise.lfsr_reg >> 13) & 1)) ?
					VOL_INIT_MAX / MAX_CHAN_VOLUME :
					VOL_INIT_MIN / MAX_CHAN_VOLUME;
			} else {
				c->val = !(((c->noise.lfsr_reg >> 6) & 1) ^
						((c->noise.lfsr_reg >> 5) & 1)) ?
					VOL_INIT_MAX / MAX_CHAN_VOLUME :
					VOL_INIT_MIN / MAX_CHAN_VOLUME;
			}

			prev_pos = pos;
		}

		sample += frozen ? c->val : seg(prev_pos, c->freq_inc, c->freq_inc, c->val);
		sample *= c->volume;
		sample /= 4;

		samples[i + 0] += sample * c->on_left * ctx->vol_l;
		samples[i + 1] += sample * c->on_right * ctx->vol_r;
	}
}

/**
 * SDL2 style audio callback function.
 */
void minigb_apu_render(struct minigb_apu_ctx *ctx,
		audio_sample_t *stream, unsigned nframes)
{
	/* TizGB: a channel in mute_mask is still emulated (its length,
	 * envelope and status bits keep running) but rendered into scratch. */
	static audio_sample_t scratch[2048 * 2];
	audio_sample_t *dst[4];
	memset(stream, 0, nframes * 2 * sizeof(audio_sample_t));
	if (!nframes)
		return;
	for (int c = 0; c < 4; c++)
		dst[c] = ((ctx->mute_mask >> c) & 1) && nframes <= 2048 ? scratch : stream;
	if (ctx->mute_mask && nframes <= 2048)
		memset(scratch, 0, nframes * 2 * sizeof(audio_sample_t));
	update_square(ctx, dst[0], 0, nframes);
	update_square(ctx, dst[1], 1, nframes);
	update_wave(ctx, dst[2], nframes);
	update_noise(ctx, dst[3], nframes);
}

void minigb_apu_audio_callback(struct minigb_apu_ctx *ctx,
		audio_sample_t *stream)
{
	minigb_apu_render(ctx, stream, AUDIO_SAMPLES);
}

static void chan_trigger(struct minigb_apu_ctx *ctx, uint_fast8_t i)
{
	struct chan *c = &ctx->chans[i];

	chan_enable(ctx, i, 1);
	c->volume = c->volume_init;

	// volume envelope
	{
		/* LUT created in Julia with:
		 * `(FREQ_INC_MULT * 64)./vcat(8, 1:7)`
		 * Must be recreated when FREQ_INC_MULT modified.
		 */
		const uint32_t inc_lut[8] = {
#if FREQ_INC_MULT == 16
			128, 1024, 512, 341,
			256,  205, 171, 146
#elif FREQ_INC_MULT == 64
            512,  4096, 2048, 1365,
            1024,  819,  683,  585
#elif FREQ_INC_MULT == 105
            /* Multiples of 105 provide integer values. */
            840,  6720, 3360, 2240,
            1680, 1344, 1120,  960
#else
#error "LUT not calculated for this value of FREQ_INC_MULT"
#endif
		};
		uint8_t val;

		val = ctx->audio_mem[(0xFF12 + (i * 5)) - AUDIO_ADDR_COMPENSATION];

		c->env.step = val & 0x7;
		c->env.up   = val & 0x8;
		c->env.inc  = inc_lut[c->env.step];
		c->env.counter = 0;
	}

	// freq sweep
	if (i == 0) {
		uint8_t val = ctx->audio_mem[0xFF10 - AUDIO_ADDR_COMPENSATION];

		c->sweep.freq  = c->freq;
		c->sweep.rate  = (val >> 4) & 0x07;
		c->sweep.down  = (val & 0x08);
		c->sweep.shift = (val & 0x07);
		c->sweep.inc   = c->sweep.rate ?
			((128u * FREQ_INC_REF) / (c->sweep.rate * AUDIO_SAMPLE_RATE)) : 0;
		c->sweep.counter = 0;
		if (c->sweep.shift && sweep_calc(c) > 2047)
			chan_enable(ctx, i, 0);
	}

	int len_max = 64;

	if (i == 2) { // wave
		len_max = 256;
		c->val = 0;
	} else if (i == 3) { // noise
		c->noise.lfsr_reg = 0xFFFF;
		c->val = VOL_INIT_MIN / MAX_CHAN_VOLUME;
	}

	c->len.inc = (256u * FREQ_INC_REF) / (AUDIO_SAMPLE_RATE * (len_max - c->len.load));
	c->len.counter = 0;
}

/**
 * Read audio register.
 * \param addr	Address of audio register. Must be 0xFF10 <= addr <= 0xFF3F.
 *				This is not checked in this function.
 * \return	Byte at address.
 */
uint8_t minigb_apu_audio_read(struct minigb_apu_ctx *ctx, const uint16_t addr)
{
	static const uint8_t ortab[] = {
		0x80, 0x3f, 0x00, 0xff, 0xbf,
		0xff, 0x3f, 0x00, 0xff, 0xbf,
		0x7f, 0xff, 0x9f, 0xff, 0xbf,
		0xff, 0xff, 0x00, 0x00, 0xbf,
		0x00, 0x00, 0x70,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	return ctx->audio_mem[addr - AUDIO_ADDR_COMPENSATION] |
		ortab[addr - AUDIO_ADDR_COMPENSATION];
}

/**
 * Write audio register.
 * \param addr	Address of audio register. Must be 0xFF10 <= addr <= 0xFF3F.
 *				This is not checked in this function.
 * \param val	Byte to write at address.
 */
void minigb_apu_audio_write(struct minigb_apu_ctx *ctx,
		const uint16_t addr, const uint8_t val)
{
	/* Find sound channel corresponding to register address. */
	uint_fast8_t i;

	if(addr == 0xFF26)
	{
		ctx->audio_mem[addr - AUDIO_ADDR_COMPENSATION] = val & 0x80;
		/* On APU power off, clear all registers apart from wave
		 * RAM. */
		if((val & 0x80) == 0)
		{
			memset(ctx->audio_mem,
					0x00, 0xFF26 - AUDIO_ADDR_COMPENSATION);
			ctx->chans[0].enabled = false;
			ctx->chans[1].enabled = false;
			ctx->chans[2].enabled = false;
			ctx->chans[3].enabled = false;
		}

		return;
	}

	/* Ignore register writes if APU powered off (wave RAM excepted, as on
	 * hardware - TizGB). */
	if(ctx->audio_mem[0xFF26 - AUDIO_ADDR_COMPENSATION] == 0x00 &&
			addr < 0xFF30)
		return;

	ctx->audio_mem[addr - AUDIO_ADDR_COMPENSATION] = val;
	i = (addr - AUDIO_ADDR_COMPENSATION) / 5;

	switch (addr) {
	case 0xFF12:
	case 0xFF17:
	case 0xFF21: {
		ctx->chans[i].volume_init = val >> 4;
		ctx->chans[i].powered     = (val >> 3) != 0;

		// "zombie mode" stuff, needed for Prehistorik Man and probably
		// others
		if (ctx->chans[i].powered && ctx->chans[i].enabled) {
			if ((ctx->chans[i].env.step == 0 && ctx->chans[i].env.inc != 0)) {
				if (val & 0x08) {
					ctx->chans[i].volume++;
				} else {
					ctx->chans[i].volume += 2;
				}
			} else {
				ctx->chans[i].volume = 16 - ctx->chans[i].volume;
			}

			ctx->chans[i].volume &= 0x0F;
			ctx->chans[i].env.step = val & 0x07;
		}
	} break;

	case 0xFF1C:
		ctx->chans[i].volume = ctx->chans[i].volume_init = (val >> 5) & 0x03;
		break;

	case 0xFF11:
	case 0xFF16:
	case 0xFF20: {
		const uint8_t duty_lookup[] = { 0x10, 0x30, 0x3C, 0xCF };
		ctx->chans[i].len.load = val & 0x3f;
		ctx->chans[i].square.duty = duty_lookup[val >> 6];
		break;
	}

	case 0xFF1B:
		ctx->chans[i].len.load = val;
		break;

	case 0xFF13:
	case 0xFF18:
	case 0xFF1D:
		ctx->chans[i].freq &= 0xFF00;
		ctx->chans[i].freq |= val;
		break;

	case 0xFF1A:
		ctx->chans[i].powered = (val & 0x80) != 0;
		chan_enable(ctx, i, val & 0x80);
		break;

	case 0xFF14:
	case 0xFF19:
	case 0xFF1E:
		ctx->chans[i].freq &= 0x00FF;
		ctx->chans[i].freq |= ((val & 0x07) << 8);
		/* Intentional fall-through. */
	case 0xFF23:
		ctx->chans[i].len.enabled = val & 0x40;
		if (val & 0x80)
			chan_trigger(ctx, i);

		break;

	case 0xFF22:
		ctx->chans[3].freq = val >> 4;
		ctx->chans[3].noise.lfsr_wide = !(val & 0x08);
		ctx->chans[3].noise.lfsr_div = val & 0x07;
		break;

	case 0xFF24:
	{
		ctx->vol_l = ((val >> 4) & 0x07);
		ctx->vol_r = (val & 0x07);
		break;
	}

	case 0xFF25:
		for (uint_fast8_t j = 0; j < 4; j++) {
			ctx->chans[j].on_left  = (val >> (4 + j)) & 1;
			ctx->chans[j].on_right = (val >> j) & 1;
		}
		break;
	}
}

void minigb_apu_audio_init(struct minigb_apu_ctx *ctx)
{
	/* Initialise channels and samples. */
	memset(ctx->chans, 0, sizeof(ctx->chans));
	ctx->chans[0].val = ctx->chans[1].val = -1;

	/* Initialise IO registers. */
	{
		const uint8_t regs_init[] = { 0x80, 0xBF, 0xF3, 0xFF, 0x3F,
					      0xFF, 0x3F, 0x00, 0xFF, 0x3F,
					      0x7F, 0xFF, 0x9F, 0xFF, 0x3F,
					      0xFF, 0xFF, 0x00, 0x00, 0x3F,
					      0x77, 0xF3, 0xF1 };

		for(uint_fast8_t i = 0; i < sizeof(regs_init); ++i)
			minigb_apu_audio_write(ctx, 0xFF10 + i, regs_init[i]);
	}

	/* Initialise Wave Pattern RAM. */
	{
		const uint8_t wave_init[] = { 0xac, 0xdd, 0xda, 0x48,
					      0x36, 0x02, 0xcf, 0x16,
					      0x2c, 0x04, 0xe5, 0x2c,
					      0xac, 0xdd, 0xda, 0x48 };

		for(uint_fast8_t i = 0; i < sizeof(wave_init); ++i)
			minigb_apu_audio_write(ctx, 0xFF30 + i, wave_init[i]);
	}
}
