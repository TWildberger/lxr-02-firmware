/*
 * MidiUart.c
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#include <Soundengine/MIDI/CortexParser.h>
#include "stm32f7xx_hal.h"
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/MIDI/Uart.h"
#include "Soundengine/FIFO.h"
#include "Soundengine/config.h"
#include "main.h"

#include <stdint.h>

 /* USART2 MIDI configured as follow:
	 - BaudRate = 31250 baud
	 - Word Length = 8 Bits
	 - One Stop Bit
	 - No parity
	 - Hardware flow control disabled (RTS and CTS signals)
	 - Receive and transmit enabled
*/
//-----------------------------------------------------------------------------
//we need a TX and RX FIFO for every uart

uint8_t midi_process_block = 0;

static Fifo fifo_midiTx;
static Fifo fifo_midiRx;

static Fifo fifo_frontTx;
static FifoBig fifo_frontRx; //we use a bigger fifo here because we have lots of data coming in for the preset
						//todo test if necessary!
//-----------------------------------------------------------------------------
void uart_clearFrontFifo()
{
	fifo_clear(&fifo_frontTx);
	fifoBig_clear(&fifo_frontRx);
}
//-----------------------------------------------------------------------------
static uint8_t uart_txRefillFromDeferred(void);
//-----------------------------------------------------------------------------
void USART3_IRQHandler(void)
{
	uint32_t isrflags = READ_REG((&huart3)->Instance->ISR);
	uint8_t data = 0;
	
  //if Receive interrupt (rx not empty)
	__HAL_UART_CLEAR_OREFLAG(&huart3);

	if ((isrflags & USART_ISR_RXNE) != RESET)
	{
		// UART_Receive_IT(huart);
		data = (uint8_t)(USART3->RDR & (uint16_t)0xFF);

		// put the received data in the RX midi buffer
    // fifo_bufferIn(&fifo_midiRx, data);
    if (fifo_bufferIn(&fifo_midiRx, data) == 0)
    {
    }
    //  if (!midi_process_block)
    //  {
    //    uart_processMidi();
    //  }
	}

	//uint8_t test = ;// __HAL_UART_GET_IT(&huart2, UART_IT_TXE);
	if ((isrflags & USART_ISR_TXE) != RESET)
	{
    if (fifo_bufferOut(&fifo_midiTx,&data) == 1) // If there is data in the output buffer to send
    {
      USART3->TDR = (data & (uint16_t)0x01FF);
    }
    else if (uart_txRefillFromDeferred() && fifo_bufferOut(&fifo_midiTx,&data) == 1)
    {
      // bytes held back during a SysEx frame
      USART3->TDR = (data & (uint16_t)0x01FF);
    }
    else // If buffer empty
    {
      // Disable Transmit Data Register empty interrupt
      __HAL_UART_DISABLE_IT(&huart3,UART_IT_TXE);
    }
	}
}

//-----------------------------------------------------------------------------
void uart_processMidi()
{
	uint8_t data;
	while(fifo_bufferOut(&fifo_midiRx, &data))
	{
		midiParser_parseUartData(data);
	}
}

void uart_sendMidi(MidiMsg msg)
{
	uart_sendMidiByte(msg.status);
	if(msg.bits.length) {
		uart_sendMidiByte(msg.data1);
		if(msg.bits.length > 1)
			uart_sendMidiByte(msg.data2);
	}

}
//-----------------------------------------------------------------------------
// While a SysEx frame is being sent, other MIDI bytes (except realtime) wait in
// fifo_midiDeferred so they are not inserted into the frame.
static Fifo fifo_midiDeferred;
static volatile uint8_t uart_sysexTxActive = 0;

