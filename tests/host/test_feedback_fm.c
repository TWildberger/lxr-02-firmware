/* SPDX-License-Identifier: GPL-3.0-only */
#include "Soundengine/DSPAudio/FeedbackFm.h"
#include "Preset/KitEngineFormat.h"
#include "Sysex/SysexCodec.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t sine[4097];

static void render(FeedbackFm *fm, int16_t *out, uint8_t n,
                   float carrier, float mod, float index, float fb)
{
    feedbackFm_render(fm, sine, out, n, carrier, mod, index, fb);
    assert(isfinite(fm->envelope) && fm->envelope >= 0.f && fm->envelope <= 1.f);
    assert(isfinite(fm->previousMod) && fabsf(fm->previousMod) <= 1.f);
    for (unsigned i = 0; i < n; ++i) assert(abs(out[i]) <= 24000);
}

static void test_extremes(void)
{
    const float frequencies[] = {-FLT_MAX, -1.f, 0.f, 8.f, 55.f, 440.f, 16000.f, FLT_MAX};
    const float controls[] = {-1.f, 0.f, .5f, 1.f, 255.f / 127.f};
    int16_t out[255];
    for (unsigned c = 0; c < 5; ++c) {
        for (unsigned f = 0; f < 8; ++f) {
            FeedbackFm fm;
            feedbackFm_init(&fm);
            feedbackFm_setDecay(&fm, controls[c]);
            assert(fm.decay > .94f && fm.decay < 1.f);
            feedbackFm_trigger(&fm);
            for (unsigned i = 0; i < 400; ++i) {
                render(&fm, out, 255, frequencies[f], frequencies[7-f],
                       controls[c], controls[(c+1)%5]);
            }
        }
    }
}

static void test_retrigger_and_blocks(void)
{
    FeedbackFm fm, fresh;
    int16_t a[128], b[128];
    feedbackFm_init(&fm);
    feedbackFm_setDecay(&fm, .5f);
    feedbackFm_trigger(&fm);
    fresh = fm;
    render(&fm, a, 128, 110.f, 330.f, .8f, .9f);
    render(&fresh, b, 64, 110.f, 330.f, .8f, .9f);
    render(&fresh, b+64, 64, 110.f, 330.f, .8f, .9f);
    assert(memcmp(a, b, sizeof(a)) == 0);
    for (unsigned i = 0; i < 200; ++i) {
        feedbackFm_trigger(&fm);
        render(&fm, b, 128, 110.f, 330.f, .8f, .9f);
        assert(memcmp(a, b, sizeof(a)) == 0);
    }
    fresh = fm;
    render(&fm, b, 0, 110.f, 330.f, .8f, .9f);
    assert(memcmp(&fresh, &fm, sizeof(fm)) == 0);
    feedbackFm_init(&fm);
    assert(fm.envelope == 0.f && fm.carrierPhase == 0 && fm.previousMod == 0.f);
}

static void test_timbre(void)
{
    FeedbackFm fm;
    int16_t a[255], b[255];
    feedbackFm_init(&fm);
    feedbackFm_setDecay(&fm, 1.f);
    feedbackFm_trigger(&fm);
    render(&fm, a, 255, 220.f, 700.f, .8f, 0.f);
    feedbackFm_trigger(&fm);
    render(&fm, b, 255, 220.f, 700.f, .8f, 1.f);
    assert(memcmp(a, b, sizeof(a)) != 0);
    feedbackFm_trigger(&fm);
    render(&fm, a, 255, 220.f, 700.f, 0.f, 1.f);
    feedbackFm_trigger(&fm);
    render(&fm, b, 255, 220.f, 1400.f, 0.f, 0.f);
    assert(memcmp(a, b, sizeof(a)) == 0);
    const float controls[] = {0.f, .5f, 1.f};
    for (unsigned c = 0; c < 3; ++c) {
        feedbackFm_setDecay(&fm, controls[c]);
        feedbackFm_trigger(&fm);
        const float seconds = .003f + 1.997f * controls[c] * controls[c];
        const unsigned samples = (unsigned)(44140.f * seconds);
        for (unsigned i = 0; i < samples; ++i)
            render(&fm, b, 1, 220.f, 700.f, .8f, .8f);
        /* The inexpensive one-pole approximation must meet the advertised T60. */
        assert(fm.envelope > .0007f && fm.envelope < .0011f);
        for (unsigned i = 0; i < 2000; ++i)
            render(&fm, b, 255, 220.f, 700.f, .8f, .8f);
        assert(fm.envelope == 0.f); /* no denormal tail */
    }
}

