/* Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef FEEDBACK_FM_H
#define FEEDBACK_FM_H

#include <stdint.h>

enum { DRUM_ENGINE_LEGACY = 0, DRUM_ENGINE_FEEDBACK_FM = 1 };

static inline int drumEngine_isFeedback(uint8_t engine)
{
    return engine == DRUM_ENGINE_FEEDBACK_FM;
}

typedef struct {
    uint32_t carrierPhase;
    uint32_t modPhase;
    float previousMod;
    float envelope;
    float decayControl;
    float decay;
} FeedbackFm;

void feedbackFm_init(FeedbackFm *fm);
void feedbackFm_trigger(FeedbackFm *fm);
void feedbackFm_setDecay(FeedbackFm *fm, float control);
/* Uses the existing 4096-point, signed sine table with its endpoint at 4096. */
void feedbackFm_render(FeedbackFm *fm, const int16_t *sine, int16_t *out,
                       uint8_t size, float carrierHz,
                       float modHz, float index, float feedback);

#endif
