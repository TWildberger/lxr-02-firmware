# Freeze / beat repeat (FX bus)

The FX bus has a seventh effect type, **`frz`**: an audio freeze / beat repeat.
While it is selected it keeps recording the FX bus. When you **engage** it
(hold a step button, MIDI or SysEx), it loops a slice of what was just played
for as long as it is held. The slice length is synced to the sequencer. On
release the live signal comes back. The code is in
`Core/Src/Soundengine/DSPAudio/FX/Freeze.c`.

This is an audio effect: it repeats whatever was routed to the FX bus,
including sound tails. A sequencer beat repeat (looping pattern steps) is a
separate, later feature ([backlog](backlog.md)).

## Using it

Like every FX type, the freeze is an insert on the FX bus. A voice is sent to it
by setting the voice's output routing to the FX bus. The FX page (**FX** button)
switches to the freeze sub-page when you select `frz`:

| Slot | Label | Parameter | Range |
|------|-------|-----------|-------|
| 1 | `typ` | FX type (`PAR_FX_TYPE`) | `frz` = 6 |
| 2 | `out` | FX output routing (`PAR_FX_ROUTING`) | stereo, or a single output as for the other FX |
| 3 | `d/w` | Dry/wet (`PAR_FX_DRY_WET`) | 0 … 100 %. Only applies while engaged. Use 100 for a classic beat repeat |
| 4 | — | | |
| 5 | `len` | Slice length (`PAR_FX_P1`) | 8 zones: `0–15` 1/4, `16–31` 1/4T, `32–47` 1/8, `48–63` 1/8T, `64–79` 1/16, `80–95` 1/16T, `96–111` 1/32, `112–127` 1/32T |
| 6 | `pit` | Pitch (`PAR_FX_P2`) | `64` = original. Below 64: repeats play slower and lower, down to half speed (one octave) at 0. Above 64: **tape stop**. Every repeat glides down, by up to 2 octaves per slice at 127, and fades out once it is very slow |
| 7 | `dec` | Decay per repeat (`PAR_FX_P3`) | gain × 0.25 (0) … × 1.0 (127, no decay) on each repeat |
| 8 | `gat` | Gate (`PAR_FX_P4`) | part of the slice that plays: 1/16 (0) … whole slice (127). Short gates give a stutter |

While the freeze is **not engaged**, the FX bus passes through exactly as with
FX type `off`, bit for bit, whatever the dry/wet setting is.

All parameters are ordinary kit parameters. They are stored in `.snd`, sent as
NRPN (FX type 106, dry/wet 108, P1–P4 = 109–112) and are available over SysEx
(`PARAM_SET`, `KIT_DUMP`) and as LFO and velocity targets like the other FX
parameters. Engaging is a performance action and is **not** stored in the kit.

## Engaging

The freeze only reacts while the FX type is `frz`. All three ways use the same
values and can be mixed. The last message wins.

| Value | Action |
|-------|--------|
| 0 | release |
| 1 … 8 | engage (or change rate while engaged): 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T |
| 9 … 127 | engage at the slice length set by `len` (P1). It follows `len` changes while engaged |

### Front panel

In **perf mode** (the mode in which steps 1–8 trigger rolls), **hold step
buttons 9–16** (no shift):

| Step | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 |
|------|---|----|----|----|----|----|----|----|
| Slice | 1/4 | 1/4T | 1/8 | 1/8T | 1/16 | 1/16T | 1/32 | 1/32T |

* The freeze is engaged while the button is held, and its LED is lit.
* Press another of these buttons while holding to change the rate
  (for example hold 9, then 11, then 13 for a roll-down). Releasing a button
  goes back to the rate of a button that is still held (the rightmost one), and
  releasing the last one releases the freeze.
* Steps 9–16 did nothing in perf mode before, so no existing combination
  changes. Rolls on steps 1–8 work at the same time.
* The freeze lasts exactly as long as a step is held: its release is handled
  even after a mode change or with Shift/Load/Save held, and then does nothing
  else in the new mode.
* If the FX type is not `frz`, the buttons do nothing (no LED).

### MIDI

NRPN **210** (CC 99 = `1`, CC 98 = `82`), value in CC 6 (data entry MSB), on any
channel the LXR receives CCs on (the global channel, or a voice channel). The
CC receive filter must be on. Examples on channel 1:

```
B0 63 01   B0 62 52   B0 06 05     engage at 1/16
B0 06 00                           release (the NRPN number stays selected)
B0 06 7F                           engage at the len (P1) rate
```

NRPN 210 sits after the mute NRPNs (200–206) and is not a kit parameter. The
stock CC/NRPN mapping is unchanged. A configurable MIDI note is in the
[backlog](backlog.md). Notes on the global channel already trigger voices.

### SysEx

