/*
 * usb_manager.c
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
 *      Author: Julian Schmidt
 */
#include "Soundengine/USB/usb_manager.h"
#include <string.h>
#include "usbd_cdc_if.h"
extern USBD_HandleTypeDef hUsbDeviceFS;




enum {
	USB_NOT_DETECTED,
	USB_DETECTED,
};
//USBD_HandleTypeDef           USB_OTG_dev;
//-------------------------------------------------------------------------------
#if 0
void usb_init()
{
#if HAL_USB
	  if (USBD_Init(&hUsbDeviceFS, &FS_Desc, DEVICE_FS) != USBD_OK)
	  {
	    Error_Handler();
	  }
	  if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_CDC) != USBD_OK)
	  {
	    Error_Handler();
	  }
	  if (USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_Interface_fops_FS) != USBD_OK)
	  {
	    Error_Handler();
	  }
	  if (USBD_Start(&hUsbDeviceFS) != USBD_OK)
	  {
	    Error_Handler();
	  }
#else

	  usb_start();
#endif
}
//-------------------------------------------------------------------------------
void usb_stop()
{
	//stop usb port
	USBD_DeInit(&USB_OTG_dev);
}
#endif
//-------------------------------------------------------------------------------
void usb_tick()
{
	usb_flushMidi();
}
//-------------------------------------------------------------------------------
#if 0
void usb_start()
{
	//zero init the usb receive buffer
	memset(usb_MidiMessages,0, sizeof(MidiMsg)*USB_MIDI_INPUT_BUFFER_SIZE);

	//start usb port
	 USBD_Init(&USB_OTG_dev,
	            &USR_desc,
				DEVICE_FS);

	 if (USBD_RegisterClass(&USB_OTG_dev, &MIDI_cb) != USBD_OK)

	 // if (USBD_CDC_RegisterInterface(&USB_OTG_dev, &USBD_Interface_fops_FS) != USBD_OK)
	  {
		//Error_Handler();
	  }
	  if (USBD_Start(&USB_OTG_dev) != USBD_OK)
	  {
		//Error_Handler();
	  }
}
#endif
//-------------------------------------------------------------------------------
// USB MIDI TX ring of 4 byte event packets, filled from the main loop and from
// interrupts, sent by usb_flushMidi() in blocks of up to 64 bytes.
#define USB_TX_RING_SIZE	1024
#define USB_TX_RING_MASK	(USB_TX_RING_SIZE - 1)
#define USB_TX_BLOCK_SIZE	64

static uint8_t usb_txRing[USB_TX_RING_SIZE];
static volatile uint16_t usb_txWr = 0;
static volatile uint16_t usb_txRd = 0;
static uint8_t usb_txStage[USB_TX_BLOCK_SIZE];
// Set when the host stops reading the IN endpoint (e.g. no app has the port
// open). Data is dropped until a transfer completes, so nothing waits on USB.
#define USB_TX_STALL_MS		100
static volatile uint8_t usb_txStalled = 0;
static uint32_t usb_txStartTick = 0;

static inline uint16_t usb_txFree(void)
{
	return (uint16_t)((usb_txRd - usb_txWr - 1) & USB_TX_RING_MASK);
}
// interrupts must be disabled
static inline void usb_txPut4(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
	uint16_t wr = usb_txWr;
	usb_txRing[wr] = b0; wr = (wr + 1) & USB_TX_RING_MASK;
	usb_txRing[wr] = b1; wr = (wr + 1) & USB_TX_RING_MASK;
	usb_txRing[wr] = b2; wr = (wr + 1) & USB_TX_RING_MASK;
	usb_txRing[wr] = b3; wr = (wr + 1) & USB_TX_RING_MASK;
	usb_txWr = wr;
}
//-------------------------------------------------------------------------------
uint8_t usb_isConfigured(void)
{
	return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED && hUsbDeviceFS.pClassData != NULL;
}
//------------------------------------------------------------------------------
/*
 * send out the next block of queued packets
 * called periodically by usb_tick()
 */
void usb_flushMidi()
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();

	if(!usb_isConfigured())
	{
		// nobody listening, drop queued data
		usb_txRd = usb_txWr;
		usb_txStalled = 0;
	}
	else if(((USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassData)->TxState != 0)
	{
		if(HAL_GetTick() - usb_txStartTick > USB_TX_STALL_MS)
		{
			usb_txStalled = 1;
			usb_txRd = usb_txWr;
		}
	}
	else if(usb_txRd != usb_txWr)
	{
		usb_txStalled = 0;
		uint16_t rd = usb_txRd;
		uint16_t n = 0;
		while(rd != usb_txWr && n < USB_TX_BLOCK_SIZE)
		{
			usb_txStage[n++] = usb_txRing[rd];
			rd = (rd + 1) & USB_TX_RING_MASK;
		}
		if(CDC_Transmit_FS(usb_txStage, n) == USBD_OK)
		{
			usb_txRd = rd;
			usb_txStartTick = HAL_GetTick();
		}
	}
	else
	{
		usb_txStalled = 0;
	}

	__set_PRIMASK(primask);
}
//------------------------------------------------------------------------------
void usb_sendMidi(MidiMsg msg)
{
	//MIDI byte[0] = 4 bits cable number + 4 bits event type code
	//then the 3 midi message bytes
	//USB MIDI msg has to be ALWAYS 4 bytes long
	//even if it is just a single clock byte
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	if(!usb_txStalled && usb_txFree() >= 4)
		usb_txPut4(msg.status>>4, msg.status, msg.data1, msg.data2);
	__set_PRIMASK(primask);
}
//------------------------------------------------------------------------------
uint8_t usb_sendSysex(const uint8_t *frame, uint16_t len)
{
	if(len == 0) return 1;
	const uint16_t needed = (uint16_t)(((len + 2) / 3) * 4);
	if(needed > USB_TX_RING_SIZE - 1) return 1; // can never fit, drop

	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	if(usb_txStalled)
	{
		// host is not reading, drop the frame
		__set_PRIMASK(primask);
		return 1;
	}
	if(usb_txFree() < needed)
	{
		__set_PRIMASK(primask);
		return 0;
	}
	// cable 0, CIN 4 = SysEx start/continue, CIN 5/6/7 = SysEx ends with 1/2/3 bytes
	uint16_t i = 0;
	while(len - i > 3)
	{
		usb_txPut4(0x04, frame[i], frame[i + 1], frame[i + 2]);
		i += 3;
	}
	switch(len - i)
	{
	case 1: usb_txPut4(0x05, frame[i], 0, 0); break;
	case 2: usb_txPut4(0x06, frame[i], frame[i + 1], 0); break;
	default: usb_txPut4(0x07, frame[i], frame[i + 1], frame[i + 2]); break;
	}
	__set_PRIMASK(primask);
	return 1;
}
//-------------------------------------------------------------------------------
uint8_t usb_getMidi(MidiMsg* msg)
{
	if(usb_MidiMessagesRead != usb_MidiMessagesWrite)
	{
		//we have unprocessed messages in the queue
		*msg = usb_MidiMessages[usb_MidiMessagesRead];

		usb_MidiMessagesRead++;
		usb_MidiMessagesRead &= USB_MIDI_INPUT_BUFFER_MASK;

		return 1; //return OK
	}
return 0; //no messages
}

