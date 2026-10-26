/*
 * CpuMeter.h
 *
 * Audio render CPU load meter, see docs/performance.md.
 *
 * The render of one audio block (OUTPUT_DMA_SIZE stereo frames) is timed with
 * the DWT cycle counter and compared with the time one block lasts at the
 * sample rate. The bookkeeping in CpuMeter.c is hardware independent, so it is
 * also built by the host unit tests in tests/host (with CPU_METER_NO_HW).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */
#ifndef CPU_METER_H_
#define CPU_METER_H_

#include <stdint.h>

// blocks averaged for the "now" values: 256 x 64 frames = 0.37 s at 44.14 kHz
#define CPU_METER_WINDOW_SHIFT	8
#define CPU_METER_WINDOW_BLOCKS	(1u << CPU_METER_WINDOW_SHIFT)

// largest value reported over SysEx (u14)
#define CPU_METER_MAX_REPORT	16383

// 1: also split the render time into voices / mixer / FX (about 120 bytes ITCM)
#ifndef CPU_METER_SECTIONS
#define CPU_METER_SECTIONS		1
#endif

enum CpuMeterSection {
	CPU_SECTION_VOICES = 0,	// LFOs, modulation, voice control and sample rendering
	CPU_SECTION_MIXER,		// buffer clears, decimation, pan/routing into the outputs
	CPU_SECTION_FX,			// FxBus_process
	CPU_NUM_SECTIONS
};

typedef struct {
	uint32_t budget;							// CPU cycles per audio block (100 %)

	// written by the render path
	uint32_t lapStart;							// cycle count at the last probe
	uint32_t blockSection[CPU_NUM_SECTIONS];	// sections of the block being rendered
	uint64_t sumTotal;
	uint64_t sumSection[CPU_NUM_SECTIONS];
	uint32_t blocks;
	volatile uint32_t peakTotal;				// worst block since reset
	volatile uint32_t underruns;				// DAC wanted a block that was not ready

	// published once per window
	volatile uint32_t avgTotal;
	volatile uint32_t avgSection[CPU_NUM_SECTIONS];
	volatile uint32_t windows;					// number of published windows
} CpuMeter;

// all load values in 1/10 % (permille of the block budget), clamped to CPU_METER_MAX_REPORT
typedef struct {
	uint16_t now;								// average of the last window
	uint16_t peak;								// worst single block since reset
	uint16_t section[CPU_NUM_SECTIONS];			// averages of the last window
	uint16_t underruns;							// since reset, saturating
} CpuMeterReport;

extern CpuMeter cpuMeter;

void cpuMeter_init(CpuMeter *m, uint32_t cyclesPerBlock);
// render path: account one block that took totalCycles (sections from blockSection)
void cpuMeter_blockDone(CpuMeter *m, uint32_t totalCycles);
// clears peak and underrun counter
void cpuMeter_resetPeak(CpuMeter *m);
void cpuMeter_report(const CpuMeter *m, CpuMeterReport *out);
// cycles -> 1/10 % of budget, rounded, clamped to CPU_METER_MAX_REPORT
uint16_t cpuMeter_permille(uint32_t cycles, uint32_t budget);

//---------------------------------------------------------------------------
// Probes for the render path (firmware only). They read the DWT cycle counter
// (enabled by DWT_Delay_Init):
//
//   CPU_METER_BEGIN();
//   ... voice work ...   CPU_METER_LAP(CPU_SECTION_VOICES);
//   ... mixing ...       CPU_METER_LAP(CPU_SECTION_MIXER);
//
// Each LAP charges the time since the previous BEGIN/LAP to a section of the
// block; cpuMeter_blockDone() collects and clears the sections. A LAP is a
// call to a small ITCM function to keep the ITCM footprint of the probes low.
#ifndef CPU_METER_NO_HW
#include "stm32f7xx.h"

#define CPU_METER_NOW()		(DWT->CYCCNT)

#if CPU_METER_SECTIONS
void cpuMeter_lap(uint32_t section);
#define CPU_METER_BEGIN()		(cpuMeter.lapStart = CPU_METER_NOW())
#define CPU_METER_LAP(section)	cpuMeter_lap(section)
#else
#define CPU_METER_BEGIN()		do {} while(0)
#define CPU_METER_LAP(section)	do {} while(0)
#endif

#endif // CPU_METER_NO_HW

#endif /* CPU_METER_H_ */
