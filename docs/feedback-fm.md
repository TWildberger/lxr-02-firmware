# Feedback-FM drum engine

Drum voices **1-3** can select one new engine: `fbm`. Snare, cymbal and hi-hat
are unchanged. This is a two-operator sine phase-modulation engine with
**modulator self-feedback** and a **separate, sample-rate timbre envelope**.
It is not another name for the original FM mode: the original has no feedback,
and its FM index follows the pitch envelope. Here, timbre can decay while pitch
stays still, and feedback can turn a clean strike into a rough, metallic attack.

This design reuses the existing sine table, pitch/note handling, amp and pitch
envelopes, transient generator, SVF, drive, pan and mixer. It replaces only the
two oscillator renders. A modal bank would add several resonators per voice;
feedback FM needs just two interpolated lookups per sample and 24 bytes of
state per voice, with no new tables, delay lines, heap allocations or FX memory.
The new DSP functions stay in flash, not ITCM.

## Controls

On Drum 1, 2 or 3's oscillator page, access its second four-knob screen:
`eng` selects `old` (0) or `fbm` (1), and `tmd` sets timbre decay. Existing kits
always load with `old` unless explicitly saved in the new versioned format.
Switching engine resets the feedback state; the next trigger also restarts its
phases and timbre envelope deterministically, even during an active tail.

| Control | Meaning in `fbm` |
|---------|------------------|
| Osc coarse/fine, note | Carrier tuning; sequencer and MIDI notes still transpose the voice |
| FM amount (`amt`) | Initial phase-modulation index, 0-127; 0 is a clean sine |
| FM frequency (`frq`) | Modulator tuning relative to the played note, with the original semitone control mapping |
| Osc PWM (`pwm`) | Feedback amount, 0-127; does not alter legacy PWM behavior |
| Timbre decay (`tmd`) | Independent timbre T60: `0.003 + 1.997*(value/127)^2` seconds (3 ms-2 s) |
| Pitch decay/amount/slope | Carrier pitch sweep, independent of timbre decay |
| Amp attack/decay/slope | Original volume envelope, controlling the complete voice tail |
| Transient, filter, drive, mixer | Original downstream processing, unchanged |
| Main/mod wave, FM mix/mod mode | Preserved for returning to `old`, but ignored by `fbm` (both operators are sine) |

The new selectors and timbre-decay parameters are panel/NRPN/PARAM_SET controls,
not additional LFO/velocity/step-automation targets. The **229 existing target
indices are unchanged** so old patterns and kits keep their destinations.
Existing targets for pitch, FM amount/frequency, PWM (feedback), amp, filter and
drive work with `fbm`. Timbre decay is a static per-kit setting that can also be
edited live. Engine selection is discrete during kit morph: source below 128,
destination at 128-255. Other sound parameters retain their original morphing.

| Voice | Engine parameter / NRPN | Timbre-decay parameter / NRPN |
|-------|-------------------------|------------------------------|
| Drum 1 | 221 / 93 | 224 / 96 |
| Drum 2 | 222 / 94 | 225 / 97 |
| Drum 3 | 223 / 95 | 226 / 98 |

For example, NRPN 93 (`CC99=0`, `CC98=93`, `CC6=1`) opts Drum 1 into `fbm`.
SysEx `PARAM_SET` uses parameter id 221 with value 1; id 224 controls its
timbre decay. A subsequent save or dump carries the format version automatically.
Generated ranges, labels and page positions are in [params.json](params.json).

### Starting sounds

Start from Initkit, select `fbm`, turn down the transient and drive, and use a
low-pass cutoff of 127 with modest resonance. These are starting points, not
hardware-auditioned factory presets; balance channel volume before comparing.

| Sound | Carrier coarse (MIDI-note value) | FM frequency value | FM amount | PWM/feedback | Timbre decay | Amp decay |
|-------|---------------------------------|--------------------|-----------|--------------|--------------|-----------|
| Rounded FM kick | 33 (about 55 Hz) | 33 | 35 | 10 | 15 (31 ms) | 40 |
| Metallic tom | 45 (about 110 Hz) | 64 | 80 | 40 | 45 (254 ms) | 55 |
| Bell / industrial hit | 57 (about 220 Hz) | 76 | 100 | 85 | 95 (1.12 s) | 90 |

Use fine near its midpoint, amp attack 0 and pitch amount initially 0. The
coarse/frequency values above are raw stored values, not the signed LCD display:
the display subtracts 60. Raise pitch amount slightly and shorten pitch decay
for a kick click; sweep feedback or FM amount with an LFO for abrasive metal.
High FM frequency plus feedback intentionally produces digital/aliased timbres.

## Bounds and compatibility

The sample rate is the hardware's 44140 Hz. Carrier and modulator frequency
are clamped to 0-8828 Hz. Phase accumulators wrap unsigned; signed deviations
remain below half a cycle. The index and feedback taper above 4414 Hz to reduce
high-frequency aliasing, but this is **not** an oversampled or fully band-limited
FM engine. Operator output is bounded to +/-24000 before the original transient,
filter, amp and drive stages; those stages retain their original gain/clipping.
Feedback uses a one-pole-smoothed modulator output. Very small timbre
envelopes become exactly zero to avoid denormal tails.

