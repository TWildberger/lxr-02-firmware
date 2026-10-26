/*
 * Host-side unit tests for the CPU meter bookkeeping
 * (Core/Src/Soundengine/CpuMeter.c). Run with: make -C tests/host
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <string.h>

#include "Soundengine/CpuMeter.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
		checks++; \
		if(!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
	} while(0)

// 216 MHz * 64 frames / 44140 Hz
#define BUDGET 313185u

static void feed(CpuMeter *m, uint32_t blocks, uint32_t total, uint32_t voices, uint32_t mixer, uint32_t fx)
{
	for(uint32_t i = 0; i < blocks; i++)
	{
		m->blockSection[CPU_SECTION_VOICES] = voices;
		m->blockSection[CPU_SECTION_MIXER] = mixer;
		m->blockSection[CPU_SECTION_FX] = fx;
		cpuMeter_blockDone(m, total);
	}
}
//---------------------------------------------------------------------------
static void test_permille(void)
{
	CHECK(cpuMeter_permille(0, BUDGET) == 0);
	CHECK(cpuMeter_permille(BUDGET, BUDGET) == 1000);
	CHECK(cpuMeter_permille(BUDGET / 2, BUDGET) == 500);
	CHECK(cpuMeter_permille(BUDGET / 4, BUDGET) == 250);
	// rounding to nearest
	CHECK(cpuMeter_permille(1, 3000) == 0);
	CHECK(cpuMeter_permille(2, 3000) == 1);
	CHECK(cpuMeter_permille(3, 2000) == 2);
	// overload above 100 % is reported, clamped to u14
	CHECK(cpuMeter_permille(2 * BUDGET, BUDGET) == 2000);
	CHECK(cpuMeter_permille(100 * BUDGET, BUDGET) == CPU_METER_MAX_REPORT);
	CHECK(cpuMeter_permille(0xFFFFFFFFu, 1) == CPU_METER_MAX_REPORT);
	CHECK(cpuMeter_permille(1234, 0) == 0);
}
//---------------------------------------------------------------------------
static void test_init(void)
{
	CpuMeter m;
	memset(&m, 0xAB, sizeof(m));
	cpuMeter_init(&m, BUDGET);
	CHECK(m.budget == BUDGET);
	CHECK(m.sumTotal == 0 && m.blocks == 0 && m.peakTotal == 0 && m.avgTotal == 0);
	CHECK(m.underruns == 0 && m.windows == 0);
	CpuMeterReport r;
	cpuMeter_report(&m, &r);
	CHECK(r.now == 0 && r.peak == 0 && r.underruns == 0);
	CHECK(r.section[0] == 0 && r.section[1] == 0 && r.section[2] == 0);

	cpuMeter_init(&m, 0);
	CHECK(m.budget == 1);
}
//---------------------------------------------------------------------------
static void test_window(void)
{
	CpuMeter m;
	cpuMeter_init(&m, BUDGET);

	// nothing is published before a full window
	feed(&m, CPU_METER_WINDOW_BLOCKS - 1, BUDGET / 4, 0, 0, 0);
	CHECK(m.avgTotal == 0 && m.windows == 0);
	feed(&m, 1, BUDGET / 4, 0, 0, 0);
	CHECK(m.windows == 1);
	CHECK(m.avgTotal == BUDGET / 4);
	CHECK(m.blocks == 0 && m.sumTotal == 0);

	// average over the window, not the last block
	feed(&m, CPU_METER_WINDOW_BLOCKS / 2, 1000, 0, 0, 0);
	feed(&m, CPU_METER_WINDOW_BLOCKS / 2, 3000, 0, 0, 0);
	CHECK(m.windows == 2);
	CHECK(m.avgTotal == 2000);

	CpuMeterReport r;
	feed(&m, CPU_METER_WINDOW_BLOCKS, BUDGET / 2, 0, 0, 0);
	cpuMeter_report(&m, &r);
	CHECK(r.now == 500);

	// the sums must not overflow with blocks far over budget
	feed(&m, CPU_METER_WINDOW_BLOCKS, 0xFFFFFFFFu, 0, 0, 0);
	CHECK(m.avgTotal == 0xFFFFFFFFu);
	cpuMeter_report(&m, &r);
	CHECK(r.now == CPU_METER_MAX_REPORT);
}
//---------------------------------------------------------------------------
static void test_peak(void)
{
	CpuMeter m;
	cpuMeter_init(&m, BUDGET);
	feed(&m, 10, 1000, 0, 0, 0);
	feed(&m, 1, BUDGET * 3 / 4, 0, 0, 0);
	feed(&m, 10, 1000, 0, 0, 0);
	CHECK(m.peakTotal == BUDGET * 3 / 4);

	// held across windows until reset
	feed(&m, 3 * CPU_METER_WINDOW_BLOCKS, 1000, 0, 0, 0);
	CpuMeterReport r;
	cpuMeter_report(&m, &r);
	CHECK(r.peak == 750);
	CHECK(r.now == cpuMeter_permille(1000, BUDGET));

	m.underruns = 7;
	cpuMeter_report(&m, &r);
	CHECK(r.underruns == 7);
	cpuMeter_resetPeak(&m);
	cpuMeter_report(&m, &r);
	CHECK(r.peak == 0 && r.underruns == 0);
	// the average is not affected by the reset
	CHECK(r.now == cpuMeter_permille(1000, BUDGET));
	feed(&m, 1, 2000, 0, 0, 0);
	CHECK(m.peakTotal == 2000);

	m.underruns = 100000;
	cpuMeter_report(&m, &r);
	CHECK(r.underruns == CPU_METER_MAX_REPORT);
}
//---------------------------------------------------------------------------
static void test_sections(void)
{
	CpuMeter m;
	cpuMeter_init(&m, BUDGET);
	feed(&m, CPU_METER_WINDOW_BLOCKS, 100000, 60000, 15000, 25000);
	// blockDone consumes the per-block section times
	CHECK(m.blockSection[0] == 0 && m.blockSection[1] == 0 && m.blockSection[2] == 0);
	CHECK(m.avgSection[CPU_SECTION_VOICES] == 60000);
	CHECK(m.avgSection[CPU_SECTION_MIXER] == 15000);
	CHECK(m.avgSection[CPU_SECTION_FX] == 25000);

	CpuMeterReport r;
	cpuMeter_report(&m, &r);
	CHECK(r.now == cpuMeter_permille(100000, BUDGET));
	CHECK(r.section[CPU_SECTION_VOICES] == cpuMeter_permille(60000, BUDGET));
	CHECK(r.section[CPU_SECTION_MIXER] == cpuMeter_permille(15000, BUDGET));
	CHECK(r.section[CPU_SECTION_FX] == cpuMeter_permille(25000, BUDGET));
	CHECK(r.section[CPU_SECTION_VOICES] == 192);	// 19.2 %

	// a block without probes (sections disabled) leaves the sections at 0
	feed(&m, CPU_METER_WINDOW_BLOCKS, 100000, 0, 0, 0);
	CHECK(m.avgSection[0] == 0 && m.avgSection[1] == 0 && m.avgSection[2] == 0);
}
//---------------------------------------------------------------------------
int main(void)
{
	test_permille();
	test_init();
	test_window();
	test_peak();
	test_sections();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
