/*
 * Host-side tests for the FX bus plate reverb
 * (Core/Src/Soundengine/DSPAudio/FX/Reverb.c). Run with: make -C tests/host
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Soundengine/DSPAudio/FX/Reverb.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
		checks++; \
		if(!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
	} while(0)

#define BLOCK 64
#define FS 44140

static __attribute__((aligned(8))) __fp16 buf[REVERB_BUFFER_SAMPLES];
static Reverb rev;

static uint32_t rng = 1;
static int16_t noise(int16_t amp)
{
	rng = rng * 1664525u + 1013904223u;
	return (int16_t)(((int32_t)(rng >> 16) - 32768) * amp / 32768);
}

static void setup(float pre, float decay, float tone, float early, uint8_t type)
{
	Reverb_init(&rev, buf);
	Reverb_setPreDelay(&rev, pre);
	Reverb_setDecay(&rev, decay);
	Reverb_setTone(&rev, tone);
	Reverb_setEarly(&rev, early);
	Reverb_setType(&rev, type);
	CHECK(rev.type == type);
}

// run one block of wet-only output with silence or the given input
static void runBlock(const int16_t *in, int16_t *out, uint8_t channels, float dryWet)
{
	static const int16_t zero[2 * BLOCK];
	memset(out, 0, 2 * BLOCK * sizeof(int16_t));
	Reverb_process(&rev, in ? in : zero, out, BLOCK, channels, dryWet);
}

static double blockEnergy(const int16_t *out)
{
	double e = 0;
	for(int i = 0; i < 2 * BLOCK; i++) e += (double)out[i] * out[i];
	return e;
}

static int bufferFinite(void)
{
	for(uint32_t i = 0; i < REVERB_BUFFER_SAMPLES; i++)
		if(!isfinite((float)buf[i])) return 0;
	return 1;
}
//---------------------------------------------------------------------------
// type "off" must behave exactly like FX_OFF in FxBus_process: legacy kits
// saved with FX type 5 rely on it
static void test_off_is_fx_off(void)
{
	int16_t in[2 * BLOCK], out[2 * BLOCK], ref[2 * BLOCK];
	Reverb_init(&rev, buf);
	CHECK(rev.type == REVERB_OFF);
	Reverb_setPreDelay(&rev, 0.3f);
	Reverb_setDecay(&rev, 1.f);
	for(int ch = 1; ch <= 2; ch++)
	{
		int ok = 1;
		for(int b = 0; b < 20; b++)
		{
			for(int i = 0; i < 2 * BLOCK; i++) { in[i] = noise(32767); out[i] = ref[i] = noise(32767); }
			for(int i = 0; i < 2 * BLOCK; i += (ch == 2 ? 1 : 2))
			{
				int32_t v = ref[i] + in[i];
				ref[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
			}
			// dry/wet has no effect when off
			Reverb_process(&rev, in, out, BLOCK, (uint8_t)ch, 0.7f);
			if(memcmp(out, ref, sizeof(out)) != 0) ok = 0;
		}
		CHECK(ok);
	}
}

static void test_dry_passthrough(void)
{
	int16_t in[2 * BLOCK], out[2 * BLOCK];
	setup(0.2f, 0.8f, 0.5f, 1.f, REVERB_PLATE);
	int ok = 1;
	for(int b = 0; b < 50; b++)
	{
		for(int i = 0; i < 2 * BLOCK; i++) in[i] = noise(20000);
		runBlock(in, out, 2, 0.f);
		if(memcmp(in, out, sizeof(in)) != 0) ok = 0;
	}
	CHECK(ok);
}

static void test_accumulates_into_output(void)
{
	int16_t in[2 * BLOCK], out[2 * BLOCK];
	for(int i = 0; i < 2 * BLOCK; i++) { in[i] = 1000; out[i] = 30000; }
	setup(0.f, 0.5f, 0.5f, 0.f, REVERB_PLATE);
	Reverb_process(&rev, in, out, BLOCK, 2, 0.f);
	// 30000 + 1000 saturates
	int ok = 1;
	for(int i = 0; i < 2 * BLOCK; i++) if(out[i] != 31000) ok = 0;
	CHECK(ok);
	for(int i = 0; i < 2 * BLOCK; i++) { in[i] = 10000; out[i] = 30000; }
	Reverb_process(&rev, in, out, BLOCK, 2, 0.f);
	ok = 1;
	for(int i = 0; i < 2 * BLOCK; i++) if(out[i] != 32767) ok = 0;
	CHECK(ok);
}

static void test_mono_routing_writes_one_channel(void)
{
	int16_t in[2 * BLOCK], out[2 * BLOCK];
	setup(0.f, 0.5f, 0.5f, 0.5f, REVERB_PLATE);
	int otherTouched = 0;
	double e = 0;
	for(int b = 0; b < 40; b++)
	{
		for(int i = 0; i < 2 * BLOCK; i++) { in[i] = b < 4 ? noise(16000) : 0; out[i] = (i & 1) ? 1234 : 0; }
		Reverb_process(&rev, in, out, BLOCK, 1, 1.f);
		for(int i = 0; i < BLOCK; i++)
		{
			if(out[2 * i + 1] != 1234) otherTouched = 1;
			e += (double)out[2 * i] * out[2 * i];
		}
	}
	CHECK(!otherTouched);
	CHECK(e > 0);
}

// T30-style RT60 estimate: time from -5 dB to -35 dB below the energy peak
// after the excitation, doubled. Uses the brightest tone so the loop damping
// does not shorten the broadband decay.
static double measureRt60(uint8_t type, float decay, double *stereoCorr)
{
	int16_t in[2 * BLOCK], out[2 * BLOCK];
	setup(0.f, decay, 1.f, 0.f, type);
	const int blocks = 60 * FS / BLOCK;
	double peak = 0;
	int t5 = -1, t35 = -1;
	double sLL = 0, sRR = 0, sLR = 0;
	// smooth over 8 blocks (~12 ms)
	double win[8] = {0};
	for(int b = 0; b < blocks && t35 < 0; b++)
	{
		for(int i = 0; i < 2 * BLOCK; i++) in[i] = b < 8 ? noise(16000) : 0;
		runBlock(in, out, 2, 1.f);
		win[b & 7] = blockEnergy(out);
		double e = 0;
		for(int i = 0; i < 8; i++) e += win[i];
		for(int i = 0; i < BLOCK; i++)
		{
			sLL += (double)out[2 * i] * out[2 * i];
			sRR += (double)out[2 * i + 1] * out[2 * i + 1];
			sLR += (double)out[2 * i] * out[2 * i + 1];
		}
		if(e > peak) { peak = e; t5 = -1; }
		else if(t5 < 0 && e < peak * pow(10, -0.5)) t5 = b;
		else if(t5 >= 0 && e < peak * pow(10, -3.5)) t35 = b;
	}
	if(stereoCorr) *stereoCorr = sLR / sqrt(sLL * sRR + 1e-9);
	if(t35 < 0) return -1;
	return 2.0 * (t35 - t5) * (double)BLOCK / FS;
}

static void test_decay_times(void)
{
	// expected RT60 = 0.3 * 64^p seconds
	const float ps[] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
	for(int t = 0; t < 2; t++)
	{
		for(unsigned k = 0; k < sizeof(ps) / sizeof(ps[0]); k++)
		{
			double corr;
			const double measured = measureRt60(t ? REVERB_ECO : REVERB_PLATE, ps[k], &corr);
			const double expected = 0.3 * pow(64.0, ps[k]);
			printf("  %s decay=%.2f RT60 expected %5.2fs measured %5.2fs  L/R corr %+.2f\n",
					t ? "eco  " : "plate", ps[k], expected, measured, corr);
			// the shortest settings are bounded by the tank loop + diffusion build-up
			CHECK(measured > 0.6 * expected && measured < 1.5 * expected + 0.4);
			CHECK(fabs(corr) < 0.5);
		}
	}
}

static void test_wet_level(void)
{
	// noise burst through mid settings: wet RMS should be in the same ballpark
	// as dry RMS so a 50% mix does not jump in level
	int16_t in[2 * BLOCK], out[2 * BLOCK];
	for(int t = 0; t < 2; t++)
	{
		setup(0.1f, 0.5f, 0.5f, 0.3f, t ? REVERB_ECO : REVERB_PLATE);
		double eIn = 0, eOut = 0;
		for(int b = 0; b < 3 * FS / BLOCK; b++)
		{
			for(int i = 0; i < 2 * BLOCK; i++) in[i] = noise(8000);
			runBlock(in, out, 2, 1.f);
			if(b > FS / BLOCK)
			{
				eIn += blockEnergy(in);
				eOut += blockEnergy(out);
			}
		}
		const double ratio = sqrt(eOut / eIn);
		printf("  %s wet/dry RMS ratio (continuous noise, decay 0.5) %.2f\n", t ? "eco  " : "plate", ratio);
		CHECK(ratio > 0.3 && ratio < 3.0);
	}
}

static void test_stability_and_clear(void)
{
	int16_t in[2 * BLOCK], out[2 * BLOCK];
	for(int t = 0; t < 2; t++)
	{
		// loudest possible input, longest decay, brightest tone, full early
		setup(1.f, 1.f, 1.f, 1.f, t ? REVERB_ECO : REVERB_PLATE);
		for(int b = 0; b < 20 * FS / BLOCK; b++)
		{
			for(int i = 0; i < 2 * BLOCK; i++) in[i] = (b & 8) ? 32767 : noise(32767);
			runBlock(in, out, 2, 1.f);
		}
		CHECK(bufferFinite());
		CHECK(isfinite(rev.tank1) && isfinite(rev.tank2));
		CHECK(fabsf(rev.tank1) <= 4.f && fabsf(rev.tank2) <= 4.f);
		// the tail must keep ringing at maximum decay but must be decaying
		double eStart = 0, eLater = 0;
		for(int b = 0; b < 8 * FS / BLOCK; b++)
		{
			runBlock(NULL, out, 2, 1.f);
			if(b < FS / BLOCK) eStart += blockEnergy(out);
			else if(b >= 7 * FS / BLOCK) eLater += blockEnergy(out);
		}
		CHECK(eLater > 0);
		CHECK(eLater < 0.5 * eStart);
		CHECK(bufferFinite());

		Reverb_clear(&rev);
		double e = 0;
		for(int b = 0; b < 100; b++) { runBlock(NULL, out, 2, 1.f); e += blockEnergy(out); }
		CHECK(e == 0);
	}
}

static void test_type_change_clears(void)
{
	int16_t in[2 * BLOCK], out[2 * BLOCK];
	setup(0.f, 1.f, 0.5f, 0.5f, REVERB_PLATE);
	for(int b = 0; b < 100; b++)
	{
		for(int i = 0; i < 2 * BLOCK; i++) in[i] = noise(16000);
		runBlock(in, out, 2, 1.f);
	}
	Reverb_setType(&rev, REVERB_ECO);
	runBlock(NULL, out, 2, 1.f);
	CHECK(blockEnergy(out) == 0);
	// same type again: no clear
	for(int i = 0; i < 2 * BLOCK; i++) in[i] = noise(16000);
	runBlock(in, out, 2, 1.f);
	Reverb_setType(&rev, REVERB_ECO);
	CHECK(!rev.clearPending);
	// out-of-range type (unchecked kit byte) means off, never on
	Reverb_setType(&rev, 200);
	CHECK(rev.type == REVERB_OFF);
	Reverb_setType(&rev, REVERB_NUM_TYPES);
	CHECK(rev.type == REVERB_OFF);
}

static void test_tone_neutral(void)
{
	setup(0.f, 0.5f, 0.5f, 0.f, REVERB_PLATE);
	CHECK(rev.toneDry == 1.f && rev.toneLp == 0.f);
	Reverb_setTone(&rev, 0.f);
	CHECK(rev.toneDry == 0.f && rev.toneLp == 1.f);	// pure low-pass
	Reverb_setTone(&rev, 1.f);
	CHECK(rev.toneDry == 1.f && rev.toneLp == -1.f);	// pure high-pass
	// out-of-range parameters clamp
	Reverb_setDecay(&rev, 5.f);
	CHECK(rev.feedback > 0.f && rev.feedback <= 0.98f);
	Reverb_setPreDelay(&rev, 2.f);
	CHECK(rev.preDelay >= 2000 && rev.preDelay <= 2047);
}

int main(void)
{
	test_off_is_fx_off();
	test_dry_passthrough();
	test_accumulates_into_output();
	test_mono_routing_writes_one_channel();
	test_decay_times();
	test_wet_level();
	test_stability_and_clear();
	test_type_change_clears();
	test_tone_neutral();

	printf("test_reverb: %d checks, %d failures\n", checks, failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