Six formerly unused kit bytes are used, **not new waveform/mode enum values**:
221-223 are engine selectors and 224-226 are timbre decay. The file appends the
six ASCII bytes `EngV01`, making `.snd` 261 bytes and `tmp.snd` 254 bytes. The
247-byte sound array, global IDs, pattern format and modulation-target IDs do
not grow or move.

On an old/unversioned SD kit, autosave or SysEx upload, only these six previously
inactive bytes are cleared. All active legacy sound bytes (including unexpected
waveform values in factory kits) keep their old interpretation and processing.
With the tag, invalid selectors/decay values become zero. Initkit also explicitly
clears these six bytes. Stock firmware ignores the trailer and plays the original
engine; saving there drops the trailer and thus returns the kit to legacy on
this fork. Back up new-engine kits before using older firmware.

SysEx capability bit 7 advertises this extension. `KIT_DUMP` still reports
247 parameters but appends `EngV01` inside packed data. A librarian must preserve
the trailer to round-trip new-engine kits; a 255-byte legacy upload deliberately
cannot turn an engine on. See [file-formats.md](file-formats.md) and
[sysex.md](sysex.md).

## Memory and CPU budget

Same-toolchain release comparison against freeze commit
`c53feecdaaa1727cd922a37c5a30488499b80e53`, using the existing CMake release
preset and Arm GNU 15.3.Rel2 (15.3.1). Linker region usage includes alignment/stubs:
The after build includes the rebase onto freeze capability commit
`9500caf3203be01f26464c381e4f2dae58b227b8`; the before column retains the
original freeze baseline measured above.

| Region | Before (bytes) | After (bytes) | Delta | Capacity |
|--------|----------------|---------------|-------|----------|
| ITCM | 14864 | 14984 | +120 | 16384 |
| DTCM | 107752 | 107824 | +72 | 131072 |
| General RAM | 279000 | 279000 | 0 | 393216 |
| Flash region | 256900 | 258724 | +1824 | 1015808 |

The per-voice state adds 72 bytes total; engine selectors fit existing struct
padding. `FX_BUFFER` remains exclusively FX-owned. No new buffer or wavetable is
allocated. ITCM growth is dispatch glue and linker veneers in existing voice
functions, not relocation of the new DSP loop.

**Estimate, not a hardware measurement:** allow roughly 80-160 incremental
cycles per new-engine sample (including block setup, two interpolated lookups,
feedback and envelope work). At 44140 Hz / 216 MHz that is about **1.6-3.3 CPU
percentage points per voice**, or **5-10 points with all three selected**.
This deliberately charges the whole new loop as incremental even though it
replaces the old pair of oscillator renders; it is a conservative planning
allowance, not a worst-case execution-time proof. Cache misses, modulation,
interrupts and UI/SD contention still need on-device measurement.

Using the supplied hardware baseline of 52-53% voices, about 5% mixer and
11.5% plate (6.7% Eco), budget up to **79% steady total with plate**, or about
**88% against the observed 78% FX-switch peak**. With freeze instead of plate,
its estimated 2-4% cost gives a lower projected total. There is only one FX
slot, so plate and freeze cannot accumulate. Only three voices can run this
engine; snare/cymbal/hat keep their old cost. This leaves an estimated 12-point
transient margin, but **safe headroom is provisional until measured**.
Do not enable additional engines or FX in this layer.

### Hardware acceptance checklist (not yet run)

One five-minute workload with all three feedback-FM engines enabled in the
live edit buffer was measured on hardware; see the
[long-run result](performance.md#feedback-fm-long-run-measurement). That single
scenario does not complete the broader acceptance checks below.

1. Back up the SD card, flash the release image, and compare representative
   factory/user kits against the freeze build with `eng=old`, including kits
   containing non-panel-range waveform/transient bytes.
2. Audition the three example sounds, minimum/maximum pitch, index, feedback,
   amp/filter settings, MIDI velocity, pitch sweeps, fast retriggers and choke.
   Check tails, DC/clicks, clipping and usable perceived volume.
3. Use all six voices, long tails and dense retriggers with all three drums on
   `fbm`; repeat with plate, Eco and freeze separately. Reset CPU peak/xruns,
   run at least five minutes each, then exercise FX switches, LFO modulation,
   panel edits, SysEx traffic, kit morph and SD saves. Record now/peak and
   section breakdown using [performance.md](performance.md).
4. Require zero steady xruns and a worst-case peak below 90% for this budget.
   If not met, keep `eng=old` or use Eco/off until the loop is optimized and
   the same workload is remeasured. A host benchmark cannot establish safety.
5. Save/reload a new-engine kit, power-cycle autosave, copy to a different slot,
   morph to/from a legacy kit, and round-trip both slot and edit-buffer SysEx.
   Confirm `EngV01` survives and every control returns. Confirm old 255-byte
   kits and 248-byte autosaves never opt in, regardless of reserved bytes.
6. Reload a kit saved on stock firmware and confirm legacy playback. Keep
   a tagged backup because a stock save intentionally loses engine selection.
