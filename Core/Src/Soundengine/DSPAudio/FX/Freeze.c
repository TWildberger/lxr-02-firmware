/*
 * Freeze.c
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

/*
 * Freeze / beat repeat, see Freeze.h for the behaviour.
 *
 * Processing is split into segments between events (repeat start, gate,
 * end of recording, end of a gain ramp, ...). Within a segment all gains
 * change linearly, so the per-frame loops have no branches. Ramp end times
 * are events, so a ramp never overshoots its target.
 *
 * Not placed in ITCM: the ITCM is nearly full and this code is cheap.
 */

#include "Soundengine/DSPAudio/FX/Freeze.h"
#include <math.h>

#define X	FREEZE_XFADE

// slice length per rate index, in 16th steps
static const float stepsPerSlice[FREEZE_NUM_RATES] =
{
	4.f, 8.f / 3.f, 2.f, 4.f / 3.f, 1.f, 2.f / 3.f, 0.5f, 1.f / 3.f
};
// grid period (16th steps) on which slices of that rate line up
static const uint8_t gridSteps[FREEZE_NUM_RATES] =
{
	4, 8, 2, 4, 1, 2, 1, 1
};

static inline int16_t sat16(int32_t v)
{
	if(v > 32767) v = 32767;
	else if(v < -32768) v = -32768;
	return (int16_t)v;
}

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline int due(uint32_t t, uint32_t now)
{
	return (int32_t)(t - now) <= 0;
}

static void rampTo(FreezeRamp *r, float target, uint32_t frames, uint32_t now)
{
	r->target = target;
	r->step = (target - r->value) / (float)frames;
	r->end = now + frames;
	r->active = 1;
}

static void rampSet(FreezeRamp *r, float v)
{
	r->value = v;
	r->target = v;
	r->step = 0.f;
	r->active = 0;
}

static void rampEnd(FreezeRamp *r)
{
	rampSet(r, r->target);
}

//-----------------------------------------------------------------------------
// helpers
//-----------------------------------------------------------------------------
float Freeze_stepsPerSlice(uint8_t idx)
{
	return stepsPerSlice[idx < FREEZE_NUM_RATES ? idx : FREEZE_NUM_RATES - 1];
}

uint8_t Freeze_rateIndex(float v)
{
	const uint32_t x = (uint32_t)(clampf(v, 0.f, 1.f) * 127.f + 0.5f);
	return (uint8_t)(x >> 4);	// 8 zones of 16 values
}

uint32_t Freeze_sliceLength(float stepLen, uint8_t idx, uint32_t capacity, uint8_t *usedIdx)
{
	const uint32_t maxLen = capacity - X - FREEZE_GUARD;
	if(idx >= FREEZE_NUM_RATES) idx = FREEZE_NUM_RATES - 1;

	float l = stepLen * stepsPerSlice[idx];
	// halve within the same family (straight / triplet) until it fits
	while(l > (float)maxLen && idx + 2u < FREEZE_NUM_RATES)
	{
		idx += 2;
		l = stepLen * stepsPerSlice[idx];
	}
	uint32_t len = l > (float)maxLen ? maxLen : (uint32_t)(l + 0.5f);
	if(len < FREEZE_MIN_LEN) len = FREEZE_MIN_LEN;
	if(usedIdx) *usedIdx = idx;
	return len;
}

//-----------------------------------------------------------------------------
// setup and parameters
//-----------------------------------------------------------------------------
static void setChannels(Freeze *f, uint8_t channels)
{
	f->channels = channels;
	f->mask = (channels == 2 ? f->bufSamples / 2u : f->bufSamples) - 1u;
}

void Freeze_init(Freeze *f, __fp16 *buf, uint32_t bufSamples, float fs)
{
	*f = (Freeze){0};
	f->buf = buf;
	f->bufSamples = bufSamples;
	f->fs = fs;
	f->bpm = 120;
	f->kitRate = 4;
	f->speed0 = 1.f;
	f->glideLog2 = 0.f;
	f->decay = 1.f;
	f->gate = 1.f;
	setChannels(f, 2);
	Freeze_reset(f);
}

void Freeze_reset(Freeze *f)
{
	f->state = FREEZE_IDLE;
	f->releasing = 0;
	f->gateActive = 0;
	f->head[0].on = 0;
	f->head[1].on = 0;
	rampSet(&f->head[0].gain, 0.f);
	rampSet(&f->head[1].gain, 0.f);
	rampSet(&f->live, 1.f);
	rampSet(&f->mix, 0.f);
	f->writing = 1;
	f->validFrom = f->now;	// earlier buffer contents belong to another FX
	f->stepCount = 0;
	f->speed = f->speed0;
	f->glideMul = 1.f;
}

