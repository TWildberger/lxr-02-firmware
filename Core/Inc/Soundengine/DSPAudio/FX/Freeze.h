/*
 * Freeze.h
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

/*
 * Freeze / beat repeat for the FX bus (FX type "frz").
 *
 * The FX-bus input is recorded continuously into a caller-provided __fp16
 * ring buffer (on the LXR: the shared FX_BUFFER, 32768 samples). Stereo
 * routings record interleaved stereo (16384 frames, ~0.37 s), mono routings
 * record the left bus channel (32768 frames, ~0.74 s).
 *
 * While engaged, a tempo-synced slice that starts on the sequencer step grid
 * is repeated:
 *
 *   g            slice start: the latest grid point at or before the engage
 *                time (sequencer running), or the engage time (free running)
 *   g .. g+L     audio keeps passing live and is recorded
 *   g+kL         repeat k starts. A new read head is started X frames
 *                earlier, X frames before g, and crossfades from the previous
 *                head (or from the live signal for k = 1) over X frames, so
 *                the transient at g plays at full level and nothing clicks.
 *
 * Engage/release ramps the effect in and out over FREEZE_RAMP frames.
 * Output = dry + (wet - dry) * dryWet * engage, so with dry/wet at 100 the
 * repeats replace the dry bus; while released the output is a bit-exact
 * passthrough (same as FX type "off").
 *
 * Parameters (0..1, from FX P1..P4):
 *   rate   slice length 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T
 *          (8 equal zones). Lengths that do not fit the buffer at the current
 *          tempo fall back to the next shorter straight/triplet length.
 *   pitch  64 = original. 0..63: repeats play slower (down to one octave
 *          lower at 0). 65..127: every repeat glides further down (tape stop /
 *          roll-down; 127 = two octaves per repeat).
 *   decay  level of each repeat relative to the previous one, 0.25..1
 *   gate   audible part of each repeat, 1/16..1
 *
 * Control (Freeze_control, same values for panel, MIDI and SysEx):
 *   0          release
 *   1..8       engage at rate index 0..7
 *   9..127     engage at the kit rate (P1, follows P1 changes)
 *
 * The module has no HAL/CMSIS dependency (tests/host/test_freeze.c).
 * Freeze_process, Freeze_step and Freeze_reset run in the audio thread.
 * Freeze_control and the setters may be called from any context.
 */

#ifndef FREEZE_H_
#define FREEZE_H_

#include <stdint.h>

#define FREEZE_NUM_RATES	8u
#define FREEZE_XFADE		128u	// loop boundary crossfade, frames
#define FREEZE_RAMP			256u	// engage / release ramp, frames
#define FREEZE_MIN_LEN		(4u * FREEZE_XFADE)
#define FREEZE_GUARD		8u		// frames kept free between slice end and pre-roll
#define FREEZE_HIST			8u		// sequencer step timestamps kept (power of 2)
#define FREEZE_MAX_CHUNK	64u
#define FREEZE_MIN_SPEED	(1.f / 32.f)

#define FREEZE_CTRL_RELEASE	0u
#define FREEZE_CTRL_KIT		0xffu	// engagedRate: follow the kit rate (P1)

enum FreezeStateEnum
{
	FREEZE_IDLE,	// passthrough, recording
	FREEZE_ARMED,	// engaged, live audio until the first repeat starts
	FREEZE_LOOPING	// repeating the slice
};

typedef struct
{
	float value;
	float step;
	float target;
	uint32_t end;
	uint8_t active;
} FreezeRamp;

typedef struct
{
	float off;			// read position relative to base, frames
	uint32_t base;		// slice start g this head plays
	FreezeRamp gain;
	uint8_t on;
} FreezeHead;

typedef struct FreezeStruct
{
	__fp16 *buf;
	uint32_t bufSamples;	// power of 2
	float fs;

	uint8_t channels;		// capture layout, 1 or 2
	uint32_t mask;			// capacity in frames - 1
	uint32_t now;			// frame counter of the next frame to process
	uint32_t validFrom;		// first frame recorded since the last reset
	uint8_t writing;

	// tempo and step grid (Freeze_step can run in another interrupt)
	uint16_t bpm;
	uint8_t seqRunning;
	volatile uint32_t stepFrame[FREEZE_HIST];
	volatile uint32_t stepCount;
	volatile uint8_t stepPos;		// sequencer master step of the latest step
	volatile uint32_t stepDelta[2];	// last two step intervals, frames

	// parameters
	volatile uint8_t kitRate;
	volatile float speed0;		// static playback speed (pitch < 64)
	volatile float glideLog2;	// log2 of the per-repeat speed factor (pitch > 64)
	volatile float decay;
	volatile float gate;

	// request mailbox (control context -> audio thread)
	volatile uint8_t reqValue;
	volatile uint8_t reqSeq;
	uint8_t reqSeen;

	// engine state
	uint8_t state;
	uint8_t releasing;
	uint8_t engagedRate;	// requested rate index or FREEZE_CTRL_KIT
	uint8_t reqIdx;			// rate index asked for (before clamping)
	uint8_t rateIdx;		// rate index in use (after clamping)
	uint8_t curHead;
	uint8_t gateActive;
	uint32_t g;				// slice start frame
	uint32_t len;			// slice length L, frames
	uint32_t captured;		// recorded slice length once recording stopped
	uint32_t writeStop;
	uint32_t nextSpawn;
	uint32_t gateTime;
	uint32_t repeat;
	float level;
	float speed;
	float glideMul;

	FreezeRamp live;
	FreezeRamp mix;
	FreezeHead head[2];
} Freeze;

void Freeze_init(Freeze *f, __fp16 *buf, uint32_t bufSamples, float fs);
// forget the recording and stop immediately (audio thread)
void Freeze_reset(Freeze *f);

void Freeze_setRate(Freeze *f, float v);
void Freeze_setPitch(Freeze *f, float v);
void Freeze_setDecay(Freeze *f, float v);
void Freeze_setGate(Freeze *f, float v);
void Freeze_setTempo(Freeze *f, uint16_t bpm, uint8_t seqRunning);

// a sequencer step (16th) starts now; stepPos = master step in the pattern
void Freeze_step(Freeze *f, uint8_t stepPos);
// 0 = release, 1..8 = engage at rate 0..7, 9..127 = engage at the kit rate
void Freeze_control(Freeze *f, uint8_t value);

// in: interleaved stereo FX bus input. out: interleaved stereo output, the
// result is added with saturation. channels == 1: left samples only.
void Freeze_process(Freeze *f, const int16_t *in, int16_t *out, uint32_t frames, uint8_t channels, float dryWet);

// --- helpers, exposed for tests ---
// slice length of rate index idx in 16th steps
float Freeze_stepsPerSlice(uint8_t idx);
// slice length in frames for stepLen frames per 16th, clamped to the
// capacity (frames). *usedIdx receives the rate index actually used.
uint32_t Freeze_sliceLength(float stepLen, uint8_t idx, uint32_t capacity, uint8_t *usedIdx);
uint8_t Freeze_rateIndex(float v);

#endif /* FREEZE_H_ */
