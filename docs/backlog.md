# Firmware backlog

Ideas collected while adding the SysEx protocol ([sysex.md](sysex.md)). Checked items are done. Each item should become its own PR and be tested on hardware. (This repo has GitHub Issues turned off, so the list lives here.)

## Device UI / workflow
- [ ] Make kit/pattern browsing quicker: show names while scrolling, and preview a kit before loading it.
- [ ] Add a "save kit to same slot" shortcut and a confirm/undo step for overwrites.
- [ ] Copy/paste whole voices between kits and between voices (sound parameters only).
- [ ] Add a knob pickup vs. jump mode, with the stored value shown next to the knob position.
- [ ] Add compare/revert for the edit buffer against the stored kit.
- [ ] Show a brief "SysEx" indicator when a host writes data.

## Sound
- [x] **Enable the hidden plate reverb.** Done: FX type `rev`, see [reverb.md](reverb.md).
- [ ] Reverb: per-voice send levels (today a voice is either routed to the FX bus or not), or a second FX slot so the reverb can run with another FX.
- [ ] Reverb: when FX type `rev` is selected on the front panel while the reverb type is `off`, switch the type to `Plt` automatically. It must not do this on kit load, where `off` keeps old FX-type-5 kits dry.
- [ ] Reverb: a size/diffusion parameter (needs a new kit parameter or a sub-menu), and crossfade pre-delay changes. Today a pre-delay change jumps and can click in the wet signal.
- [ ] Remove the now-unused `TiltEq.c` (was only used by the old reverb).
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