`FX_FREEZE` (`46`), payload one u7 value as in the table above. There is no
reply on success, so it can be sent from a pad or key with low latency. If the
FX type is not `frz`, the device answers `NAK(46, ERR_RANGE, fx type)`. See
[sysex.md §5.19](sysex.md#519-fx_freeze-46).

```
F0 7D 4C 7F 01 46 05 3D F7     engage at 1/16
F0 7D 4C 7F 01 46 00 38 F7     release
F0 7D 4C 7F 01 46 7F 47 F7     engage at the len (P1) rate
```

## Timing

* **Slice length**: one 16th step = `44140 × 15 / BPM` frames. While the
  sequencer runs, the measured time between the last steps is used instead, so
  shuffle, MIDI clock and the external trigger clock are followed.
* **Grid**: with the sequencer running, slices sit on the step grid: 1/4
  slices start on a beat (every 4 steps from the pattern start), 1/4T and 1/8T
  every 8 and 4 steps, 1/8 and 1/16T every 2, 1/16 and the 1/32s on every step.
  The slice that repeats is the grid slice you engaged in: it plays on to its
  end, and from the next grid line it repeats (like a beat repeat with a
  1-slice interval). The repeats stay on the beat. Changing to another rate
  that fits in the recorded slice keeps the slice start and switches at the
  last grid line of the new rate before the planned repeat, so the loop never
  plays past the recorded audio (if that line is too close, the planned repeat
  is kept and the new rate starts there).
* **Free-running** (sequencer stopped): the slice starts where you engaged, plays
  on, and repeats after one slice length taken from the BPM setting.
* The sequencer step is taken at audio-block resolution (64 frames, 1.45 ms).
* Tempo changes while engaged take effect at the next engage.

### Buffer limit

The freeze uses the FX buffer shared with the delay and the reverb (32768
samples, 16-bit float). There is room for 16384 frames (0.37 s) in stereo, or
32768 (0.74 s) if the FX output is a single (mono) output. A slice that does not
fit is halved (1/4 → 1/8 → 1/16, 1/4T → 1/8T …), and clipped to the buffer as a
last resort. Longest fitting slices:

| Slice | Stereo FX output | Mono FX output |
|-------|------------------|----------------|
| 1/4 | from 163 BPM | from 82 BPM |
| 1/4T | from 109 BPM | from 55 BPM |
| 1/8 | from 82 BPM | from 41 BPM |
| 1/8T | from 55 BPM | from 28 BPM |

At 120 BPM in stereo, 1/4 plays as 1/8. Right after selecting `frz` (or
changing the FX output), the buffer only holds what was recorded since. If
the slice start is not recorded yet, the next grid slice is used.

## Click-free transitions

* Engage and release fade the FX output between dry and wet over 256 frames
  (5.8 ms).
* Each repeat starts with a 128-frame (2.9 ms) crossfade from the previous one,
  and from the live signal into the first one. The crossfade reads the audio
  just before the slice start, so the slice start, for example a hit on the
  grid, is played in full.
* A gate closes with the same 128-frame fade.
* A longer rate while engaged needs audio that is not in the loop: the repeats
  fade back to the live signal, then a new slice is taken.

## Compatibility

* No new parameter, no `.snd`/`glo.cfg` format change. Old kits never contain FX
  type 6 and sound as before. FX type values above 6 still pass through dry.
* A kit saved with FX type `frz` plays the FX bus dry on stock v1.7 (unknown FX
  types fall back to `off`). The stock FX menu shows a wrong name for it.
* On first selection the P1–P4 values left from the previous FX type are used:
  set them up and save the kit.

## CPU and memory

* No new buffers. 264 bytes of state in DTCM. All code is in flash. ITCM grows
  by 48 bytes (the FX switch case and the sequencer hook).
* Estimated **fx** load ([performance.md](performance.md#freeze)): ≈0.7–0.9 %
  while not engaged (recording only), ≈2–2.7 % while repeating, ≈3.4–4.4 % in
  the one or two blocks of a crossfade between repeats. The delay measured 3.2 %.

## Hardware test checklist

Set up: a kit with voices routed to the FX bus, FX type `frz`, `d/w` 100,
`len` 64 (1/16), `pit` 64, `dec` 127, `gat` 127, sequencer playing at 120 BPM.

- [ ] Selecting `frz` shows the sub-page `typ out d/w – len pit dec gat`.
- [ ] Not engaged: the sound is identical to FX `off` (also with `d/w` at 50).
- [ ] Perf mode, hold step 13: the current 16th repeats from the next step on,
      in time with the beat, no clicks at the loop points. Release: the live signal comes back without a
      click.
- [ ] Hold steps 9–16 one by one: 1/4 (plays as 1/8 at 120 BPM in stereo),
      1/4T, 1/8, …, 1/32T. Hold 9, then 11, 13, 15 for a roll-down; release in
      reverse order.
- [ ] Rolls on steps 1–8 still work, also together with a freeze.
- [ ] Hold step 13, press Shift + an FX/mode step (or change mode), release
      everything: the freeze releases with step 13, and its release does not
      set a step in the new mode.
- [ ] FX type not `frz`: steps 9–16 do nothing.
- [ ] Sequencer stopped (free-running), play pads, engage: one 1/16 (at the BPM
      setting) from the moment of engaging repeats.
- [ ] `pit` 0: repeats an octave lower. `pit` 127: tape stop within a slice or
      two, fades out without a DC thump.
- [ ] `dec` 0: repeats die away within a few repeats. `gat` 0: short stutters.
- [ ] Turn `len` while engaged with value ≥ 9 (MIDI/SysEx): the rate follows.
- [ ] Hold 11 (1/8), then press and release 13 (1/16) right before a repeat:
      no burst of older audio.
- [ ] FX output to a single output: 1/4 at 120 BPM plays as a full 1/4.
- [ ] MIDI: NRPN 210 (CC 99 = 1, CC 98 = 82, CC 6 = 5) engages, CC 6 = 0 releases.
- [ ] SysEx: `F0 7D 4C 7F 01 46 05 3D F7` engages, `… 46 00 38 F7` releases. With
      another FX type the device answers `NAK(46, 04, type)`.
- [ ] MIDI clock sync and shuffle: repeats stay on the grid.
- [ ] Switch FX type `frz` ↔ `del` ↔ `rev` while playing (also while engaged): no
      noise burst, no stale audio, xruns stay 0.
- [ ] Load an old kit with the FX bus off/delay/reverb: it sounds unchanged.
- [ ] Measure the **fx** load and fill in [performance.md](performance.md#freeze).
