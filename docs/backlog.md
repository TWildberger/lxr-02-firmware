# Firmware backlog

Ideas collected while adding the SysEx protocol ([sysex.md](sysex.md)). None of these are implemented yet. Each item should become its own PR and be tested on hardware. (This repo has GitHub Issues turned off, so the list lives here.)

## Device UI / workflow
- [ ] Make kit/pattern browsing quicker: show names while scrolling, and preview a kit before loading it.
- [ ] Add a "save kit to same slot" shortcut and a confirm/undo step for overwrites.
- [ ] Copy/paste whole voices between kits and between voices (sound parameters only).
- [ ] Add a knob pickup vs. jump mode, with the stored value shown next to the knob position.
- [ ] Add compare/revert for the edit buffer against the stored kit.
- [ ] Show a brief "SysEx" indicator when a host writes data.

## Sound
- [ ] **Enable the hidden plate reverb.** `Core/Src/Soundengine/FxBus.c` has a `USE_REVERB` path (`Reverb_init`/`Reverb_process`, `FX_REVERB`), and `PAR_FX_REVERB_TYPE` exists in `Parameters.h`. Check CPU and memory headroom first: in the `-sx` build ITCM is at 92% and DTCM at 82%. Then add a menu entry and regenerate `params.json`.
- [ ] Add per-voice velocity curves.

## Sequencer
- [ ] Per-step probability / conditional trigs.
- [ ] Per-track ratchets (sub-step repeats) with a UI.
- [ ] Pattern-chain editor and song-mode improvements.
- [ ] Undo for step edits; copy/paste for a bar range.

## SysEx protocol follow-ups (v1 limits)
- [ ] Notify pattern/step edits, mutes and transport changes. For now the host has to re-request the pattern or `STATUS`.
- [ ] Send `KIT_CHANGED` with the right slot after a MIDI bank change (CC 0). That path loads the kit without updating `menu_loadedPresetNr`, so the event carries the previous slot number.
- [ ] Add project/song dumps.
- [ ] Add an option to forward USB SysEx to DIN only when it isn't addressed to the LXR. Today all USB SysEx is forwarded raw when usb2midi routing is on, which is the stock behaviour.

## MIDI robustness (found while reading the code)
- [ ] `uart_processMidi()` runs both from the TIM2 ISR and from the main loop. If the ISR interrupts the main loop mid-message, the shared parser state can be corrupted. Run it in only one context.
- [ ] MIDI thru of SysEx forwards each byte as a separate message. On USB those bytes get CIN = status>>4, which is wrong for SysEx data. Forward them as proper SysEx packets.
