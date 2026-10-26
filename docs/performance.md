# Audio CPU meter and performance

The firmware measures how much of the available time the audio render uses.
This is the headroom left for new engines and effects (for example the
[plate reverb](reverb.md) and [feedback-FM engine](feedback-fm.md)). This page describes what is measured, how
to read it, and holds the numbers measured on hardware.

## What is measured

Audio runs at ≈44.14 kHz (`REAL_FS`) in blocks of `OUTPUT_DMA_SIZE` = 64 stereo
frames (`Core/Inc/Soundengine/config.h`). One block lasts 1.45 ms, which is
`SystemCoreClock × 64 / 44140` ≈ **313 185 CPU cycles** at 216 MHz. This is
100 %.

The main loop renders the next block when the I2S DMA callback asks for it
(`soundengine_calcNextSampleBlock` → `mixer_calcNextSampleBlock`). The DWT
cycle counter is read around that call, and the result is accounted in
`Core/Src/Soundengine/CpuMeter.c`.

| Value | Meaning |
|-------|---------|
| **now** (`cpu`) | Average render time per block over the last 256 blocks (≈0.37 s) |
| **peak** (`pk`) | Worst single block since boot or the last reset |
| **voices** (`vce`) | LFOs, modulation, voice parameter updates and the six voice renders |
| **mixer** (`mix`) | Buffer clears, per-voice decimation, pan and output routing |
| **fx** (`fx`) | `FxBus_process` (distortion, ring, compressor, delay, reverb, freeze), including the one-off buffer clear when the FX type changes |
| **xruns** (`xrn`) | Underruns: the DAC wanted a new block but the previous one had not been rendered yet. Saturates at 16383 |

Notes:

* The times are wall-clock cycles. Interrupts that fire during the render (I2S
  DMA, TIM2, UART, USB) are included. That matches what matters for dropouts.
* *voices + mixer + fx* is slightly less than *now*. The rest is the call
  overhead and the code around the mixer.
* The sequencer tick, UI, MIDI and SD card work in the main loop are **not**
  in the load. They still delay the render because it is polled from the main
  loop. If the main loop stalls for longer than one block, you get an
  **xrun** even when the load is low. So check xruns together with peak.
* Loads above 100 % are possible (one block took longer than real time). With
  the double-buffered DMA a single block over 100 % does not always cause an
  audible dropout, but it does use up all headroom.
* Overhead: each of the 15 section probes per block is one call to a 36-byte
  ITCM function (≈20 cycles). The total adds two DWT reads and ≈100 cycles of
  bookkeeping in flash. Together this is about 0.1 % of the budget. Building
  with `-DCPU_METER_SECTIONS=0` removes the section probes. The total and xruns
  are still measured.
* The meter lives in normal RAM (`.bss`, 88 bytes). Nothing was moved out of
  ITCM or DTCM.

## How to read it

### On the LCD

Global menu (**shift + load/save**). Press it again, or turn the encoder, to
move past the `syx id ech` screen to the CPU screens:

```
CPU pk  vce mix*        fx  xrn        <
3.4 4.1 2.2 0.6         0.4   0
```

* Loads are shown as `d.d` percent below 10 %, and as whole percent from 10 %
  up (up to 999).
* The screen refreshes every 250 ms while it is shown.
* Click the encoder on an entry to see its full name and a finer value. For
  example, the top row shows `CPU     Load` and the bottom row shows ` 12.3%`.
  The xrun count is shown up to 16383.
* In that edit view, turning the encoder on **pk** or **xrn** clears the peak
  and the xrun counter. The other entries are read-only.

### Over SysEx

