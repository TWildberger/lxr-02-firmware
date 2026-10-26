/* Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "Soundengine/DSPAudio/FeedbackFm.h"

static float unit(float x)
{
    return x > 0.f ? (x < 1.f ? x : 1.f) : 0.f;
}

static float sineAt(const int16_t *sine, uint32_t phase)
{
    const uint32_t i = phase >> 20;
    const float frac = (phase & 0xfffff) * (1.f / 1048576.f);
    return (sine[i] + frac * (sine[i + 1] - sine[i])) * (1.f / 32768.f);
}

void feedbackFm_setDecay(FeedbackFm *fm, float control)
{
    fm->decayControl = unit(control);
    /* Independent timbre T60: 3 ms to 2 s at the hardware's 44140 Hz. */
    const float seconds = .003f + 1.997f * fm->decayControl * fm->decayControl;
    fm->decay = 1.f - 6.907755f / (44140.f * seconds);
}

void feedbackFm_init(FeedbackFm *fm)
{
    *fm = (FeedbackFm){0};
    feedbackFm_setDecay(fm, 0.f);
}

void feedbackFm_trigger(FeedbackFm *fm)
{
    /* The firmware's sine table starts at -1; a quarter cycle is its zero crossing. */
    fm->carrierPhase = 0x40000000u;
    fm->modPhase = 0x40000000u;
    fm->previousMod = 0.f;
    fm->envelope = 1.f;
}

void feedbackFm_render(FeedbackFm *fm, const int16_t *sine, int16_t *out,
                       uint8_t size, float carrierHz,
                       float modHz, float index, float feedback)
{
    const float sampleRate = 44140.f;
    const float limit = sampleRate * .2f;
    carrierHz = unit(carrierHz / limit) * limit;
    modHz = unit(modHz / limit) * limit;
    const uint32_t carrierInc = (uint32_t)(carrierHz / sampleRate * 4294967296.f);
    const uint32_t modInc = (uint32_t)(modHz / sampleRate * 4294967296.f);
    const float highest = carrierHz > modHz ? carrierHz : modHz;
    /* Taper phase deviation in the upper octave; this is not an oversampled engine. */
    const float taper = highest > sampleRate * .1f ? sampleRate * .1f / highest : 1.f;
    const float depth = unit(index) * taper * 1932735232.f; /* < half a cycle */
    const float fb = unit(feedback) * taper * 858993408.f;
    for (uint8_t i = 0; i < size; ++i) {
        const int32_t fbPhase = (int32_t)(fm->previousMod * fb * fm->envelope);
        const float mod = sineAt(sine, fm->modPhase + (uint32_t)fbPhase);
        fm->previousMod = .5f * (fm->previousMod + mod);
        const int32_t deviation = (int32_t)(mod * depth * fm->envelope);
        out[i] = (int16_t)(24000.f * sineAt(sine, fm->carrierPhase + (uint32_t)deviation));
        fm->modPhase += modInc;
        fm->carrierPhase += carrierInc;
        fm->envelope *= fm->decay;
        if (fm->envelope < .000001f) fm->envelope = 0.f;
    }
}
