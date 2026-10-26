/*
 * FxBus.c
 *
 *  Created on: 26.08.2020
 *      Author: julian
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */


#include "Soundengine/FxBus.h"
#include "Soundengine/DSPAudio/BufferTools.h"
#include "Soundengine/DSPAudio/mixer.h"
#include "Soundengine/DSPAudio/FX/FxRamBuffer.h"
#include "Soundengine/Sequencer/sequencer.h"
INDTCMZ FxBus fxBus;

void FxBus_init()
{
	fxBus.dryWet = 0;
	fxBus.audioRouting = 0;
	fxBus.fxType = FX_OFF;
	fxBus.lastFxType = FX_OFF;
	fxBus.bufferOwner = FX_OFF;

	fxBus.delay.delayType = DELAY_NORMAL;

	Distortion_init(&fxBus.distortion);
	Ringmod_init(&fxBus.ringmod);
	Compressor_init(&fxBus.compressor);
	Delay_init(&fxBus.delay);



#if USE_REVERB
	Reverb_init(&fxBus.reverb, FX_BUFFER);
#endif
	Freeze_init(&fxBus.freeze, FX_BUFFER, FX_BUFFER_SIZE + 1, REAL_FS);
}

// Not in ITCM (the freeze code isn't either)
static __attribute__((noinline)) void FxBus_processFreeze(int16_t *fxBuffer, int16_t *out, uint32_t size, uint8_t channels)
{
	Freeze_setTempo(&fxBus.freeze, seq_getBpm(), seq_isRunning());
	Freeze_process(&fxBus.freeze, fxBuffer, out, size, channels, fxBus.dryWet);
}

IN_ITCM_RAM void FxBus_process(int16_t *fxBuffer, int16_t* outL,int16_t* outR,int16_t* outL2, int16_t* outR2, uint32_t size)
{
	int16_t* out;
	int16_t* out2 = NULL;

	if(fxBus.fxType != fxBus.lastFxType) FxBus_handleFxChange();

	// < MIXER_ROUTING_DAC1_L -> stereo 1 or 2
	const uint8_t channels = fxBus.audioRouting < MIXER_ROUTING_DAC1_L ? 2 : 1;
	switch(fxBus.audioRouting)
	{
	default:
	case MIXER_ROUTING_DAC1_STEREO:
		out = outL2;
		out2 = outR2;
		break;
	case MIXER_ROUTING_DAC2_STEREO:
		out = outL;
		out2 = outR;
		break;
	case MIXER_ROUTING_DAC1_L:
		out = outL2;
		break;
	case MIXER_ROUTING_DAC1_R:
		out = outR2;
		break;
	case MIXER_ROUTING_DAC2_L:
		out = outL;
		break;
	case MIXER_ROUTING_DAC2_R:
		out = outR;
		break;

	}

	switch(fxBus.fxType)
	{
	default:
	case FX_OFF:
	{
		if(channels == 2)
		{
			int32_t* stereoIn = (int32_t*)fxBuffer;
			int32_t* stereoOut = (int32_t*)out;

			for(uint8_t i=0;i<size;i++)
			{
				//add stereo samples using SIMD
				*stereoOut = __QADD16(*stereoOut, *stereoIn);
				stereoOut++;
				stereoIn++;
			}
		}
		else
		{
			int32_t* in = (int32_t*)fxBuffer;
			for(uint8_t i=0;i<size;i++)
			{
				*out =  __QADD16(*out, *in);
				out +=2;
				in++;

			}
		}
	}
	break;

	case FX_DISTORTION:
		Distortion_process(&fxBus.distortion, fxBuffer, out, out2, size, channels, fxBus.dryWet);
		break;

	case FX_RING:
		Ringmod_process(&fxBus.ringmod, fxBuffer, out, out2, size, channels, fxBus.dryWet);
		break;

	case FX_COMPRESSOR:
		Compressor_process(&fxBus.compressor, fxBuffer, out, out2, size, channels, fxBus.dryWet);
		break;

	case FX_DELAY:
		if(fxBus.delay.delayType == DELAY_NORMAL)
		{
			Delay_process(&fxBus.delay, fxBuffer, out, out2, size, channels, fxBus.dryWet);
		}
		else
		{
			Delay_processPingPong(&fxBus.delay, fxBuffer, out, out2, size, channels, fxBus.dryWet);
		}
		break;
#if USE_REVERB
	case FX_REVERB:
		Reverb_process(&fxBus.reverb, fxBuffer, out, size, channels, fxBus.dryWet);
		break;
#endif
	case FX_FREEZE:
		FxBus_processFreeze(fxBuffer, out, size, channels);
		break;

	}

}

