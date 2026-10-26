/*
 * CpuMeter.c
 *
 * Audio render CPU load meter bookkeeping, see CpuMeter.h and
 * docs/performance.md. Hardware independent.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */
#include "Soundengine/CpuMeter.h"

#include <string.h>

CpuMeter cpuMeter;

#if !defined(CPU_METER_NO_HW) && CPU_METER_SECTIONS
//---------------------------------------------------------------------------
__attribute__((section(".itcm_text"), noinline)) void cpuMeter_lap(uint32_t section)
{
	const uint32_t now = CPU_METER_NOW();
	cpuMeter.blockSection[section] += now - cpuMeter.lapStart;
	cpuMeter.lapStart = now;
}
#endif

//---------------------------------------------------------------------------
void cpuMeter_init(CpuMeter *m, uint32_t cyclesPerBlock)
{
	memset(m, 0, sizeof(*m));
	m->budget = cyclesPerBlock ? cyclesPerBlock : 1;
}
//---------------------------------------------------------------------------
void cpuMeter_blockDone(CpuMeter *m, uint32_t totalCycles)
{
	m->sumTotal += totalCycles;
	for(uint8_t i = 0; i < CPU_NUM_SECTIONS; i++)
	{
		m->sumSection[i] += m->blockSection[i];
		m->blockSection[i] = 0;
	}
	if(totalCycles > m->peakTotal)
		m->peakTotal = totalCycles;

	if(++m->blocks >= CPU_METER_WINDOW_BLOCKS)
	{
		m->avgTotal = (uint32_t)(m->sumTotal >> CPU_METER_WINDOW_SHIFT);
		m->sumTotal = 0;
		for(uint8_t i = 0; i < CPU_NUM_SECTIONS; i++)
		{
			m->avgSection[i] = (uint32_t)(m->sumSection[i] >> CPU_METER_WINDOW_SHIFT);
			m->sumSection[i] = 0;
		}
		m->blocks = 0;
		m->windows++;
	}
}
//---------------------------------------------------------------------------
void cpuMeter_resetPeak(CpuMeter *m)
{
	m->peakTotal = 0;
	m->underruns = 0;
}
//---------------------------------------------------------------------------
uint16_t cpuMeter_permille(uint32_t cycles, uint32_t budget)
{
	if(budget == 0) return 0;
	const uint64_t v = ((uint64_t)cycles * 1000u + budget / 2) / budget;
	return v > CPU_METER_MAX_REPORT ? CPU_METER_MAX_REPORT : (uint16_t)v;
}
//---------------------------------------------------------------------------
void cpuMeter_report(const CpuMeter *m, CpuMeterReport *out)
{
	out->now = cpuMeter_permille(m->avgTotal, m->budget);
	out->peak = cpuMeter_permille(m->peakTotal, m->budget);
	for(uint8_t i = 0; i < CPU_NUM_SECTIONS; i++)
		out->section[i] = cpuMeter_permille(m->avgSection[i], m->budget);
	const uint32_t u = m->underruns;
	out->underruns = u > CPU_METER_MAX_REPORT ? CPU_METER_MAX_REPORT : (uint16_t)u;
}
