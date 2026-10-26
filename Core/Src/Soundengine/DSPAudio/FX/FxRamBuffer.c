/*
 * FxRamBuffer.c
 *
 *  Created on: 30.08.2020
 *      Author: julian
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */


#include "Soundengine/DSPAudio/FX/FxRamBuffer.h"

INDTCMZ __attribute__((aligned(8))) __fp16 FX_BUFFER[FX_BUFFER_SIZE+1];

// newlib-nano's memset stores single bytes (~200k cycles for 64 KB), which
// would overrun an audio block. Keep GCC from turning this back into memset.
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void FxRamBuffer_clear()
{
	uint32_t *p = (uint32_t*)(void*)FX_BUFFER;
	for(uint32_t i = 0; i < sizeof(FX_BUFFER) / 4; i += 4)
	{
		p[i] = 0;
		p[i + 1] = 0;
		p[i + 2] = 0;
		p[i + 3] = 0;
	}
}

