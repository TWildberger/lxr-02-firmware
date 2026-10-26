/*
 * usb_manager.h
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 *
 *
 *  Created on: 26.10.2012
 *      Author: Julian
 */

#ifndef USB_MANAGER_H_
#define USB_MANAGER_H_
#include "Soundengine/MIDI/MidiMessages.h"




#include "stm32f7xx_hal.h"
#include "usbd_cdc.h"



//void usb_init();
//void usb_stop();
//void usb_start();
void usb_tick();

void usb_sendMidi(MidiMsg msg);
uint8_t usb_getMidi(MidiMsg* msg);
void usb_flushMidi();
// 1 when enumerated by a host
uint8_t usb_isConfigured(void);
// queues a complete F0 ... F7 frame as USB MIDI SysEx packets.
// returns 0 (nothing queued) when there is not enough room, retry later.
// returns 1 without queueing when the host has stopped reading (frame dropped)
uint8_t usb_sendSysex(const uint8_t *frame, uint16_t len);

#endif /* USB_MANAGER_H_ */


