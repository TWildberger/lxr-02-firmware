#include "Soundengine/Sequencer/SeqCondition.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	uint8_t packed[SEQ_CONDITION_BYTES] = {0};
	uint8_t original[SEQ_CONDITION_BYTES];

	for(uint16_t step = 0; step < 448; step++)
	{
		const uint8_t condition = (uint8_t)(step % SEQ_CONDITION_COUNT);
		seq_condition_set(packed, step, condition);
		assert(seq_condition_get(packed, step) == condition);
	}

	memcpy(original, packed, sizeof(packed));
	for(uint16_t step = 0; step < 448; step++)
		assert(seq_condition_get(packed, step) == step % SEQ_CONDITION_COUNT);
	assert(memcmp(original, packed, sizeof(packed)) == 0);

	assert(seq_condition_allows(SEQ_CONDITION_ALWAYS, 0));
	assert(!seq_condition_allows(SEQ_CONDITION_EVERY_2ND, 0));
	assert(seq_condition_allows(SEQ_CONDITION_EVERY_2ND, 1));
	assert(seq_condition_allows(SEQ_CONDITION_EVERY_3RD, 2));
	assert(seq_condition_allows(SEQ_CONDITION_EVERY_4TH, 3));
	assert(seq_condition_allows(SEQ_CONDITION_FIRST_2, 0));
	assert(!seq_condition_allows(SEQ_CONDITION_FIRST_2, 1));
	assert(seq_condition_allows(SEQ_CONDITION_FIRST_3, 3));
	assert(seq_condition_allows(SEQ_CONDITION_FIRST_4, 4));
	assert(!seq_condition_allows(SEQ_CONDITION_AFTER_FIRST, 0));
	assert(seq_condition_allows(SEQ_CONDITION_AFTER_FIRST, 1));

	assert(seq_probability_allows(0, 0));
	assert(!seq_probability_allows(0, 1));
	assert(seq_probability_allows(50, 50));
	assert(!seq_probability_allows(50, 51));
	assert(seq_probability_allows(100, 100));
	assert(seq_probability_allows(127, 127));
	assert(!seq_probability_allows(100, 101));

	puts("sequencer condition tests passed");
	return 0;
}