`STATUS_REPLY` carries the six values when the identity capability bit 6 is
set ([sysex.md §5.4](sysex.md#54-status_request-03--status_reply-04)).
Send `F0 7D 4C 7F 01 03 7D F7` for a plain request. Send
`F0 7D 4C 7F 01 03 01 7C F7` to also clear the peak and the xruns after the
reply.

`tools/lxr_status.py` does this for you (needs `pip install mido python-rtmidi`):

```
python3 tools/lxr_status.py --list                 # show MIDI ports
python3 tools/lxr_status.py --port LXR             # one reading
python3 tools/lxr_status.py --port LXR --reset     # read, then clear peak/xruns
python3 tools/lxr_status.py --port LXR --watch 1   # poll every second
python3 tools/lxr_status.py --port LXR --watch 1 --json > run.jsonl
```

### Measuring a scenario

1. Set up the scenario (kit, pattern, FX) and let it run for a few seconds.
2. `lxr_status.py --reset` (or turn the encoder on `pk`) to clear the peak
   from the setup.
3. Let it run for at least 30 s, then read *now*, *peak* and *xruns*.

The peak includes rare events, for example SD card access or a burst of MIDI.
For headroom planning, use *now* for the steady load and *peak* for the margin.

## Hardware numbers

Measured on the user's LXR-02 over USB with the release image
(`LXR_V2_V1.7-sx.img`, SHA-256 `46b8f317…`), kit slot 16 (`TKit1`), on
2026-10-25. Values are after `--reset` and are percentages of block time.

| Scenario | now % | peak % | voices % | mixer % | fx % | xruns |
|----------|------:|-------:|---------:|--------:|-----:|------:|
| Idle, sequencer stopped | 56.9 | 57.4 | 51.6 | 5.1 | 0.2 | 0 |
| 7 voices retriggered on 16ths at 120 BPM | 57.9 | 59.7 | 52.6 | 5.1 | 0.2 | 0 |
| 7 voices retriggered every 20 ms | 58.7 | 59.7 | 53.4 | 5.1 | 0.2 | 0 |
| 7 voices retriggered every 5 ms | 58.8 | 59.9 | 53.4 | 5.2 | 0.2 | 0 |

The boot-time peak was 65.9% with 78 xruns before reset; this likely includes
boot/kit-load work such as SD access, so it is not a steady-state measurement.
The tested kit does not use FX. Voices render continuously even when not being
triggered, so the load stays nearly flat at about 57–60%, leaving about 40%
headroom. Sequencer playback was not measured because MIDI Start is ignored
with the current sync settings. Delay and reverb loads are in
[Plate reverb](#plate-reverb). 300 BPM and SD-save loads remain to be
measured.

Firmware: `2f179b5` · Image SHA-256: `46b8f317…` · Date: 2026-10-25 · Kit: `TKit1` (slot 16)

## Sequencer condition storage estimate

Packed conditions add 168 bytes per pattern (10,752 bytes across 64 patterns),
plus a 168-byte backup copy and about 45 bytes of cycle/repeat counters. Against
the FM branch's 279,000-byte general-RAM baseline, that is about 289,965 bytes
(73.7% of 384 KiB), before linker alignment. The pattern table and backup are
ordinary `.bss`, not the tightly budgeted ITCM/DTCM sections. Sequencer
conditions add a small comparison to the existing step probability/ratchet
path; their CPU cost has not been measured on hardware. Repeat changes
sequencer indices only and does not add an audio FX algorithm or audio-buffer
allocation. It adds some code to the ITCM-resident sequencer tick; the local
release toolchain is missing newlib's `nano.specs`, so its exact ITCM growth
could not be measured. See the pending
[hardware checklist](sequencer-features.md#hardware-validation-checklist).

## Plate reverb

The reverb ([reverb.md](reverb.md)) is counted in **fx**.

### Measured

Measured on the user's LXR-02 over USB with the PR #3 release image
(`LXR_V2_V1.7-sx.img`, SHA-256 `d90e572d…`), kit slot 16 (`TKit1`, FX normally
off), on 2026-10-25. All 7 voices were retriggered on 16ths at 120 BPM during
each 6 s window, and the counters were reset for each row. FX dry/wet was 50,
P1–P4 were 20/100/64/64. Values are percentages of block time.

| Setting | now % | peak % | voices % | mixer % | fx % | xruns |
|---------|------:|-------:|---------:|--------:|-----:|------:|
| FX off | 57.9 | 59.1 | 52.6 | 5.1 | 0.2 | 0 |
| FX `del` | 61.0 | 62.3 | 52.6 | 5.1 | 3.2 | 0 |
| FX `rev`, type `off` | 58.3 | 59.4 | 52.6 | 5.1 | 0.5 | 0 |
| FX `rev`, type `Plt` | 69.3 | 70.3 | 52.6 | 5.1 | 11.5 | 0 |
| FX `rev`, type `Eco` | 64.4 | 66.6 | 52.6 | 5.1 | 6.7 | 0 |

| Switching test (`PARAM_SET`, 20 switches 150 ms apart) | peak % | xruns |
|--------------------------------------------------------|-------:|------:|
| FX type `del` ↔ `rev` (type `Plt`) | 78.0 | 0 |
| Reverb type `Plt` ↔ `Eco` | 72.4 | 0 |

The reverb's cost does not depend on how many voices are routed to the FX bus,
because the tank runs on every block. `Plt` costs about 11.3 points over FX off
and leaves about 30% headroom. `Eco` costs about 6.5 points. The one-off buffer
clears on a type change raise the peak, but did not cause xruns.

Firmware: `d30f967` · Image SHA-256: `d90e572d…` · Date: 2026-10-25 · Kit: `TKit1` (slot 16)

### Estimate from the code

Before the measurement, the cost was estimated from the disassembly of the
release build (GCC 15.3, `-O3`). The measurement is about 1.5–2 points above
the estimate for `Plt` and about 1 point above it for `Eco`.

| Reverb type | Instructions | Estimated cycles / block | Estimated fx % |
|-------------|-------------|--------------------------|----------------|
| `off` (dry passthrough) | ≈4 per sample | ≈0.6 k | ≈0.2 (same as FX off) |
| `Plt` | 348 per frame (tank) + ≈30 (dry/wet mix) | 24–31 k | ≈8–10 |
| `Eco` | 365 per 2 frames + ≈30 per frame | 14–18 k | ≈4.5–6 |
| Selecting the reverb or changing its type (once) | 40 KB clear | ≈10 k | +3 in that block |
| Reverb → delay switch (once) | 64 KB clear | ≈16 k | +5 in that block |
| Delay → reverb switch (once) | 64 KB + 40 KB clear | ≈26 k | +8 in that block |

Cycles assumed 1.0–1.3 cycles per instruction (Cortex-M7 dual issue; code in
flash behind the I-cache, delay lines and state in DTCM).

## Freeze

The freeze / beat repeat ([freeze.md](freeze.md)) is counted in **fx**.

### Measured

To be measured on hardware, with the same setup as the reverb rows above (kit
`TKit1`, 7 voices on 16ths at 120 BPM, counters reset for each row), FX type
`frz`, `d/w` 100, P1–P4 = 64/64/127/127 (1/16, original pitch, no decay, no
gate). "Engaged" rows: hold perf step 13 (1/16) for the whole window.

| Setting | now % | peak % | voices % | mixer % | fx % | xruns |
|---------|------:|-------:|---------:|--------:|-----:|------:|
| FX `frz`, not engaged | | | | | | |
| FX `frz`, engaged 1/16, FX output stereo | | | | | | |
| FX `frz`, engaged 1/32T (most crossfades), stereo | | | | | | |
| FX `frz`, engaged 1/16, `pit` 127 (tape stop) | | | | | | |
| FX `frz`, engaged 1/4, FX output mono (single output) | | | | | | |

| Switching test | peak % | xruns |
|----------------|-------:|------:|
| Engage/release 1/16 every 150 ms (SysEx `FX_FREEZE`) | | |
| FX type `frz` ↔ `rev` (type `Plt`), 20 switches 150 ms apart | | |

Firmware: · Image SHA-256: · Date: · Kit:

### Estimate from the code

From the disassembly of the release build (GCC 15.3, `-O3`). Per stereo frame:
recording ≈10 instructions, clearing/fading the live part ≈5–24, each playing
repeat (fp16 read, linear interpolation for the pitch, gain ramp) 43 (mono
output: 29), the dry/wet mix with saturation 43. Code in flash behind the
I-cache, buffer and state in DTCM, 1.0–1.3 cycles per instruction.

| State | Instructions per frame | Estimated cycles / block | Estimated fx % |
|-------|------------------------|--------------------------|----------------|
| Not engaged (passthrough + recording) | ≈35 (mono output: 19) | 2.2–2.9 k | ≈0.7–0.9 |
| Engaged, one repeat playing | ≈100 | 6.4–8.3 k | ≈2–2.7 |
| Engaged, 128-frame crossfade (two repeats + live fade) | ≈165 | 10.5–13.7 k | ≈3.4–4.4 in that block, 1–2 blocks per repeat |
| `pit` above 64 (tape stop) | +14 | +0.9–1.2 k | +0.3–0.4 |
| Selecting `frz` | no buffer clear | — | — |

At 1/32T and 120 BPM a repeat lasts 1839 frames (29 blocks), so 2 of 29 blocks
have the crossfade cost and the average stays ≈2.5–3 %.

## Hardware test checklist

- [ ] Flash `build/release/LXR_V2_V1.7-sx.img` with the bootloader. The unit
      boots and plays audio as before.
- [ ] Global menu: the encoder steps through `syx id ech` and then on to
      `cpu pk vce mix` and `fx xrn`. The entries before them still work.
- [ ] The CPU screen updates about 4× per second and the values look plausible
      (they rise when voices play or FX are on).
- [ ] Encoder click shows `CPU` + full name and the value with `%`. Turning the
      encoder on `pk`/`xrn` clears them. On the other entries it changes
      nothing.
- [x] `lxr_status.py --port LXR` reports CPU values over USB.
- [x] `IDENTITY_REPLY` capability bits = `0x17F` (bits 0–6 and 8 set).
- [x] `--reset` clears peak and xruns.
- [x] `STATUS_REQUEST` reports kit 16 at boot; `KIT_REQUEST` for slot 16
      returns the kit name `TKit1`.
- [x] `KIT_DUMP` round trip is byte-exact; `GLOBALS` returns 33 values and
      `PARAM_GET` works.
- [ ] Verify `lxr_status.py` values match the LCD. The LCD page has not been
      tested yet.
- [ ] Test the LCD page and encoder resets.
- [ ] Power-cycle after changing the kit from the panel.
- [ ] xruns stay at 0 during normal playback. Note whether they rise during
      SD saves, kit loads or heavy MIDI input.
- [ ] Fill in the hardware numbers table above.

## Memory use of the meter

From `--print-memory-usage` of the release build:

| Region | Before | After | Change |
|--------|-------:|------:|-------:|
| ITCM | 15136 B (92.38 %) | 15224 B (92.92 %) | +88 B |
| DTCM | 107392 B (81.93 %) | 107392 B (81.93 %) | 0 |
| RAM | 278912 B (70.93 %) | 279008 B (70.96 %) | +96 B |
| FLASH | 240900 B (23.72 %) | 243052 B (23.93 %) | +2152 B |
