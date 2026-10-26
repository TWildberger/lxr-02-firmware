# LXR-02 SD card file formats (firmware 1.7)

This describes the files the firmware reads and writes on the SD card, as
implemented in `Core/Src/Preset/presetManager.c`. All multi-byte integers are
**little endian**. Names are 8 bytes of ASCII, padded with `00` or spaces (not
necessarily NUL-terminated).

Parameter numbers, ranges and labels are in [`params.json`](params.json).
Regenerate it with `python3 tools/gen_params.py`, and check it with
`python3 tools/gen_params.py --check` (CI-friendly: exits non-zero if the
file no longer matches the sources).

## Folder layout

| Path | Content |
|------|---------|
| `/proj.idx` | 1 byte: number of the last used project (0–63) |
| `/projNN/` | One folder per project (`NN` = 00–63) |
| `/projNN/prj.nfo` | 8-byte project name |
| `/projNN/NN-xxxxx.snd` | Kit slot `NN` |
| `/projNN/NN-xxxxx.pat` | Pattern slot `NN` |
| `/projNN/NN-xxxxx.sng` | Song slot `NN` |
| `/proj00/glo.cfg` | Global settings, read at boot |
| `/tmp/` | Working copy of the current project plus autosave files |

File names are `NN-` + up to the first 5 characters of the slot name, with
spaces turned into `_` and characters FAT does not allow removed, plus the
extension. When loading, the firmware matches files on the `NN-` prefix and
the extension, and falls back to an older naming scheme. Renaming a slot
therefore creates a new file name.

At boot the firmware reads `glo.cfg` from `/proj00`, then switches to `/tmp`
and restores the autosaved kit and pattern. Saving a project writes
`prj.nfo`, `glo.cfg` and all 64 kits and patterns into `/projNN`.
`preset_saveGlobals()` writes `glo.cfg` into the *current* folder.

## `.snd`: kit (261 bytes; legacy 255 bytes)

| Offset | Size | Content |
|--------|------|---------|
| 0 | 8 | Kit name |
| 8 | 247 | Sound parameters 0 … 246 (`END_OF_SOUND_PARAMETERS`), one byte each, in parameter id order |
| 255 | 6 | Engine extension version tag, ASCII `EngV01` (new saves only) |

* Values are stored exactly as held in memory. Modulation destination
  parameters (`PAR_VEL_DEST_*`, `PAR_TARGET_LFO*`) hold an index into
  `mod_targets` (`params.json`).
* Shorter files (older firmware with fewer sound parameters) are accepted.
  Missing parameters read as 0.
* SysEx `KIT_DUMP` carries the same 261 bytes (see [sysex.md](sysex.md)).
  The parameter count remains 247; the tag is not a parameter.
* **Legacy migration:** without the exact `EngV01` tag, ids 221–226 (formerly
  unused MIDI-note bytes) reset to zero. No legacy waveform, FM-mode or other
  active sound byte is reinterpreted or range-clamped. With the tag, ids 221–223
  select the engine for Drum 1–3 (`0` = legacy, `1` = feedback FM); other values
  become zero. Ids 224–226 are timbre decay (0–127; invalid values become zero).
  All established parameter and modulation-target IDs stay unchanged.
  See [feedback-fm.md](feedback-fm.md).
* Stock firmware ignores the trailer and these six formerly unused bytes, so
  it plays new kits using the original engine. Saving with stock firmware drops
  the trailer; reloading that save on this firmware deliberately selects legacy.
  Back up new-engine kits before editing them on older firmware.
