#include "Soundengine/Sequencer/SeqCondition.h"

uint8_t seq_condition_get(const uint8_t *packed, uint16_t step)
{
	const uint16_t bit = (uint16_t)(step * 3u);
	const uint16_t byte = bit >> 3;
	const uint8_t shift = bit & 7u;

	if(shift <= 5u)
		return (uint8_t)((packed[byte] >> shift) & 0x07u);

	return (uint8_t)(((packed[byte] >> shift)
			| (packed[byte + 1u] << (8u - shift))) & 0x07u);
}

void seq_condition_set(uint8_t *packed, uint16_t step, uint8_t condition)
{
	const uint16_t bit = (uint16_t)(step * 3u);
	const uint16_t byte = bit >> 3;
	const uint8_t shift = bit & 7u;
	const uint16_t mask = (uint16_t)0x07u << shift;
	uint16_t value = (uint16_t)(condition & 0x07u) << shift;

	if(shift <= 5u)
	{
		packed[byte] = (uint8_t)((packed[byte] & (uint8_t)~mask) | value);
		return;
	}

	packed[byte] = (uint8_t)((packed[byte] & (uint8_t)~(0xFFu << shift))
			| value);
	packed[byte + 1u] = (uint8_t)((packed[byte + 1u]
			& (uint8_t)~(mask >> 8)) | (value >> 8));
}

uint8_t seq_condition_allows(uint8_t condition, uint16_t cycle)
{
	switch(condition)
	{
	case SEQ_CONDITION_EVERY_2ND:
		return (cycle % 2u) == 1u;
	case SEQ_CONDITION_EVERY_3RD:
		return (cycle % 3u) == 2u;
	case SEQ_CONDITION_EVERY_4TH:
		return (cycle % 4u) == 3u;
	case SEQ_CONDITION_FIRST_2:
		return (cycle % 2u) == 0u;
	case SEQ_CONDITION_FIRST_3:
		return (cycle % 3u) == 0u;
	case SEQ_CONDITION_FIRST_4:
		return (cycle % 4u) == 0u;
	case SEQ_CONDITION_AFTER_FIRST:
		return cycle > 0;
	default:
		return 1;
	}
}

uint8_t seq_probability_allows(uint8_t probability, uint8_t roll)
{
	return roll <= probability;
}