void Freeze_setRate(Freeze *f, float v)
{
	f->kitRate = Freeze_rateIndex(v);
}

void Freeze_setPitch(Freeze *f, float v)
{
	const int32_t x = (int32_t)(clampf(v, 0.f, 1.f) * 127.f + 0.5f);
	if(x < 64)
	{
		f->speed0 = exp2f(-(float)(64 - x) / 64.f);
		f->glideLog2 = 0.f;
	}
	else
	{
		f->speed0 = 1.f;
		f->glideLog2 = -2.f * (float)(x - 64) / 63.f;
	}
}

void Freeze_setDecay(Freeze *f, float v)
{
	f->decay = 0.25f + 0.75f * clampf(v, 0.f, 1.f);
}

void Freeze_setGate(Freeze *f, float v)
{
	f->gate = 1.f / 16.f + (15.f / 16.f) * clampf(v, 0.f, 1.f);
}

void Freeze_setTempo(Freeze *f, uint16_t bpm, uint8_t seqRunning)
{
	f->bpm = bpm ? bpm : 120;
	if(!seqRunning) f->stepCount = 0;
	f->seqRunning = seqRunning;
}

void Freeze_step(Freeze *f, uint8_t stepPos)
{
	const uint32_t c = f->stepCount;
	const uint32_t now = f->now;
	if(c)
	{
		f->stepDelta[1] = f->stepDelta[0];
		f->stepDelta[0] = now - f->stepFrame[(c - 1) & (FREEZE_HIST - 1)];
	}
	f->stepFrame[c & (FREEZE_HIST - 1)] = now;
	f->stepPos = stepPos;
	f->stepCount = c + 1;
}

void Freeze_control(Freeze *f, uint8_t value)
{
	f->reqValue = value & 0x7f;
	f->reqSeq = (uint8_t)(f->reqSeq + 1);
}

//-----------------------------------------------------------------------------
// engage / release (audio thread)
//-----------------------------------------------------------------------------

// frames per 16th step. *synced = 1 if the sequencer step grid is usable.
static float stepLength(const Freeze *f, uint8_t *synced)
{
	float len = f->fs * 15.f / (float)f->bpm;
	*synced = 0;
	const uint32_t c = f->stepCount;
	if(f->seqRunning && c)
	{
		if(c >= 3)
		{
			// two intervals: one shuffled and one straight step
			const float m = 0.5f * (float)(f->stepDelta[0] + f->stepDelta[1]);
			if(m > 0.f) len = m;
		}
		const uint32_t age = f->now - f->stepFrame[(c - 1) & (FREEZE_HIST - 1)];
		*synced = (float)age <= 2.f * len + (float)FREEZE_MAX_CHUNK;
	}
	return len;
}

static void arm(Freeze *f, uint8_t idx)
{
	uint8_t synced;
	const float stepLen = stepLength(f, &synced);
	const uint32_t len = Freeze_sliceLength(stepLen, idx, f->mask + 1u, &f->rateIdx);
	const uint32_t now = f->now;
	uint32_t g = now;

	if(synced)
	{
		const uint32_t c = f->stepCount;
		const uint32_t d = f->stepPos % gridSteps[f->rateIdx];
		uint32_t anchor = f->stepFrame[(c - 1) & (FREEZE_HIST - 1)];
		if(d < c && d < FREEZE_HIST)
			anchor = f->stepFrame[(c - 1 - d) & (FREEZE_HIST - 1)];
		if((int32_t)(now - anchor) >= 0)
			g = anchor + ((now - anchor) / len) * len;
	}
	// the pre-roll before g must have been recorded
	while((int32_t)(g - X - 2u - f->validFrom) < 0)
		g += len;

	f->reqIdx = idx;
	f->len = len;
	f->captured = len;	// recorded up to g + len + 1
	f->g = g;
	f->writeStop = g + len + 2u;
	f->nextSpawn = g + len - X;
	f->state = FREEZE_ARMED;
	f->gateActive = 0;
	f->repeat = 0;
	f->writing = 1;
}

