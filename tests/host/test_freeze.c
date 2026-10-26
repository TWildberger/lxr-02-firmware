/*
 * Host tests for the freeze / beat repeat FX (Core/Src/Soundengine/DSPAudio/FX/Freeze.c).
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Soundengine/DSPAudio/FX/Freeze.h"

#define FS 44140.f
#define BUF 32768u
#define BLOCK 64u
#define MAXF (200000u)

static __fp16 buf[BUF];
static int16_t inBuf[2 * MAXF];
static int16_t outBuf[2 * MAXF];
static Freeze fz;
static int failures;

#define CHECK(c, ...) do { if(!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

static uint32_t rng = 1;
static int16_t noise(void)
{
	rng = rng * 1664525u + 1013904223u;
	return (int16_t)((int32_t)(rng >> 16) % 16000);
}

static void fillNoise(uint32_t frames)
{
	for(uint32_t i = 0; i < 2 * frames; i++) inBuf[i] = noise();
}

static void fillSine(uint32_t frames)
{
	for(uint32_t i = 0; i < frames; i++)
	{
		inBuf[2 * i] = (int16_t)(10000.f * sinf(2.f * 3.14159265f * 220.f * (float)i / FS));
		inBuf[2 * i + 1] = (int16_t)(8000.f * sinf(2.f * 3.14159265f * 330.f * (float)i / FS));
	}
}

static void setup(uint8_t channels)
{
	(void)channels;
	for(uint32_t i = 0; i < BUF; i++) buf[i] = (__fp16)NAN;	// unrecorded memory must never be heard
	Freeze_init(&fz, buf, BUF, FS);
	memset(outBuf, 0, sizeof(outBuf));
}

// sequencer emulation: a step every stepFrames frames (multiple of BLOCK), or none
static uint32_t stepFrames;
static uint8_t stepPos;

static void run(uint32_t from, uint32_t to, uint8_t channels, float dryWet)
{
	for(uint32_t t = from; t < to; t += BLOCK)
	{
		Freeze_setTempo(&fz, 120, stepFrames != 0);
		if(stepFrames && t % stepFrames == 0)
			Freeze_step(&fz, (uint8_t)(stepPos++ & 15));
		Freeze_process(&fz, &inBuf[2 * t], &outBuf[2 * t], BLOCK, channels, dryWet);
	}
}

static int close16(int16_t got, float want)
{
	return fabsf((float)got - want) <= fabsf(want) / 1024.f + 2.f;
}

static int16_t L(uint32_t t) { return outBuf[2 * t]; }
static int16_t R(uint32_t t) { return outBuf[2 * t + 1]; }

// out[b + j] ~ in[g + j] for j in [0, n)
static int checkRepeat(uint32_t b, uint32_t g, uint32_t n, uint8_t channels, float level)
{
	int bad = 0;
	for(uint32_t j = 0; j < n; j++)
	{
		if(!close16(L(b + j), level * (float)inBuf[2 * (g + j)])) bad++;
		if(channels == 2 && !close16(R(b + j), level * (float)inBuf[2 * (g + j) + 1])) bad++;
	}
	return bad;
}

static uint32_t maxStep(uint32_t from, uint32_t to)
{
	uint32_t m = 0;
	for(uint32_t t = from + 1; t < to; t++)
	{
		const uint32_t dl = (uint32_t)abs(L(t) - L(t - 1));
		const uint32_t dr = (uint32_t)abs(R(t) - R(t - 1));
		if(dl > m) m = dl;
		if(dr > m) m = dr;
	}
	return m;
}

static double rms(uint32_t from, uint32_t to)
{
	double s = 0;
	for(uint32_t t = from; t < to; t++) s += (double)L(t) * L(t);
	return sqrt(s / (double)(to - from));
}

//-----------------------------------------------------------------------------
static void test_math(void)
{
	uint8_t used;
	const float step120 = FS * 15.f / 120.f;	// 5517.5

	CHECK(Freeze_rateIndex(0.f) == 0, "zone 0");
	CHECK(Freeze_rateIndex(15.f / 127.f) == 0, "zone 0 end");
	CHECK(Freeze_rateIndex(16.f / 127.f) == 1, "zone 1");
	CHECK(Freeze_rateIndex(111.f / 127.f) == 6, "zone 6 end");
	CHECK(Freeze_rateIndex(112.f / 127.f) == 7, "zone 7");
	CHECK(Freeze_rateIndex(1.f) == 7, "zone 7 end");

	CHECK(Freeze_sliceLength(step120, 0, 32768, &used) == 22070 && used == 0, "1/4 mono @120");
	CHECK(Freeze_sliceLength(step120, 0, 16384, &used) == 11035 && used == 2, "1/4 stereo @120 -> 1/8");
	CHECK(Freeze_sliceLength(step120, 1, 16384, &used) == 14713 && used == 1, "1/4T stereo @120 fits");
	CHECK(Freeze_sliceLength(step120, 4, 16384, &used) == 5518 && used == 4, "1/16 @120");
	CHECK(Freeze_sliceLength(step120, 7, 16384, &used) == 1839 && used == 7, "1/32T @120");
	CHECK(Freeze_sliceLength(FS * 15.f / 60.f, 0, 16384, &used) == 11035 && used == 4, "1/4 stereo @60 -> 1/16");
	CHECK(Freeze_sliceLength(FS * 15.f / 60.f, 1, 16384, &used) == 14713 && used == 3, "1/4T stereo @60 -> 1/8T");
	// absurdly slow: longest that fits
	CHECK(Freeze_sliceLength(100000.f, 0, 16384, &used) == 16384 - FREEZE_XFADE - FREEZE_GUARD, "clamp to capacity");
	// absurdly fast: minimum length
	CHECK(Freeze_sliceLength(10.f, 7, 16384, &used) == FREEZE_MIN_LEN, "minimum length");
	CHECK(fabsf(Freeze_stepsPerSlice(3) - 4.f / 3.f) < 1e-6f, "1/8T = 4/3 steps");
}

static void test_passthrough(void)
{
	for(uint8_t ch = 1; ch <= 2; ch++)
	{
		setup(ch);
		stepFrames = 0;
		fillNoise(4096);
		for(uint32_t i = 0; i < 2 * 4096; i++) outBuf[i] = (int16_t)(noise() * 2);
		int16_t ref[2 * 4096];
		for(uint32_t i = 0; i < 2 * 4096; i++)
		{
			int32_t v = outBuf[i] + ((ch == 2 || (i & 1) == 0) ? inBuf[i] : 0);
			ref[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		run(0, 4096, ch, 1.f);
		CHECK(memcmp(ref, outBuf, sizeof(ref)) == 0, "idle passthrough bit-exact (%u ch)", ch);
	}
}

// engage while the sequencer is stopped: the slice starts at the engage time
static void test_freeRunning(uint8_t ch)
{
	setup(ch);
	stepFrames = 0;
	fillNoise(100000);
	const uint32_t E = 12800;
	run(0, E, ch, 1.f);
	Freeze_control(&fz, 5);	// 1/16
	run(E, 59968, ch, 1.f);

	const uint32_t g = E, len = 5518, X = FREEZE_XFADE;
	CHECK(fz.g == g && fz.len == len, "free-running slice g=%u len=%u", fz.g, fz.len);
	int same = 1;
	for(uint32_t t = E; t < g + len - X; t++)
		if(L(t) != inBuf[2 * t] || (ch == 2 && R(t) != inBuf[2 * t + 1])) same = 0;
	CHECK(same, "live until the first repeat (%u ch)", ch);
	for(uint32_t k = 1; k <= 5; k++)
		CHECK(checkRepeat(g + k * len, g, len - X, ch, 1.f) == 0, "repeat %u matches the slice (%u ch)", k, ch);
}

static void test_synced(void)
{
	// steps every 5504 frames (86 blocks, ~120.3 BPM), mono
	struct { uint8_t ctrl; uint32_t engageStep; uint32_t anchorStep; float steps; } c[] =
	{
		{3, 6, 6, 2.f},				// 1/8 on step 6 -> step 6
		{3, 7, 6, 2.f},				// 1/8 on step 7 -> step 6
		{1, 7, 4, 4.f},				// 1/4 on step 7 -> step 4
		{4, 5, 4, 4.f / 3.f},		// 1/8T on step 5 -> step 4
		{5, 9, 9, 1.f},				// 1/16 -> current step
	};
	for(uint32_t n = 0; n < sizeof(c) / sizeof(c[0]); n++)
	{
		setup(1);
		stepFrames = 5504;
		stepPos = 0;
		fillNoise(120000);
		const uint32_t E = c[n].engageStep * 5504 + 1024;
		run(0, E, 1, 1.f);
		Freeze_control(&fz, c[n].ctrl);
		run(E, 109952, 1, 1.f);
		const uint32_t len = (uint32_t)(5504.f * c[n].steps + 0.5f);
		uint32_t g = c[n].anchorStep * 5504;
		while(g + len <= E) g += len;
		CHECK(fz.g == g && fz.len == len, "synced case %u: g=%u (want %u) len=%u (want %u)", n, fz.g, g, fz.len, len);
		CHECK(checkRepeat(g + len, g, len - FREEZE_XFADE, 1, 1.f) == 0, "synced case %u repeat 1", n);
		CHECK(checkRepeat(g + 2 * len, g, len - FREEZE_XFADE, 1, 1.f) == 0, "synced case %u repeat 2", n);
	}
	stepFrames = 0;
}

static void test_clicks(void)
{
	setup(2);
	stepFrames = 0;
	fillSine(120000);
	run(0, 8960, 2, 1.f);
	const uint32_t base = maxStep(1, 8960);
	Freeze_control(&fz, 6);	// 1/16T: slice is not a whole number of sine periods
	run(8960, 39936, 2, 1.f);
	Freeze_control(&fz, 3);	// longer than recorded: back to live, new slice
	run(39936, 69952, 2, 1.f);
	Freeze_control(&fz, 7);	// shorter: same recording
	run(69952, 89984, 2, 1.f);
	Freeze_control(&fz, 0);
	run(89984, 119936, 2, 1.f);
	const uint32_t m = maxStep(1, 119936);
	CHECK(m < 3 * base, "no clicks: max step %u, sine alone %u", m, base);}

static void test_release(void)
{
	setup(2);
	stepFrames = 0;
	fillNoise(60000);
	run(0, 6400, 2, 1.f);
	Freeze_control(&fz, 5);
	run(6400, 30016, 2, 1.f);
	CHECK(fz.state == FREEZE_LOOPING, "looping");
	Freeze_control(&fz, 0);
	run(30016, 59968, 2, 1.f);
	CHECK(fz.state == FREEZE_IDLE, "idle after release");
	int same = 1;
	for(uint32_t t = 30016 + FREEZE_RAMP + BLOCK; t < 59968; t++)
		if(L(t) != inBuf[2 * t] || R(t) != inBuf[2 * t + 1]) same = 0;
	CHECK(same, "bit-exact passthrough after release");
	const uint32_t last = 59968 - 1, w = (last & fz.mask) * 2;
	CHECK(fz.writing == 1 && (float)buf[w] == (float)(__fp16)(float)inBuf[2 * last], "recording again");
}

static void test_decayGate(void)
{
	setup(1);
	stepFrames = 0;
	fillNoise(80000);
	Freeze_setDecay(&fz, 0.f);	// 0.25 per repeat
	run(0, 6400, 1, 1.f);
	Freeze_control(&fz, 5);
	run(6400, 39936, 1, 1.f);
	const uint32_t g = fz.g, len = fz.len, X = FREEZE_XFADE;
	CHECK(checkRepeat(g + len, g, len - X, 1, 1.f) == 0, "first repeat at full level");
	CHECK(checkRepeat(g + 2 * len, g, len - X, 1, 0.25f) == 0, "second repeat at 0.25");
	CHECK(checkRepeat(g + 3 * len, g, len - X, 1, 0.0625f) == 0, "third repeat at 0.0625");

	setup(1);
	fillNoise(80000);
	Freeze_setGate(&fz, 0.f);	// 1/16 of the repeat
	run(0, 6400, 1, 1.f);
	Freeze_control(&fz, 5);
	run(6400, 39936, 1, 1.f);
	const uint32_t gate = len / 16;
	int silent = 1;
	for(uint32_t k = 1; k <= 3; k++)
		for(uint32_t t = g + k * len + gate; t < g + (k + 1) * len - X; t++)
			if(L(t) != 0) silent = 0;
	CHECK(silent, "gated part of each repeat is silent");
	CHECK(checkRepeat(g + len, g, gate - X, 1, 1.f) == 0, "gate keeps the start of the repeat");
}

static void test_pitch(void)
{
	setup(1);
	stepFrames = 0;
	fillNoise(80000);
	Freeze_setPitch(&fz, 0.f);	// half speed
	run(0, 6400, 1, 1.f);
	Freeze_control(&fz, 5);
	run(6400, 39936, 1, 1.f);
	const uint32_t g = fz.g, len = fz.len;
	int bad = 0;
	for(uint32_t j = 0; 2 * j < len - FREEZE_XFADE; j++)
		if(!close16(L(g + len + 2 * j), (float)inBuf[2 * (g + j)])) bad++;
	CHECK(bad == 0, "octave down plays the slice at half speed (%d bad)", bad);

	// roll-down: speed falls every repeat, level fades out, nothing explodes
	setup(1);
	fillNoise(200000);
	Freeze_setPitch(&fz, 1.f);
	run(0, 6400, 1, 1.f);
	Freeze_control(&fz, 5);
	run(6400, 199936, 1, 1.f);
	const double r1 = rms(g + len + FREEZE_XFADE, g + 2 * len - FREEZE_XFADE);
	const double r8 = rms(g + 8 * len, g + 9 * len);
	CHECK(fz.speed >= FREEZE_MIN_SPEED && fz.speed < 0.05f, "speed glided down to %f", (double)fz.speed);
	CHECK(r8 < 0.5 * r1, "tape stop fades out (%f -> %f)", r1, r8);
	CHECK(maxStep(6400, 199936) < 20000, "no full-scale jumps while gliding");
}

static void test_rateChange(void)
{
	setup(1);
	stepFrames = 0;
	fillNoise(80000);
	const uint32_t X = FREEZE_XFADE;
	run(0, 6400, 1, 1.f);
	Freeze_control(&fz, 3);	// 1/8 = 11035
	run(6400, 20032, 1, 1.f);
	const uint32_t g = fz.g;
	CHECK(fz.len == 11035 && fz.state == FREEZE_LOOPING, "1/8 looping");
	// shorter: the same recording, the grid of the new length from g
	const uint32_t now = 20032;
	const uint32_t planned = fz.nextSpawn + X;	// next 1/8 repeat
	Freeze_control(&fz, 5);
	run(now, 59968, 1, 1.f);
	const uint32_t len = 5518;
	// the last 1/16 grid line before the planned repeat
	const uint32_t k = (planned - g) / len;
	const uint32_t b = g + k * len;
	CHECK(b <= planned && b >= now + 2 * X, "shorter rate: next repeat at %u (planned %u)", b, planned);
	CHECK(fz.g == g && fz.len == len, "shorter rate keeps g");
	CHECK(checkRepeat(b, g, len - X, 1, 1.f) == 0, "shorter rate repeats from g");
	CHECK(checkRepeat(b + len, g, len - X, 1, 1.f) == 0, "shorter rate repeats again");

	// longer than recorded: back to live, a new slice is recorded
	setup(1);
	fillNoise(80000);
	run(0, 6400, 1, 1.f);
	Freeze_control(&fz, 5);
	run(6400, 12800, 1, 1.f);
	run(12800, 14016, 1, 1.f);
	CHECK(fz.state == FREEZE_LOOPING, "1/16 looping");
	Freeze_control(&fz, 3);
	run(14016, 79936, 1, 1.f);
	const uint32_t g2 = fz.g;
	CHECK(fz.len == 11035 && (int32_t)(g2 - X - 2 - (14016 + X)) >= 0, "new slice after the old heads faded (g=%u)", g2);
	int live = 1;
	for(uint32_t t = 14016 + X; t < g2 + fz.len - X; t++)
		if(L(t) != inBuf[2 * t]) live = 0;
	CHECK(live, "live again until the new slice repeats");
	CHECK(checkRepeat(g2 + fz.len, g2, fz.len - X, 1, 1.f) == 0, "new slice repeats");
}

// The heads never read past the recorded slice, however the rate is changed
// while looping (the recording stops at g + captured).
static float maxHeadOff;
static void runWatchHeads(uint32_t from, uint32_t to, uint8_t channels)
{
	for(uint32_t t = from; t < to; t += BLOCK)
	{
		run(t, t + BLOCK, channels, 1.f);
		for(uint32_t i = 0; i < 2; i++)
			if(fz.head[i].on && fz.head[i].off > maxHeadOff) maxHeadOff = fz.head[i].off;
	}
}

static void test_rateChangeBounds(void)
{
	// change rate just before the planned repeat, to shorter and back to longer
	static const uint8_t seqs[][2] = { {3, 5}, {3, 7}, {5, 3}, {3, 6}, {4, 2} };
	for(uint32_t n = 0; n < sizeof(seqs) / sizeof(seqs[0]); n++)
	{
		for(uint32_t lead = 64; lead <= 3 * 64 + 64; lead += 192)
		{
			setup(2);
			stepFrames = 0;
			fillSine(120000);
			run(0, 6400, 2, 1.f);
			const uint32_t base = maxStep(1, 6400);
			Freeze_control(&fz, seqs[n][0]);
			maxHeadOff = 0.f;
			runWatchHeads(6400, 6464, 2);
			// to repeat 1, then to `lead` frames before the next spawn
			while(fz.state != FREEZE_LOOPING || fz.repeat < 1) runWatchHeads(fz.now, fz.now + BLOCK, 2);
			while((int32_t)(fz.nextSpawn - fz.now) > (int32_t)lead) runWatchHeads(fz.now, fz.now + BLOCK, 2);
			Freeze_control(&fz, seqs[n][1]);
			const uint32_t t0 = fz.now;
			runWatchHeads(t0, t0 + 60032, 2);
			CHECK(maxHeadOff <= (float)fz.captured + 1.f, "case %u/%u: head read up to %.0f, recorded %u",
				n, lead, (double)maxHeadOff, fz.captured);
			const uint32_t m = maxStep(6400, t0 + 60032);
			CHECK(m < 3 * base, "case %u/%u: no clicks: max step %u, sine alone %u", n, lead, m, base);
		}
	}
}

// the frame counter wraps and validFrom can be ~2^31 frames old (13.5 h)
static void test_longRun(void)
{
	setup(2);
	stepFrames = 0;
	fillNoise(40000);
	const uint32_t t0 = 0x80000000u + 1000u;
	fz.now = t0;
	fz.validFrom = t0 - 0x7ffff000u;	// nearly 2^31 frames since the FX was selected
	run(0, 6400, 2, 1.f);
	CHECK((int32_t)(fz.now - fz.validFrom) <= (int32_t)(fz.mask + 1u + BLOCK), "validFrom follows now");
	Freeze_control(&fz, 5);
	run(6400, 39936, 2, 1.f);
	CHECK(fz.state == FREEZE_LOOPING && fz.repeat >= 2, "engages after a long run (state %u)", fz.state);
	const uint32_t g = fz.g - t0;
	CHECK(g <= 6400 + BLOCK, "slice starts at the engage (g=%u)", g);
	CHECK(checkRepeat(g + fz.len, g, fz.len - FREEZE_XFADE, 2, 1.f) == 0, "repeat ok after a long run");

	// and across the 2^32 wrap
	setup(2);
	fillNoise(40000);
	const uint32_t t1 = 0xffffffffu - 9000u;
	fz.now = t1;
	fz.validFrom = t1;
	run(0, 6400, 2, 1.f);
	Freeze_control(&fz, 5);
	run(6400, 39936, 2, 1.f);
	const uint32_t g1 = fz.g - t1;
	CHECK(fz.state == FREEZE_LOOPING, "engages across the wrap");
	CHECK(checkRepeat(g1 + fz.len, g1, fz.len - FREEZE_XFADE, 2, 1.f) == 0, "repeat ok across the wrap");
}

static void test_validFrom(void)
{
	// engage right after the FX was selected: only recorded audio is used
	setup(2);
	stepFrames = 0;
	fillNoise(40000);
	Freeze_control(&fz, 8);	// 1/32T
	run(0, 39936, 2, 1.f);
	CHECK((int32_t)(fz.g - FREEZE_XFADE - 2 - fz.validFrom) >= 0, "pre-roll inside the recording");
	CHECK(checkRepeat(fz.g + fz.len, fz.g, fz.len - FREEZE_XFADE, 2, 1.f) == 0, "repeat ok");
}

static void test_kitRate(void)
{
	setup(1);
	stepFrames = 0;
	fillNoise(40000);
	Freeze_setRate(&fz, 64.f / 127.f);	// zone 4: 1/16
	run(0, 6400, 1, 1.f);
	Freeze_control(&fz, 127);
	run(6400, 6464, 1, 1.f);
	CHECK(fz.engagedRate == FREEZE_CTRL_KIT && fz.reqIdx == 4 && fz.len == 5518, "engage at the kit rate");
	Freeze_setRate(&fz, 100.f / 127.f);	// zone 6: 1/32
	run(6464, 6528, 1, 1.f);
	CHECK(fz.reqIdx == 6 && fz.len == 2759, "follows P1 while engaged");
	Freeze_control(&fz, 2);	// explicit 1/4T
	run(6528, 6592, 1, 1.f);
	Freeze_setRate(&fz, 0.f);
	run(6592, 6656, 1, 1.f);
	CHECK(fz.reqIdx == 1, "explicit rate ignores P1");
	Freeze_control(&fz, 0);
	run(6656, 7680, 1, 1.f);
	CHECK(fz.state == FREEZE_IDLE, "released");
}

static void test_routingChange(void)
{
	setup(2);
	stepFrames = 0;
	fillNoise(40000);
	run(0, 6400, 2, 1.f);
	Freeze_control(&fz, 5);
	run(6400, 20032, 2, 1.f);
	run(20032, 20096, 1, 1.f);
	CHECK(fz.state == FREEZE_IDLE && fz.channels == 1 && fz.validFrom == 20032, "routing change resets");
}

int main(void)
{
	test_math();
	test_passthrough();
	test_freeRunning(1);
	test_freeRunning(2);
	test_synced();
	test_clicks();
	test_release();
	test_decayGate();
	test_pitch();
	test_rateChange();
	test_rateChangeBounds();
	test_longRun();
	test_validFrom();
	test_kitRate();
	test_routingChange();
	if(failures)
	{
		printf("test_freeze: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_freeze: all tests passed\n");
	return 0;
}
