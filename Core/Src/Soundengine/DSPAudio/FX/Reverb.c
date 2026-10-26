/*
 * Reverb.c
 *
 *  Created on: 30.07.2014
 *      Author: julian
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#include "Soundengine/DSPAudio/FX/Reverb.h"
#include <math.h>
#include <string.h>

/*
 * Buffer layout (__fp16 samples). Every line is a power-of-two ring that
 * shares the single write index r->w, so a read at "age" a is
 * buf[BASE + ((w - a) & MASK)].
 */
#define PRE_BASE	0u		// pre-delay + early reflection taps
#define PRE_MASK	4095u
#define IAP_BASE	4096u	// 4 input diffusers, 512 each
#define IAP_MASK	511u
#define APM0_BASE	6144u	// modulated tank allpasses, 1024 each
#define APM1_BASE	7168u
#define APM_MASK	1023u
#define DEL0_BASE	8192u	// tank delays and allpasses, 2048 each
#define AP0_BASE	10240u
#define DEL1_BASE	12288u
#define DEL2_BASE	14336u
#define AP1_BASE	16384u
#define DEL3_BASE	18432u
#define TANK_MASK	2047u
#if (DEL3_BASE + TANK_MASK + 1u) != REVERB_BUFFER_SAMPLES
#error "reverb buffer layout does not match REVERB_BUFFER_SAMPLES"
#endif

// delay lengths in tank ticks
#define IAP0_LEN	142u
#define IAP1_LEN	107u
#define IAP2_LEN	379u
#define IAP3_LEN	277u
#define APM0_LEN	672.f
#define APM1_LEN	908.f
#define DEL0_LEN	2047u
#define AP0_LEN		1800u
#define DEL1_LEN	1913u
#define DEL2_LEN	1720u
#define AP1_LEN		2039u
#define DEL3_LEN	1163u
#define LOOP_LEN	(672u + 2047u + 1800u + 1913u + 908u + 1720u + 2039u + 1163u)

#define MOD_DEPTH	8.f		// +- ticks
#define MOD_RATE	0.8f	// Hz
#define TWO_PI		6.28318530718f

#define IN_DIFF1	0.75f
#define IN_DIFF2	0.625f
#define DECAY_DIFF1	0.7f
#define DECAY_DIFF2	0.5f
#define TANK_LIMIT	4.f		// keeps the fp16 lines finite even if something goes wrong
#define TAP_GAIN	0.6f
#define WET_GAIN	(1.0f * 32767.f)
#define ECO_WET_GAIN	1.41f	// compensates the 2:1 input averaging
#define INPUT_GAIN	(1.f / 65536.f)	// (L+R) -> +-1

#define MAX_CHUNK	64u

static const uint16_t earlyAge[4] = {240, 480, 760, 1000};

#define RD(base, mask, age)		((float)b[(base) + ((w - (age)) & (mask))])
#define WR(base, mask, v)		(b[(base) + (w & (mask))] = (__fp16)(v))

typedef struct
{
	float tank1, tank2, damp1, damp2, toneState;
	float feedback, dampCoef, toneCoef, toneDry, toneLp, earlyGain;
	uint32_t preDelay;
} TankState;

static inline float clampf(float v, float lo, float hi)
{
	return fminf(fmaxf(v, lo), hi);
}

static inline float allpass(__fp16 *b, uint32_t w, uint32_t base, uint32_t mask, uint32_t len, float g, float x)
{
	const float d = RD(base, mask, len);
	const float u = x + g * d;
	WR(base, mask, u);
	return d - g * u;
}

static inline float allpassMod(__fp16 *b, uint32_t w, uint32_t base, float age, float x)
{
	const uint32_t i = (uint32_t)age;
	const float f = age - (float)i;
	const float a = RD(base, APM_MASK, i);
	const float c = RD(base, APM_MASK, i + 1u);
	const float d = a + f * (c - a);
	const float u = x - DECAY_DIFF1 * d;
	WR(base, APM_MASK, u);
	return d + DECAY_DIFF1 * u;
}