static void test_kit_migration(void)
{
    uint8_t kit[247], original[247];
    for (unsigned value = 0; value < 256; ++value) {
        memset(original, value, sizeof(original));
        memcpy(kit, original, sizeof(kit));
        kitEngine_migrate(kit, sizeof(kit), NULL, 0);
        for (unsigned i = 0; i < sizeof(kit); ++i) {
            if (i >= 221 && i <= 226) assert(kit[i] == 0);
            else assert(kit[i] == original[i]); /* includes every legacy wave/FM byte */
        }
        for (unsigned i = 221; i < 224; ++i) assert(!drumEngine_isFeedback(kit[i]));
        assert(drumEngine_isFeedback(value) == (value == 1));
        memcpy(kit, original, sizeof(kit));
        kitEngine_migrate(kit, sizeof(kit), (const uint8_t *)KIT_ENGINE_TAG, KIT_ENGINE_TAG_SIZE);
        assert(kit[221] == (value <= 1 ? value : 0));
        assert(kit[224] == (value <= 127 ? value : 0));
    }
    memset(kit, 1, sizeof(kit));
    for (size_t n = 0; n < KIT_ENGINE_TAG_SIZE; ++n) {
        kitEngine_migrate(kit, sizeof(kit), (const uint8_t *)KIT_ENGINE_TAG, n);
        assert(kit[221] == 0);
    }
    memset(kit, 1, sizeof(kit));
    kitEngine_migrate(kit, sizeof(kit), (const uint8_t *)"EngV02", 6);
    assert(kit[221] == 0);
    /* The same versioned body travels through SD containers and SysEx packing. */
    uint8_t raw[261] = {0}, packed[300], decoded[261];
    raw[8+221] = 1;
    raw[8+224] = 90;
    memcpy(raw+255, KIT_ENGINE_TAG, KIT_ENGINE_TAG_SIZE);
    const int32_t count = sx_pack(raw, sizeof(raw), packed);
    assert(count > 0);
    assert(sx_unpack(packed, count, decoded, sizeof(decoded)) == sizeof(raw));
    const uint16_t modIndices[] = {149, 150, 151, 152, 153, 154, 167, 168, 169, 170, 171, 172};
    sx_kitApply(kit, sizeof(kit), decoded+8, 247, modIndices, 12, 229);
    kitEngine_migrate(kit, sizeof(kit), decoded+255, 6);
    assert(kit[221] == 1 && kit[224] == 90);
    kitEngine_migrate(kit, sizeof(kit), NULL, 0);
    assert(kit[221] == 0 && kit[224] == 0); /* a 255-byte legacy upload */
    /* Short legacy kits must not touch memory beyond their parameter count. */
    uint8_t shortKit[16] = {0};
    kitEngine_migrate(shortKit, sizeof(shortKit), NULL, 0);
}

int main(void)
{
    for (unsigned i = 0; i <= 4096; ++i)
        sine[i] = (int16_t)(-32767.0 * cos(6.283185307179586 * i / 4096.0));
    test_extremes();
    test_retrigger_and_blocks();
    test_timbre();
    test_kit_migration();
    puts("test_feedback_fm: all tests passed");
    return 0;
}