static void engage(Freeze *f, uint8_t idx)
{
	const uint8_t wasReleasing = f->releasing;
	const uint32_t now = f->now;
	f->releasing = 0;
	if(f->mix.target < 1.f || f->mix.value < 1.f)
		rampTo(&f->mix, 1.f, FREEZE_RAMP, now);

	switch(f->state)
	{
	case FREEZE_IDLE:
		arm(f, idx);
		break;

	case FREEZE_ARMED:
		// events were not handled while releasing: always start over
		if(idx != f->reqIdx || wasReleasing)
			arm(f, idx);
		break;

	case FREEZE_LOOPING:
	default:
	{
		if(idx == f->reqIdx)
			break;
		uint8_t synced, used;
		const float stepLen = stepLength(f, &synced);
		const uint32_t len = Freeze_sliceLength(stepLen, idx, f->mask + 1u, &used);
		if(len <= f->captured)
		{
			// Another slice of the same recording: keep g and the grid. The
			// current head reads at most up to its scheduled end, so the next
			// repeat must not start later than planned: take the last grid
			// line of the new length before it. That line must be at least
			// two crossfades away, so the previous head switch has finished.
			// If there is none, keep the planned repeat (off the new grid).
			const uint32_t k = (f->nextSpawn + X - f->g) / len;
			const uint32_t s = f->g + k * len - X;
			if(k >= 1 && (int32_t)(s - now) >= (int32_t)X)
				f->nextSpawn = s;
			f->len = len;
			f->rateIdx = used;
			f->reqIdx = idx;
			f->gateActive = 0;
		}
		else
		{
			// longer than what was recorded: back to live, record a new slice
			for(uint32_t i = 0; i < 2; i++)
				if(f->head[i].on) rampTo(&f->head[i].gain, 0.f, X, now);
			rampTo(&f->live, 1.f, X, now);
			// don't overwrite what the fading heads still read
			f->validFrom = now + X;
			arm(f, idx);
		}
		break;
	}
	}
}

static void release(Freeze *f)
{
	if(f->state == FREEZE_IDLE || f->releasing)
		return;
	f->releasing = 1;
	rampTo(&f->mix, 0.f, FREEZE_RAMP, f->now);
}

static void goIdle(Freeze *f)
{
	f->state = FREEZE_IDLE;
	f->releasing = 0;
	f->gateActive = 0;
	f->head[0].on = 0;
	f->head[1].on = 0;
	rampSet(&f->head[0].gain, 0.f);
	rampSet(&f->head[1].gain, 0.f);
	rampSet(&f->live, 1.f);
	if(!f->writing)
	{
		f->writing = 1;
		f->validFrom = f->now;
	}
	f->speed = f->speed0;
	f->glideMul = 1.f;
}

static void spawn(Freeze *f)
{
	const uint32_t now = f->now;
	const uint32_t b = f->nextSpawn + X;	// repeat start
	const uint32_t len = f->len;

	if(f->state == FREEZE_ARMED)
	{
		f->state = FREEZE_LOOPING;
		f->level = 1.f;
		f->repeat = 1;
		f->speed = f->speed0;
		rampTo(&f->live, 0.f, X, now);
	}
	else
	{
		f->level *= f->decay;
		f->repeat++;
	}
	if(f->glideLog2 < 0.f)
	{
		f->glideMul = exp2f(f->glideLog2 / (float)len);
	}
	else
	{
		f->glideMul = 1.f;
		f->speed = f->speed0;
	}

	FreezeHead *old = &f->head[f->curHead];
	if(old->on) rampTo(&old->gain, 0.f, X, now);
	f->curHead ^= 1;
	FreezeHead *h = &f->head[f->curHead];
	h->on = 1;
	h->base = f->g;
	// at time b the head is at the slice start
	h->off = -f->speed * (float)(int32_t)(b - now);
	h->gain.value = 0.f;
	rampTo(&h->gain, f->level, X, now);

	uint32_t gateLen = (uint32_t)(f->gate * (float)len);
	if(gateLen < X) gateLen = X;
	f->gateActive = gateLen + X <= len;
	f->gateTime = b + gateLen - X;

	f->nextSpawn += len;
}

static inline int eventsBlocked(const Freeze *f)
{
	// a release before the first repeat: keep recording, don't start repeats
	return f->state == FREEZE_IDLE || (f->releasing && f->state == FREEZE_ARMED);
}

static void handleEvents(Freeze *f)
{
	const uint32_t now = f->now;

	if(f->live.active && due(f->live.end, now))
		rampEnd(&f->live);
	for(uint32_t i = 0; i < 2; i++)
	{
		FreezeHead *h = &f->head[i];
		if(h->gain.active && due(h->gain.end, now))
		{
			rampEnd(&h->gain);
			if(h->gain.value == 0.f) h->on = 0;
		}
	}
	if(f->mix.active && due(f->mix.end, now))
	{
		rampEnd(&f->mix);
		if(f->releasing && f->mix.value == 0.f)
			goIdle(f);
	}
	if(eventsBlocked(f))
		return;

	if(f->gateActive && due(f->gateTime, now))
	{
		f->gateActive = 0;
		FreezeHead *h = &f->head[f->curHead];
		if(h->on) rampTo(&h->gain, 0.f, X, now);
	}
	if(due(f->nextSpawn, now))
		spawn(f);
	if(f->writing && f->state == FREEZE_LOOPING && due(f->writeStop, now))
	{
		f->writing = 0;
	}
}