static inline __attribute__((always_inline)) void tick(TankState *s, __fp16 *b, uint32_t w, float x, float modA, float modB, float *outL, float *outR)
{
	// input tilt filter + pre-delay
	s->toneState += s->toneCoef * (x - s->toneState);
	WR(PRE_BASE, PRE_MASK, s->toneDry * x + s->toneLp * s->toneState);

	const uint32_t pd = s->preDelay;
	const float pre = RD(PRE_BASE, PRE_MASK, pd);
	const float early = s->earlyGain * (RD(PRE_BASE, PRE_MASK, pd + earlyAge[0])
			+ 0.5f * RD(PRE_BASE, PRE_MASK, pd + earlyAge[1])
			+ 0.125f * RD(PRE_BASE, PRE_MASK, pd + earlyAge[2])
			+ 0.25f * RD(PRE_BASE, PRE_MASK, pd + earlyAge[3]));

	// two parallel input diffuser pairs -> decorrelated feeds for the tank halves
	float in1 = allpass(b, w, IAP_BASE + 512u, IAP_MASK, IAP1_LEN, IN_DIFF1, pre);
	in1 = allpass(b, w, IAP_BASE, IAP_MASK, IAP0_LEN, IN_DIFF1, in1);
	float in2 = allpass(b, w, IAP_BASE + 1536u, IAP_MASK, IAP3_LEN, IN_DIFF2, pre);
	in2 = allpass(b, w, IAP_BASE + 1024u, IAP_MASK, IAP2_LEN, IN_DIFF2, in2);

	const float fb = s->feedback;

	// tank half 1
	float y = allpassMod(b, w, APM0_BASE, APM0_LEN + modA, in1 + s->tank2);
	WR(DEL0_BASE, TANK_MASK, y);
	s->damp1 += s->dampCoef * (RD(DEL0_BASE, TANK_MASK, DEL0_LEN) - s->damp1);
	y = allpass(b, w, AP0_BASE, TANK_MASK, AP0_LEN, DECAY_DIFF2, s->damp1 * fb);
	WR(DEL1_BASE, TANK_MASK, y);
	const float t1 = clampf(fb * RD(DEL1_BASE, TANK_MASK, DEL1_LEN), -TANK_LIMIT, TANK_LIMIT);

	// tank half 2
	y = allpassMod(b, w, APM1_BASE, APM1_LEN + modB, in2 + s->tank1);
	WR(DEL2_BASE, TANK_MASK, y);
	s->damp2 += s->dampCoef * (RD(DEL2_BASE, TANK_MASK, DEL2_LEN) - s->damp2);
	y = allpass(b, w, AP1_BASE, TANK_MASK, AP1_LEN, DECAY_DIFF2, s->damp2 * fb);
	WR(DEL3_BASE, TANK_MASK, y);
	const float t2 = clampf(fb * RD(DEL3_BASE, TANK_MASK, DEL3_LEN), -TANK_LIMIT, TANK_LIMIT);

	s->tank1 = t1;
	s->tank2 = t2;

	// output taps (Dattorro's tap set scaled to these line lengths)
	const float l = RD(DEL2_BASE, TANK_MASK, 108u) + RD(DEL2_BASE, TANK_MASK, 1213u)
			- RD(AP1_BASE, TANK_MASK, 1469u) + RD(DEL3_BASE, TANK_MASK, 734u)
			- RD(DEL0_BASE, TANK_MASK, 915u) - RD(AP0_BASE, TANK_MASK, 187u)
			- RD(DEL1_BASE, TANK_MASK, 548u);
	const float r = RD(DEL0_BASE, TANK_MASK, 162u) + RD(DEL0_BASE, TANK_MASK, 1667u)
			- RD(AP0_BASE, TANK_MASK, 1228u) + RD(DEL1_BASE, TANK_MASK, 1375u)
			- RD(DEL2_BASE, TANK_MASK, 861u) - RD(AP1_BASE, TANK_MASK, 257u)
			- RD(DEL3_BASE, TANK_MASK, 44u);

	*outL = TAP_GAIN * l + early;
	*outR = TAP_GAIN * r + early;
}

static inline float tickRate(const Reverb *r)
{
	return r->type == REVERB_ECO ? REVERB_FS * 0.5f : REVERB_FS;
}

static float onePoleCoef(float f, float fs)
{
	if(f > 0.45f * fs) f = 0.45f * fs;
	return 1.f - expf(-TWO_PI * f / fs);
}

static void updatePreDelay(Reverb *r)
{
	const float ms = 1.f + 45.f * r->param[0];
	uint32_t ticks = (uint32_t)(ms * 0.001f * tickRate(r));
	if(ticks < 1) ticks = 1;
	if(ticks > TANK_MASK) ticks = TANK_MASK;
	r->preDelay = ticks;
}

