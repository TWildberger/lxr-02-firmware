/*
 * Packed per-step trig conditions and probability helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef SEQ_CONDITION_H_
#define SEQ_CONDITION_H_

#include <stdint.h>

#define SEQ_CONDITION_COUNT 8
#define SEQ_CONDITION_BYTES 168
#define SEQ_CONDITION_TRACK_BYTES 24

enum SeqCondition
{
	SEQ_CONDITION_ALWAYS = 0,
	SEQ_CONDITION_EVERY_2ND,
	SEQ_CONDITION_EVERY_3RD,
	SEQ_CONDITION_EVERY_4TH,
	SEQ_CONDITION_FIRST_2,
	SEQ_CONDITION_FIRST_3,
	SEQ_CONDITION_FIRST_4,
	SEQ_CONDITION_AFTER_FIRST,
};

uint8_t seq_condition_get(const uint8_t *packed, uint16_t step);
void seq_condition_set(uint8_t *packed, uint16_t step, uint8_t condition);
uint8_t seq_condition_allows(uint8_t condition, uint16_t cycle);
uint8_t seq_probability_allows(uint8_t probability, uint8_t roll);

#endif
