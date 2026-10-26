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

## `.snd`: kit (255 bytes)

| Offset | Size | Content |
|--------|------|---------|
| 0 | 8 | Kit name |
| 8 | 247 | Sound parameters 0 … 246 (`END_OF_SOUND_PARAMETERS`), one byte each, in parameter id order |

* Values are stored exactly as held in memory. Modulation destination
  parameters (`PAR_VEL_DEST_*`, `PAR_TARGET_LFO*`) hold an index into
  `mod_targets` (`params.json`).
* Shorter files (older firmware with fewer sound parameters) are accepted.
  Missing parameters read as 0.
* SysEx `KIT_DUMP` carries the same 255 bytes (see [sysex.md](sysex.md)).

`tmp.snd` (autosave of the edit buffer) has a different layout: 1 byte kit
slot number + 247 parameter bytes, and no name.

## `.pat`: pattern (3663 bytes)

| Offset | Size | Content |
|--------|------|---------|
| 0 | 8 | Pattern name |
| 8 | 6 | Version tag `"PatV03"` (files without `PatV` are read as version 1) |
| 14 | 3649 | [Pattern body](#pattern-body) |

SysEx `PATTERN_CHUNK` transfers carry `name[8]` + body (3657 bytes), i.e. the
`.pat` file without the version tag.

`tmp.pat` (autosave of the playing pattern) is 1 byte pattern number + body,
with no name and no tag.

### Pattern body

| Offset | Size | Content |
|--------|------|---------|
| 0 | 7 × 512 | For each track 0–6: 64 [steps](#step-7-bytes) (448 bytes), then 64 [timing bytes](#step-timing-1-byte) |
| 3584 | 7 × 4 × 2 | Main step bits: for each track, for each bar 0–3, a `uint16` LE; bit *n* = step `bar × 16 + n` is on |
| 3640 | 1 | Shuffle |
| 3641 | 7 | Track lengths, one per track (steps) |
| 3648 | 1 | Kit number linked to the pattern |

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