static void updateDecay(Reverb *r)
{
	const float rt60 = 0.3f * powf(64.f, r->param[1]);
	const float loopSeconds = (float)LOOP_LEN / tickRate(r);
	// 4 gain stages per loop: fb^4 per loop, -60 dB after rt60
	float fb = powf(10.f, -0.75f * loopSeconds / rt60);
	if(fb > 0.98f) fb = 0.98f;
	r->feedback = fb;
}

static void updateTone(Reverb *r)
{
	const float c = r->param[2];
	const float fs = tickRate(r);
	// output = dry*x + lp*lowpass(x): flat at c = 0.5
	if(c < 0.5f)
	{
		const float h = 2.f * c;		// high band gain 0..1
		r->toneCoef = onePoleCoef(1200.f, fs);
		r->toneDry = h;
		r->toneLp = 1.f - h;
	}
	else
	{
		const float l = 2.f - 2.f * c;	// low band gain 1..0
		r->toneCoef = onePoleCoef(500.f, fs);
		r->toneDry = 1.f;
		r->toneLp = l - 1.f;
	}
	r->dampCoef = onePoleCoef(800.f * powf(25.f, c), fs);
}

static void updateEarly(Reverb *r)
{
	r->earlyGain = 0.5f * r->param[3];
}

// newlib-nano's memset stores single bytes (~3 cycles/byte), too slow for a
// 40 KB clear in the audio thread. Keep GCC from turning this back into memset.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-loop-distribute-patterns")))
#endif
static void zeroWords(uint32_t *p, uint32_t words)
{
	while(words >= 4)
	{
		p[0] = 0;
		p[1] = 0;
		p[2] = 0;
		p[3] = 0;
		p += 4;
		words -= 4;
	}
	while(words--) *p++ = 0;
}

void Reverb_clear(Reverb *r)
{
	zeroWords((uint32_t*)(void*)r->buf, REVERB_BUFFER_SAMPLES * sizeof(__fp16) / 4u);
	r->tank1 = r->tank2 = 0.f;
	r->damp1 = r->damp2 = 0.f;
	r->toneState = 0.f;
	r->lastL = r->lastR = 0.f;
	r->clearPending = 0;
}

void Reverb_init(Reverb *r, __fp16 *buf)
{
	memset(r, 0, sizeof(*r));
	r->buf = buf;
	r->type = REVERB_OFF;
	r->param[0] = 0.f;
	r->param[1] = 0.5f;
	r->param[2] = 0.5f;
	r->param[3] = 0.f;
	updatePreDelay(r);
	updateDecay(r);
	updateTone(r);
	updateEarly(r);
	r->clearPending = 1;
}

void Reverb_setPreDelay(Reverb *r, float p)
{
	p = clampf(p, 0.f, 1.f);
	if(p == r->param[0]) return;
	r->param[0] = p;
	updatePreDelay(r);
}

void Reverb_setDecay(Reverb *r, float p)
{
	p = clampf(p, 0.f, 1.f);
	if(p == r->param[1]) return;
	r->param[1] = p;
	updateDecay(r);
}

void Reverb_setTone(Reverb *r, float p)
{
	p = clampf(p, 0.f, 1.f);
	if(p == r->param[2]) return;
	r->param[2] = p;
	updateTone(r);
}

void Reverb_setEarly(Reverb *r, float p)
{
	p = clampf(p, 0.f, 1.f);
	if(p == r->param[3]) return;
	r->param[3] = p;
	updateEarly(r);
}

void Reverb_setType(Reverb *r, uint8_t type)
{
	// kit bytes are not range-checked on load: unknown values must not switch
	// the reverb on
	if(type >= REVERB_NUM_TYPES) type = REVERB_OFF;
	if(type == r->type) return;
	r->type = type;
	updatePreDelay(r);
	updateDecay(r);
	updateTone(r);
	// line contents were written at the other rate
	r->clearPending = 1;
}

static inline int16_t sat16(int32_t v)
{
	if(v > 32767) v = 32767;
	else if(v < -32768) v = -32768;
	return (int16_t)v;
}