* FX type (`PAR_FX_TYPE`, offset 242) 5 is the plate reverb, and reverb type
  (`PAR_FX_REVERB_TYPE`, offset 252) is 0 = `off`, 1 = `Plt`, 2 = `Eco`. Older
  kits with FX type 5 have reverb type 0, so they still play the FX bus dry,
  as on stock v1.7. Kits saved with the reverb on play the FX bus dry on stock
  v1.7. See [reverb.md](reverb.md#compatibility).
* FX type 6 is the freeze / beat repeat (`frz`, [freeze.md](freeze.md)). It uses
  P1–P4 like the other FX types; there is no new byte. Stock v1.7 plays FX type 6
  dry.

`tmp.snd` (autosave of the edit buffer) has a different layout: 1 byte kit
slot number + 247 parameter bytes + the same 6-byte `EngV01` tag (254 bytes),
and no name. Legacy 248-byte autosaves migrate as above. At boot it is loaded after the
project, so it decides the edit buffer and the current kit slot.

> Stock v1.7 writes the load/save menu cursor as the slot byte, and at boot
> restores only that cursor, not the "current kit" number (which stays at the
> project's value, usually 0). This firmware writes the slot of the kit that is
> actually in the edit buffer, and restores it as the current kit. Files are
> compatible both ways.

## `.pat`: pattern

| Offset | Size | Content |
|--------|------|---------|
| 0 | 8 | Pattern name |
| 8 | 6 | Version tag `"PatV04"` (files without `PatV` are read as version 1) |
| 14 | 3649 | Legacy-compatible [pattern body](#pattern-body) |
| 3663 | 168 | Packed three-bit trig conditions (V04 only) |

V04 files are 3831 bytes. V01–V03 load with every condition set to `Always`.
The base pattern body and SysEx `PATTERN_CHUNK` layout remain unchanged:
transfers carry `name[8]` + the 3649-byte body (3657 bytes total), without the
version tag or condition extension. Conditions use the separate SysEx commands
documented in [sysex.md](sysex.md).

`tmp.pat` (autosave of the playing pattern) is 1 byte pattern number + the
3649-byte body, followed by the same 168-byte conditions extension. Older
autosaves without the extension load with all conditions set to `Always`.

### Pattern body

| Offset | Size | Content |
|--------|------|---------|
| 0 | 7 × 512 | For each track 0–6: 64 [steps](#step-7-bytes) (448 bytes), then 64 [timing bytes](#step-timing-1-byte) |
| 3584 | 7 × 4 × 2 | Main step bits: for each track, for each bar 0–3, a `uint16` LE; bit *n* = step `bar × 16 + n` is on |
| 3640 | 1 | Shuffle |
| 3641 | 7 | Track lengths, one per track (steps) |
| 3648 | 1 | Kit number linked to the pattern |

The V04 conditions extension is track-major, then step-major (448 trigs).
Each trig is a three-bit value packed least-significant-bit first. Values are
`0` Always, `1` every 2nd cycle, `2` every 3rd, `3` every 4th, `4` first of
every 2 cycles, `5` first of every 3, `6` first of every 4, and `7` every
cycle after the first. The track cycle starts at zero when playback starts or
the pattern changes.

Tracks: 0–2 Drum 1–3, 3 Snare/Clap, 4 Cymbal, 5 Closed hi-hat,
6 Open hi-hat.

#### Step (7 bytes)

| Byte | Field | Notes |
|------|-------|-------|
| 0 | `volume` | Bits 0–6 volume (0–127); bit 7 = sub-step active |
| 1 | `prob` | Probability 0–100 |
| 2 | `note` | MIDI note 0–127 |
| 3 | `param1Nr` | Automation destination 1 |
| 4 | `param1Val` | Automation value 1 |
| 5 | `param2Nr` | Automation destination 2 |
| 6 | `param2Val` | Automation value 2 |

#### Step timing (1 byte)

| Bits | Field | Notes |
|------|-------|-------|
| 0–3 | `flam` | 0 = off, 1–5 = ×2, ×3, ×4, ×6, ×8 |
| 4–7 | `shift` | 0–14, 7 = no shift (signed shift = value − 7) |

## `.sng`: song (72 bytes)

| Offset | Size | Content |
|--------|------|---------|
| 0 | 8 | Song name |
| 8 | 64 | Pattern numbers in play order. 64 = end of song, 65 = loop |

## `glo.cfg`: global settings

One byte per global parameter, from `PAR_BEGINNING_OF_GLOBALS` (272) up to
`NUM_PARAMS − 1`, in parameter id order (`scope == "global"` in
`params.json`).

| Firmware | Size | Content |
|----------|------|---------|
| Stock 1.7 | 30 bytes | ids 272 … 301 |
| This fork (`1.7-sx`) | 33 bytes | ids 272 … 304; adds `SysEx` enable (302), `DeviceID` (303, stored as 1–16), `ParmEcho` (304) |

Compatibility:

* This fork reads a stock 30-byte `glo.cfg` and uses defaults for the three
  new settings (SysEx on, device ID 1, echo off). Out-of-range bytes also
  fall back to these defaults.
* Stock firmware reads the first 30 bytes of a 33-byte file and ignores the
  rest, so moving back to stock firmware is safe.
* SysEx `GLOBALS_DUMP` carries these bytes in the same order.

## `prj.nfo` and `proj.idx`

* `prj.nfo`: 8-byte project name.
* `/proj.idx`: 1 byte, last used project number.

## Legacy `.all` / `.prj` single-file formats

`presetManager.c` still contains code for single-file project formats: name,
version byte `2`, globals padded with `0xFF` to 64 bytes, a kit padded to 512
bytes, then pattern data. That code is compiled out (`#if 1 … #else`) in
1.7, which uses the folder layout above. If it is re-enabled, the
`glo.cfg` reader's range checks keep the `0xFF` padding from turning into
SysEx settings.
