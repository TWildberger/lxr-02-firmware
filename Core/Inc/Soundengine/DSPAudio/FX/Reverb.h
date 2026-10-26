/*
 * Reverb.h
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

/*
 * Plate reverb for the FX bus (figure-8 tank after J. Dattorro, "Effect
 * Design Part 1", JAES 1997).
 *
 * All delay lines live in one caller-provided __fp16 buffer of
 * REVERB_BUFFER_SAMPLES samples (on the LXR this is the start of the shared
 * FX_BUFFER, which the delay also uses). This module has no HAL/CMSIS
 * dependency so it can be unit tested on the host (tests/host/test_reverb.c).
 *
 * Parameters (all 0..1):
 *   preDelay  1..46 ms pre-delay
 *   decay     RT60 0.3..19 s
 *   tone      0.5 = neutral; <0.5 darker (input low-pass + loop damping),
 *             >0.5 brighter (input high-pass, less damping)
 *   early     early reflection level
 * Type (PAR_FX_REVERB_TYPE):
 *   REVERB_OFF    dry passthrough, same as FX type "off". Default, and the
 *                 value found in older kits that were saved with FX type 5
 *                 while the reverb was compiled out: they keep sounding dry.
 *   REVERB_PLATE  tank runs at the full sample rate
 *   REVERB_ECO    tank runs at half rate (~half the CPU, <=11 kHz wet
 *                 bandwidth, delays twice as long -> larger room)
 */

#ifndef REVERBALGO_H_
#define REVERBALGO_H_

#include <stdint.h>

#ifndef REVERB_FS
#define REVERB_FS 44140.f	// must match REAL_FS in Soundengine/config.h
#endif

#define REVERB_BUFFER_SAMPLES 20480u

enum ReverbTypeEnum
{
	REVERB_OFF,
	REVERB_PLATE,
	REVERB_ECO,
	REVERB_NUM_TYPES
};

typedef struct ReverbStruct
{
	__fp16 *buf;
	uint32_t w;				// shared write index for all lines

	float tank1;			// output of tank half 1 (feeds half 2)
	float tank2;			// output of tank half 2 (feeds half 1)
	float damp1;			// loop damping filter states
	float damp2;
	float toneState;		// input tilt filter state

	float feedback;
	float dampCoef;
	float toneCoef;
	float toneDry;			// tilt: out = toneDry*x + toneLp*lp
	float toneLp;
	float earlyGain;
	uint32_t preDelay;		// in tank ticks

	float lfoPhase;
	float modA;				// current modulation excursion (ticks)
	float modB;

	float lastL;			// last wet output (eco interpolation)
	float lastR;

	float param[4];			// cached raw parameters, see Reverb_setParams
	uint8_t type;
	uint8_t clearPending;
} Reverb;

// buf must hold REVERB_BUFFER_SAMPLES samples and be 4-byte aligned
void Reverb_init(Reverb *r, __fp16 *buf);
// silence the tank and all lines (also done lazily on the next process call
// after init or a type change)
void Reverb_clear(Reverb *r);

/*
 * Process one block.
 * fxBuffer: interleaved stereo int16 FX bus input, size frames.
 * out:      interleaved stereo int16 destination; the result is added with
 *           saturation. channels == 2 writes out[2i] and out[2i+1],
 *           channels == 1 writes only out[2i] (mono routing).
 * size must be even.
 */
void Reverb_process(Reverb *r, const int16_t *fxBuffer, int16_t *out, uint16_t size, uint8_t channels, float dryWet);

void Reverb_setPreDelay(Reverb *r, float p);
void Reverb_setDecay(Reverb *r, float p);
void Reverb_setTone(Reverb *r, float p);
void Reverb_setEarly(Reverb *r, float p);
void Reverb_setType(Reverb *r, uint8_t type);

#endif /* REVERBALGO_H_ */