static inline void mixOut(int16_t *dst, int16_t dry, float wet, float dryWet)
{
	const float v = clampf((float)dry + (wet - (float)dry) * dryWet, -32768.f, 32767.f);
	*dst = sat16((int32_t)*dst + (int32_t)v);
}

static void processChunk(Reverb *r, const int16_t *in, int16_t *out, uint32_t frames, uint8_t channels, float dryWet)
{
	float wetL[MAX_CHUNK];
	float wetR[MAX_CHUNK];

	TankState s = {
		r->tank1, r->tank2, r->damp1, r->damp2, r->toneState,
		r->feedback, r->dampCoef, r->toneCoef, r->toneDry, r->toneLp, r->earlyGain,
		r->preDelay
	};
	__fp16 * const b = r->buf;
	uint32_t w = r->w;

	// block-rate LFO, linearly ramped across the chunk
	const uint32_t ticks = r->type == REVERB_ECO ? frames / 2u : frames;
	r->lfoPhase += TWO_PI * MOD_RATE * (float)frames / REVERB_FS;
	if(r->lfoPhase > TWO_PI) r->lfoPhase -= TWO_PI;
	const float targetA = MOD_DEPTH * sinf(r->lfoPhase);
	const float targetB = MOD_DEPTH * cosf(r->lfoPhase);
	const float stepA = (targetA - r->modA) / (float)ticks;
	const float stepB = (targetB - r->modB) / (float)ticks;
	float modA = r->modA;
	float modB = r->modB;

	if(r->type == REVERB_ECO)
	{
		float lastL = r->lastL;
		float lastR = r->lastR;
		for(uint32_t i = 0; i < frames; i += 2)
		{
			const float x = (float)((int32_t)in[2 * i] + in[2 * i + 1] + in[2 * i + 2] + in[2 * i + 3]) * (0.5f * INPUT_GAIN);
			float l, rr;
			modA += stepA;
			modB += stepB;
			tick(&s, b, w++, x, modA, modB, &l, &rr);
			wetL[i] = 0.5f * (lastL + l);
			wetR[i] = 0.5f * (lastR + rr);
			wetL[i + 1] = l;
			wetR[i + 1] = rr;
			lastL = l;
			lastR = rr;
		}
		r->lastL = lastL;
		r->lastR = lastR;
	}
	else
	{
		for(uint32_t i = 0; i < frames; i++)
		{
			const float x = (float)((int32_t)in[2 * i] + in[2 * i + 1]) * INPUT_GAIN;
			modA += stepA;
			modB += stepB;
			tick(&s, b, w++, x, modA, modB, &wetL[i], &wetR[i]);
		}
	}

	r->modA = targetA;
	r->modB = targetB;
	r->w = w;
	r->tank1 = s.tank1;
	r->tank2 = s.tank2;
	r->damp1 = s.damp1;
	r->damp2 = s.damp2;
	r->toneState = s.toneState;

	const float gain = r->type == REVERB_ECO ? WET_GAIN * ECO_WET_GAIN : WET_GAIN;
	if(channels == 2)
	{
		for(uint32_t i = 0; i < frames; i++)
		{
			mixOut(&out[2 * i], in[2 * i], wetL[i] * gain, dryWet);
			mixOut(&out[2 * i + 1], in[2 * i + 1], wetR[i] * gain, dryWet);
		}
	}
	else
	{
		for(uint32_t i = 0; i < frames; i++)
		{
			mixOut(&out[2 * i], in[2 * i], (wetL[i] + wetR[i]) * (0.5f * gain), dryWet);
		}
	}
}

void Reverb_process(Reverb *r, const int16_t *fxBuffer, int16_t *out, uint16_t size, uint8_t channels, float dryWet)
{
	if(r->type == REVERB_OFF)
	{
		// same as FX_OFF: add the dry bus (mono routing: left bus channel only)
		const uint32_t step = channels == 2 ? 1u : 2u;
		for(uint32_t i = 0; i < 2u * size; i += step)
			out[i] = sat16((int32_t)out[i] + fxBuffer[i]);
		return;
	}
	if(r->clearPending) Reverb_clear(r);

	uint32_t remaining = size & ~1u;
	while(remaining)
	{
		const uint32_t n = remaining > MAX_CHUNK ? MAX_CHUNK : remaining;
		processChunk(r, fxBuffer, out, n, channels, dryWet);
		fxBuffer += 2 * n;
		out += 2 * n;
		remaining -= n;
	}
}