// FX types that keep audio in the shared FX_BUFFER
static uint8_t FxBus_usesFxBuffer(uint8_t type)
{
	switch(type)
	{
	case FX_DELAY:
#if USE_REVERB
	case FX_REVERB:
#endif
	case FX_FREEZE:
		return 1;
	default:
		return 0;
	}
}

// Not in ITCM: only runs once per FX type change.
__attribute__((noinline)) void FxBus_handleFxChange()
{
	const uint8_t type = fxBus.fxType;

	if(FxBus_usesFxBuffer(type) && fxBus.bufferOwner != type)
	{
		// another FX left its data in the shared buffer: start from silence.
		// The freeze never plays what it hasn't recorded: no clear needed.
		if(fxBus.bufferOwner != FX_OFF && type != FX_FREEZE)
			FxRamBuffer_clear();
		fxBus.bufferOwner = type;
	}
#if USE_REVERB
	// don't resume a stale tail when the reverb is selected again
	if(type == FX_REVERB)
		Reverb_clear(&fxBus.reverb);
#endif
	// a new recording starts, an engaged freeze is released
	if(type == FX_FREEZE)
		Freeze_reset(&fxBus.freeze);
	fxBus.lastFxType = type;
}

void FxBus_prepareFxChange()
{
	//memset(FX_BUFFER, 0, FX_BUFFER_SIZE*sizeof(__fp16) );
//	Ringmod_resetPhase(&fxBus.ringmod);
}

// control rate only (parameter changes, LFO at most once per block): kept
// out of the nearly full ITCM
void FxBus_updateParameter()
{
	switch(fxBus.fxType)
	{
	case FX_OFF:

		break;

	case FX_DISTORTION:
		Distortion_setDrive(&fxBus.distortion, fxBus.parameter[0] );
		Distortion_setParam1(&fxBus.distortion, fxBus.parameter[1] );
		Distortion_setParam2(&fxBus.distortion, fxBus.parameter[2] );
		Distortion_setParam3(&fxBus.distortion, fxBus.parameter[3] );
		break;

	case FX_RING:
		Ringmod_setFreq(&fxBus.ringmod, fxBus.parameter[0] );
		//Ringmod_setWave(&fxBus.ringmod, fxBus.parameter[1] );
		break;

	case FX_COMPRESSOR:

		Compressor_setAttack(&fxBus.compressor , fxBus.parameter[0]);
		Compressor_setRelease(&fxBus.compressor , fxBus.parameter[1]);
		Compressor_setTreshold(&fxBus.compressor , fxBus.parameter[2]);
		Compressor_setGain(&fxBus.compressor , fxBus.parameter[3]);
		break;

	case FX_DELAY:
		Delay_setRange(&fxBus.delay, fxBus.parameter[1]);
		Delay_setDelayTime(&fxBus.delay, fxBus.parameter[0]);
		Delay_setFilter(&fxBus.delay, fxBus.parameter[2]);
		Delay_setFeedback(&fxBus.delay, fxBus.parameter[3]);

		break;
#if USE_REVERB
	case FX_REVERB:
		Reverb_setPreDelay(&fxBus.reverb, fxBus.parameter[0]);
		Reverb_setDecay(&fxBus.reverb, fxBus.parameter[1]);
		Reverb_setTone(&fxBus.reverb, fxBus.parameter[2]);
		Reverb_setEarly(&fxBus.reverb, fxBus.parameter[3]);
		break;
#endif
	case FX_FREEZE:
		Freeze_setRate(&fxBus.freeze, fxBus.parameter[0]);
		Freeze_setPitch(&fxBus.freeze, fxBus.parameter[1]);
		Freeze_setDecay(&fxBus.freeze, fxBus.parameter[2]);
		Freeze_setGate(&fxBus.freeze, fxBus.parameter[3]);
		break;
	default:
		break;
	}
}

uint8_t FxBus_freezeControl(uint8_t value)
{
	if(fxBus.fxType != FX_FREEZE)
		return 0;
	Freeze_control(&fxBus.freeze, value);
	return 1;
}

void FxBus_seqStep(uint8_t stepPos)
{
	if(fxBus.fxType == FX_FREEZE)
		Freeze_step(&fxBus.freeze, stepPos);
}

void FxBus_setParameter(uint8_t idx, float value)
{
	fxBus.parameter[idx] = value;

	FxBus_updateParameter();


}