static inline uint8_t uart_fifoFull(const Fifo *fifo)
{
	return ((fifo->write + 1) & BUFFER_MASK) == fifo->read;
}
static inline uint8_t uart_fifoEmpty(const Fifo *fifo)
{
	return fifo->read == fifo->write;
}
static inline uint8_t uart_fifoUsed(const Fifo *fifo)
{
	return (uint8_t)((fifo->write - fifo->read) & BUFFER_MASK);
}
// SysEx may only fill this much of fifo_midiTx so realtime bytes (clock) are
// never dropped and wait for at most a few ms
#define UART_SYSEX_TX_MAX_QUEUED	16
// interrupts must be disabled
static void uart_moveDeferred(void)
{
	uint8_t data;
	while(!uart_fifoEmpty(&fifo_midiDeferred) && !uart_fifoFull(&fifo_midiTx))
	{
		fifo_bufferOut(&fifo_midiDeferred, &data);
		fifo_bufferIn(&fifo_midiTx, data);
	}
	if(!uart_fifoEmpty(&fifo_midiTx))
		__HAL_UART_ENABLE_IT(&huart3, UART_IT_TXE);
}
// called from the TXE interrupt when fifo_midiTx ran empty
static uint8_t uart_txRefillFromDeferred(void)
{
	uint8_t moved = 0;
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	if(!uart_sysexTxActive && !uart_fifoEmpty(&fifo_midiDeferred))
	{
		uart_moveDeferred();
		moved = 1;
	}
	__set_PRIMASK(primask);
	return moved;
}
//-----------------------------------------------------------------------------
void uart_sendMidiByte(uint8_t data)
{
	// called from the main loop and from interrupts
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();

	if(data < 0xF8 && (uart_sysexTxActive || !uart_fifoEmpty(&fifo_midiDeferred)))
	{
		fifo_bufferIn(&fifo_midiDeferred, data);
	}
	else
	{
		//put data in the output fifo
		fifo_bufferIn(&fifo_midiTx,data);
		//enable Transmit Data Register empty interrupt
		__HAL_UART_ENABLE_IT(&huart3, UART_IT_TXE);
	}

	__set_PRIMASK(primask);
};
//-----------------------------------------------------------------------------
void uart_txPoll(void)
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	if(!uart_sysexTxActive) uart_moveDeferred();
	__set_PRIMASK(primask);
}
//-----------------------------------------------------------------------------
uint8_t uart_sysexTxBegin(void)
{
	uint8_t ok = 0;
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	if(!uart_sysexTxActive) uart_moveDeferred();
	if(uart_fifoEmpty(&fifo_midiDeferred))
	{
		uart_sysexTxActive = 1;
		ok = 1;
	}
	__set_PRIMASK(primask);
	return ok;
}
//-----------------------------------------------------------------------------
uint8_t uart_sysexTxPut(uint8_t data)
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	uint8_t ok = 0;
	if(uart_fifoUsed(&fifo_midiTx) < UART_SYSEX_TX_MAX_QUEUED)
		ok = fifo_bufferIn(&fifo_midiTx, data);
	__HAL_UART_ENABLE_IT(&huart3, UART_IT_TXE);
	__set_PRIMASK(primask);
	return ok;
}
//-----------------------------------------------------------------------------
void uart_sysexTxEnd(void)
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	uart_sysexTxActive = 0;
	uart_moveDeferred();
	__set_PRIMASK(primask);
}
//-----------------------------------------------------------------------------
void uart_processFront()
{
#if UART_DEBUG_ECHO_MODE
	uint8_t data;
	if(fifoBig_bufferOut(&fifo_frontRx,&data))
	{
		//echo back received data
		uart_sendFrontpanelByte(data);
	}
#else
	uint8_t data;
	if(fifoBig_bufferOut(&fifo_frontRx,&data))
	{
		frontParser_parseUartData(data);
	}
#endif
}
//-----------------------------------------------------------------------------
#include "frontPanelParser_ui.h"
void uart_sendFrontpanelByte(uint8_t data)
{

#if 1
	frontPanel_parseData(data);
#else
	//do not send anything besides sysex data while sysex mode is active!
	if(frontParser_sysexActive == 0)
	{
		//put data in the output fifo
		fifo_bufferIn(&fifo_frontTx,data);

		//enable Transmit Data Register empty interrupt
	//	USART_ITConfig(USART3, USART_IT_TXE, ENABLE);
	}
#endif
};
//-----------------------------------------------------------------------------
void uart_sendFrontpanelSysExByte(uint8_t data)
{
	//put data in the output fifo
	fifo_bufferIn(&fifo_frontTx,data);

	//enable Transmit Data Register empty interrupt
	//USART_ITConfig(USART3, USART_IT_TXE, ENABLE);
}
//-----------------------------------------------------------------------------
void initMidiUart()
{
	//init the fifo
	fifo_init(&fifo_midiTx);
	fifo_init(&fifo_midiDeferred);
	fifo_init(&fifo_midiRx);

	/*
	 * UART2, APB1
	 *
	 */
#if 0

	USART_InitTypeDef USART_InitStructure;
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_ClockInitTypeDef USART_ClockInitStructure;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE); // Enable GPIOA on APB1 peripheral clock
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE); // Enable USART2 Clock

    //Set USART2 Rx (PA.03) as AF push-pull
    GPIO_InitStructure.GPIO_Pin 	= GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode 	= GPIO_Mode_AF ;
    GPIO_InitStructure.GPIO_Speed 	= GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType	= GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd	= GPIO_PuPd_DOWN;

    GPIO_Init(GPIOA, &GPIO_InitStructure);

    //TX pin
    GPIO_InitStructure.GPIO_Pin 	= GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode 	= GPIO_Mode_AF ;
    GPIO_InitStructure.GPIO_Speed 	= GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType	= GPIO_OType_OD;
    GPIO_InitStructure.GPIO_PuPd	= GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    //configure AF
    GPIO_PinAFConfig(GPIOA,GPIO_PinSource2 ,GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA,GPIO_PinSource3 ,GPIO_AF_USART2);

    //uart clock
    USART_ClockStructInit(&USART_ClockInitStructure);
    USART_ClockInit(USART2, &USART_ClockInitStructure);

    //init the uart
	USART_InitStructure.USART_BaudRate = 31250;
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	USART_InitStructure.USART_Parity = USART_Parity_No;
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

	USART_Init(USART2, &USART_InitStructure); // Now do the setup

	//Enable USART2
	USART_Cmd(USART2, ENABLE);

	//Enable inmterrupt
	//configure NVIC
	NVIC_InitTypeDef NVIC_InitStructure;
	//select NVIC channel to configure
	NVIC_InitStructure.NVIC_IRQChannel = USART2_IRQn;
	//set priority to lowest
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0x0F;
	//set subpriority to lowest
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0x0F;
	//enable IRQ channel
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	//update NVIC registers
	NVIC_Init(&NVIC_InitStructure);
	//disable Transmit Data Register empty interrupt
	USART_ITConfig(USART2, USART_IT_TXE, DISABLE);
	//enable Receive Data register not empty interrupt
	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
