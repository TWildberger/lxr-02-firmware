/*
 * SysexHandler.h
 *
 * LXR-02 SysEx protocol (docs/sysex.md): receive rings for DIN and USB MIDI,
 * request handling, replies and parameter echo.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */
#ifndef SYSEX_HANDLER_H_
#define SYSEX_HANDLER_H_

#include <stdint.h>

void sysex_init(void);
// main loop: handles received frames, sends replies and notifications
void sysex_tick(void);

// DIN MIDI parser hook, called for every parsed message (ISR or main loop).
// isSysexByte = 1 for SysEx data bytes (status then holds the data byte).
void sysex_rxDinMessage(uint8_t status, uint8_t isSysexByte);
// USB MIDI SysEx packet payload (1-3 bytes), called from the USB ISR
void sysex_rxBytesUsb(const uint8_t *bytes, uint8_t n);

// marks a parameter as changed by incoming MIDI (CC/NRPN), so it is not echoed
void sysex_markExternalChange(uint16_t paramNr);

#endif /* SYSEX_HANDLER_H_ */
