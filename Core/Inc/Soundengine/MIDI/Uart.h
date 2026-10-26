/*
 * MidiUart.h
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */


#ifndef MIDIUART_H_
#define MIDIUART_H_

#include "Soundengine/MIDI/MidiMessages.h"

#define ACK 1
#define NACK -1

extern uint8_t midi_process_block;

void initMidiUart();

void uart_sendMidi(MidiMsg msg);

void uart_sendMidiByte(uint8_t data);

// SysEx output: Begin returns 0 while older MIDI bytes are still queued. Between
// Begin and End other bytes (except realtime) are held back. Put returns 0 when
// the TX FIFO is full (retry later).
uint8_t uart_sysexTxBegin(void);
uint8_t uart_sysexTxPut(uint8_t data);
void uart_sysexTxEnd(void);
// moves held back bytes to the TX FIFO, call regularly from the main loop
void uart_txPoll(void);
//check if there is some received data in the Rx buffer and send it to the midi parser
void uart_processMidi();

void initFrontpanelUart();
//check if there is data in the front panel Rx buffer and send it to the front panel parser
void uart_processFront();
//sends a byte to the frontpanel
void uart_sendFrontpanelByte(uint8_t data);
//send sysex data to the frontpanel
//unlike the sendFrontPanelByte function, this function is allowed to send data whilke sysex mode is active
void uart_sendFrontpanelSysExByte(uint8_t data);

void uart_clearFrontFifo();
#endif /* MIDIUART_H_ */