static uint32_t nextEventDelta(const Freeze *f)
{
	const uint32_t now = f->now;
	uint32_t best = 0xffffffffu;
#define CAND(t) do { const int32_t d_ = (int32_t)((t) - now); if(d_ > 0 && (uint32_t)d_ < best) best = (uint32_t)d_; } while(0)
	if(f->live.active) CAND(f->live.end);
	if(f->mix.active) CAND(f->mix.end);
	if(f->head[0].gain.active) CAND(f->head[0].gain.end);
	if(f->head[1].gain.active) CAND(f->head[1].gain.end);
	if(f->writing) CAND(f->validFrom);
	if(!eventsBlocked(f))
	{
		CAND(f->nextSpawn);
		if(f->gateActive) CAND(f->gateTime);
		if(f->writing && f->state == FREEZE_LOOPING) CAND(f->writeStop);
	}
#undef CAND
	return best;
}

//-----------------------------------------------------------------------------
// rendering
//-----------------------------------------------------------------------------
static void renderHead(const Freeze *f, FreezeHead *h, float *wl, float *wr, uint32_t n)
{
	const __fp16 *buf = f->buf;
	const uint32_t mask = f->mask;
	const uint32_t g = h->base;
	const float gm = f->glideMul;
	float off = h->off;
	float gain = h->gain.value;
	const float gs = h->gain.step;
	float sp = f->speed;

	if(f->channels == 2)
	{
		for(uint32_t i = 0; i < n; i++)
		{
			const float fl = floorf(off);
			const float fr = off - fl;
			const uint32_t a = ((g + (uint32_t)(int32_t)fl) & mask) * 2u;
			const uint32_t b = ((g + (uint32_t)(int32_t)fl + 1u) & mask) * 2u;
			// fades out as a tape stop gets very slow (no DC hold)
			const float ge = gain * fminf(1.f, sp * 4.f);
			const float l0 = (float)buf[a], l1 = (float)buf[b];
			const float r0 = (float)buf[a + 1], r1 = (float)buf[b + 1];
			wl[i] += ge * (l0 + fr * (l1 - l0));
			wr[i] += ge * (r0 + fr * (r1 - r0));
			off += sp;
			sp = fmaxf(sp * gm, FREEZE_MIN_SPEED);
			gain += gs;
		}
	}
	else
	{
		for(uint32_t i = 0; i < n; i++)
		{
			const float fl = floorf(off);
			const float fr = off - fl;
			const uint32_t a = (g + (uint32_t)(int32_t)fl) & mask;
			const uint32_t b = (a + 1u) & mask;
			const float ge = gain * fminf(1.f, sp * 4.f);
			const float s0 = (float)buf[a], s1 = (float)buf[b];
			wl[i] += ge * (s0 + fr * (s1 - s0));
			off += sp;
			sp = fmaxf(sp * gm, FREEZE_MIN_SPEED);
			gain += gs;
		}
	}
	h->off = off;
	h->gain.value = gain;
}