#endif
}
//-------------------------------------------------------------------------------------------

void initFrontpanelUart()
{
	//fifo init
	fifo_init(&fifo_frontTx);
	fifoBig_init(&fifo_frontRx);
	/*
	 * UART3, APB1
	 * (PB10 TX, PB11 RX)
	 *
	 */

#if 0

	USART_InitTypeDef USART_InitStructure;
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_ClockInitTypeDef USART_ClockInitStructure;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE); // Enable GPIOD on APB1 peripheral clock
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE); // Enable USART3 Clock

    //Set USART3 Rx (PB.11) as AF push-pull
    GPIO_InitStructure.GPIO_Pin 	= GPIO_Pin_11;
    GPIO_InitStructure.GPIO_Mode 	= GPIO_Mode_AF ;
    GPIO_InitStructure.GPIO_Speed 	= GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType	= GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd	= GPIO_PuPd_DOWN;

    GPIO_Init(GPIOB, &GPIO_InitStructure);

    //TX pin
    GPIO_InitStructure.GPIO_Pin 	= GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Mode 	= GPIO_Mode_AF ;
    GPIO_InitStructure.GPIO_Speed 	= GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType	= GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd	= GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    //configure AF
    GPIO_PinAFConfig(GPIOB,GPIO_PinSource11 ,GPIO_AF_USART3);
    GPIO_PinAFConfig(GPIOB,GPIO_PinSource10,GPIO_AF_USART3);

    //uart clock
    USART_ClockStructInit(&USART_ClockInitStructure);
    USART_ClockInit(USART3, &USART_ClockInitStructure);

    //init the uart
	//USART_InitStructure.USART_BaudRate = 38400UL;
    //USART_InitStructure.USART_BaudRate = 1000000UL;
    USART_InitStructure.USART_BaudRate = 500000UL;

	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	USART_InitStructure.USART_Parity = USART_Parity_No;
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

	USART_Init(USART3, &USART_InitStructure); // Now do the setup

	//Enable USART3
	USART_Cmd(USART3, ENABLE);

	//Enable inmterrupt
	//configure NVIC
	NVIC_InitTypeDef NVIC_InitStructure;
	//select NVIC channel to configure
	NVIC_InitStructure.NVIC_IRQChannel = USART3_IRQn;
	//set priority to lowest
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0x0F;
	//set subpriority to lowest
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0x0F;
	//enable IRQ channel
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	//update NVIC registers
	NVIC_Init(&NVIC_InitStructure);
	//disable Transmit Data Register empty interrupt
	USART_ITConfig(USART3, USART_IT_TXE, DISABLE);
	//enable Receive Data register not empty interrupt
	USART_ITConfig(USART3, USART_IT_RXNE, ENABLE);
#endif
}
