/* Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef KIT_ENGINE_FORMAT_H
#define KIT_ENGINE_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KIT_ENGINE_TAG "EngV01"
#define KIT_ENGINE_TAG_SIZE 6
#define KIT_ENGINE_FIRST 221
#define KIT_ENGINE_DECAY_FIRST 224

/* Only a versioned container may interpret formerly reserved bytes as engines. */
static inline void kitEngine_migrate(uint8_t *params, size_t count,
                                     const uint8_t *tag, size_t tagSize)
{
    const int versioned = tagSize == KIT_ENGINE_TAG_SIZE &&
                          memcmp(tag, KIT_ENGINE_TAG, KIT_ENGINE_TAG_SIZE) == 0;
    for (size_t i = KIT_ENGINE_FIRST; i < KIT_ENGINE_FIRST + 3 && i < count; ++i) {
        if (!versioned || params[i] > 1) params[i] = 0;
    }
    for (size_t i = KIT_ENGINE_DECAY_FIRST; i < KIT_ENGINE_DECAY_FIRST + 3 && i < count; ++i) {
        if (!versioned || params[i] > 127) params[i] = 0;
    }
}

#endif