static void renderSegment(Freeze *f, const int16_t *in, int16_t *out, uint32_t n, float dryWet)
{
	float wl[FREEZE_MAX_CHUNK];
	float wr[FREEZE_MAX_CHUNK];
	const uint8_t stereo = f->channels == 2;
	const uint32_t now = f->now;

	// record
	if(f->writing && (int32_t)(now - f->validFrom) >= 0)
	{
		__fp16 *buf = f->buf;
		const uint32_t mask = f->mask;
		if(stereo)
		{
			for(uint32_t i = 0; i < n; i++)
			{
				const uint32_t w = ((now + i) & mask) * 2u;
				buf[w] = (__fp16)(float)in[2 * i];
				buf[w + 1] = (__fp16)(float)in[2 * i + 1];
			}
		}
		else
		{
			for(uint32_t i = 0; i < n; i++)
				buf[(now + i) & mask] = (__fp16)(float)in[2 * i];
		}
	}

	// live part of the wet signal
	{
		float lv = f->live.value;
		const float ls = f->live.step;
		if(lv == 0.f && ls == 0.f)
		{
			for(uint32_t i = 0; i < n; i++)
				wl[i] = wr[i] = 0.f;
		}
		else for(uint32_t i = 0; i < n; i++)
		{
			wl[i] = lv * (float)in[2 * i];
			wr[i] = stereo ? lv * (float)in[2 * i + 1] : 0.f;
			lv += ls;
		}
		f->live.value = lv;
	}

	// repeats
	for(uint32_t k = 0; k < 2; k++)
		if(f->head[k].on) renderHead(f, &f->head[k], wl, wr, n);
	if(f->glideMul != 1.f)
	{
		float sp = f->speed;
		for(uint32_t i = 0; i < n; i++)
			sp = fmaxf(sp * f->glideMul, FREEZE_MIN_SPEED);
		f->speed = sp;
	}

	// mix: dry + (wet - dry) * dryWet * engage
	{
		float m = f->mix.value;
		const float ms = f->mix.step;
		for(uint32_t i = 0; i < n; i++)
		{
			const float k = dryWet * m;
			const float dl = (float)in[2 * i];
			const float vl = clampf(dl + (wl[i] - dl) * k, -32768.f, 32767.f);
			out[2 * i] = sat16((int32_t)out[2 * i] + (int32_t)vl);
			if(stereo)
			{
				const float dr = (float)in[2 * i + 1];
				const float vr = clampf(dr + (wr[i] - dr) * k, -32768.f, 32767.f);
				out[2 * i + 1] = sat16((int32_t)out[2 * i + 1] + (int32_t)vr);
			}
			m += ms;
		}
		f->mix.value = m;
	}

	f->now = now + n;
}

static void applyRequests(Freeze *f)
{
	const uint8_t seq = f->reqSeq;
	if(seq != f->reqSeen)
	{
		f->reqSeen = seq;
		const uint8_t v = f->reqValue;
		if(v == FREEZE_CTRL_RELEASE)
		{
			release(f);
		}
		else
		{
			f->engagedRate = v <= FREEZE_NUM_RATES ? (uint8_t)(v - 1u) : FREEZE_CTRL_KIT;
			engage(f, f->engagedRate == FREEZE_CTRL_KIT ? f->kitRate : f->engagedRate);
		}
	}
	else if(f->state != FREEZE_IDLE && !f->releasing && f->engagedRate == FREEZE_CTRL_KIT
			&& f->kitRate != f->reqIdx)
	{
		engage(f, f->kitRate);
	}
}

void Freeze_process(Freeze *f, const int16_t *in, int16_t *out, uint32_t frames, uint8_t channels, float dryWet)
{
	if(channels != f->channels)
	{
		// routing changed: the recording has the wrong layout
		setChannels(f, channels);
		Freeze_reset(f);
	}
	// keep validFrom within one buffer length of now, so the signed frame
	// comparisons stay valid however long the FX runs
	if(f->writing && (int32_t)(f->now - f->validFrom) > (int32_t)(f->mask + 1u))
		f->validFrom = f->now - (f->mask + 1u);
	applyRequests(f);

	if(f->state == FREEZE_IDLE && !f->mix.active && f->mix.value == 0.f)
	{
		// bit-exact passthrough (same as FX type "off") while recording
		const uint32_t now = f->now;
		const uint32_t mask = f->mask;
		__fp16 *buf = f->buf;
		const uint8_t rec = f->writing && (int32_t)(now - f->validFrom) >= 0;
		if(channels == 2)
		{
			for(uint32_t i = 0; i < frames; i++)
			{
				const int16_t l = in[2 * i], r = in[2 * i + 1];
				if(rec)
				{
					const uint32_t w = ((now + i) & mask) * 2u;
					buf[w] = (__fp16)(float)l;
					buf[w + 1] = (__fp16)(float)r;
				}
				out[2 * i] = sat16((int32_t)out[2 * i] + l);
				out[2 * i + 1] = sat16((int32_t)out[2 * i + 1] + r);
			}
		}
		else
		{
			for(uint32_t i = 0; i < frames; i++)
			{
				const int16_t l = in[2 * i];
				if(rec) buf[(now + i) & mask] = (__fp16)(float)l;
				out[2 * i] = sat16((int32_t)out[2 * i] + l);
			}
		}
		f->now = now + frames;
		return;
	}

	uint32_t i = 0;
	while(i < frames)
	{
		handleEvents(f);
		uint32_t n = frames - i;
		if(n > FREEZE_MAX_CHUNK) n = FREEZE_MAX_CHUNK;
		const uint32_t d = nextEventDelta(f);
		if(d < n) n = d;
		renderSegment(f, in + 2 * i, out + 2 * i, n, dryWet);
		i += n;
	}
}
