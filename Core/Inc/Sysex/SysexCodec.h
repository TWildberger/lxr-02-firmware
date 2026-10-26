/*
 * SysexCodec.h
 *
 * Framing, 7-bit encodings and stream assembly for the LXR-02 SysEx protocol
 * (docs/sysex.md). Hardware independent, so it is also built by the host unit
 * tests in tests/host.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */
#ifndef SYSEX_CODEC_H_
#define SYSEX_CODEC_H_

#include <stdint.h>

#define SX_MANUFACTURER_ID		0x7D	// MIDI non-commercial / educational
#define SX_MODEL_ID				0x4C	// 'L'
#define SX_PROTOCOL_VERSION		0x01
#define SX_DEVICE_BROADCAST		0x7F
#define SX_MAX_FRAME			320		// F0 ... F7 inclusive
#define SX_HEADER_LEN			6		// F0 7D 4C dd vv cc
#define SX_MIN_FRAME			8		// header + ss + F7

#define SX_FLAG_SAVE			0x01

enum SxCommand {
	SX_CMD_IDENTITY_REQ		= 0x01,
	SX_CMD_IDENTITY_REPLY	= 0x02,
	SX_CMD_STATUS_REQ		= 0x03,
	SX_CMD_STATUS_REPLY		= 0x04,
	SX_CMD_KIT_REQ			= 0x10,
	SX_CMD_KIT_DUMP			= 0x11,
	SX_CMD_KIT_STORE		= 0x12,
	SX_CMD_KIT_LOAD			= 0x13,
	SX_CMD_KIT_NAME			= 0x14,
	SX_CMD_PATTERN_REQ		= 0x20,
	SX_CMD_PATTERN_CHUNK	= 0x21,
	SX_CMD_PATTERN_NAME		= 0x22,
	SX_CMD_GLOBALS_REQ		= 0x30,
	SX_CMD_GLOBALS_DUMP		= 0x31,
	SX_CMD_PARAM_GET		= 0x40,
	SX_CMD_PARAM_SET		= 0x41,
	SX_CMD_PARAM_VALUE		= 0x42,
	SX_CMD_PARAM_NOTIFY		= 0x43,
	SX_CMD_ECHO_SET			= 0x44,
	SX_CMD_EVENT			= 0x45,
	SX_CMD_ACK				= 0x7E,
	SX_CMD_NAK				= 0x7F,
};

enum SxError {
	SX_ERR_CHECKSUM		= 0x01,
	SX_ERR_UNKNOWN_CMD	= 0x02,
	SX_ERR_LENGTH		= 0x03,
	SX_ERR_RANGE		= 0x04,
	SX_ERR_BUSY			= 0x05,
	SX_ERR_SEQUENCE		= 0x06,
	SX_ERR_VERSION		= 0x07,
	SX_ERR_READ_ONLY	= 0x08,
	SX_ERR_SD			= 0x09,
};

enum SxEvent {
	SX_EVENT_KIT_CHANGED		= 0x01,
	SX_EVENT_PATTERN_CHANGED	= 0x02,
};

//---------------------------------------------------------------------------
// 7-in-8 packing (same scheme as the front panel parser)

// number of wire bytes needed for n raw bytes
uint16_t sx_packedLen(uint16_t rawLen);
// packs n raw bytes into out, returns the number of bytes written
uint16_t sx_pack(const uint8_t *raw, uint16_t n, uint8_t *out);
// unpacks n wire bytes into out (at most outMax bytes).
// returns the raw length, or -1 if a byte has bit 7 set, an MSB byte has bits
// set for missing group members, a lone MSB byte is left over or out is too small.
int32_t sx_unpack(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t outMax);

// XOR of n bytes masked to 7 bits
uint8_t sx_checksum(const uint8_t *data, uint16_t n);

//---------------------------------------------------------------------------
// Frame writer

typedef struct {
	uint8_t *buf;
	uint16_t cap;
	uint16_t len;
	uint8_t overflow;
} SxWriter;

void sxw_begin(SxWriter *w, uint8_t *buf, uint16_t cap, uint8_t devByte, uint8_t cmd);
void sxw_u7(SxWriter *w, uint8_t v);
void sxw_u14(SxWriter *w, uint16_t v);
void sxw_packed(SxWriter *w, const uint8_t *raw, uint16_t n);
// appends checksum and F7. returns the frame length, 0 if it did not fit.
uint16_t sxw_end(SxWriter *w);

//---------------------------------------------------------------------------
// Frame parser

typedef enum {
	SX_PARSE_OK = 0,			// one of our frames, checksum good
	SX_PARSE_IGNORE,			// not for us (other manufacturer/model or malformed)
	SX_PARSE_UNIVERSAL_ID,		// F0 7E <dev> 06 01 F7
	SX_PARSE_BAD_CHECKSUM,		// our header, wrong checksum (dev/ver/cmd valid)
} SxParseResult;

typedef struct {
	uint8_t devByte;
	uint8_t version;
	uint8_t cmd;
	const uint8_t *payload;
	uint16_t payloadLen;
} SxFrame;

// frame must start with F0 and end with F7
SxParseResult sx_parseFrame(const uint8_t *frame, uint16_t len, SxFrame *out);

// 1 if devByte addresses the device with the given 1 based id
uint8_t sx_isForDevice(uint8_t devByte, uint8_t deviceId);

//---------------------------------------------------------------------------
// Payload reader

typedef struct {
	const uint8_t *p;
	uint16_t len;
	uint16_t pos;
	uint8_t err;		// set when reading past the end
} SxReader;

void sxr_init(SxReader *r, const uint8_t *p, uint16_t len);
uint8_t sxr_u7(SxReader *r);
uint16_t sxr_u14(SxReader *r);
uint16_t sxr_remaining(const SxReader *r);
const uint8_t *sxr_ptr(const SxReader *r);

//---------------------------------------------------------------------------
// Byte stream assembler (one per port)

typedef enum {
	SXA_NONE = 0,		// byte consumed, nothing complete yet
	SXA_FRAME,			// buf[0..len-1] holds a complete F0 ... F7 frame
	SXA_OVERFLOW,		// a frame longer than SX_MAX_FRAME ended; buf holds its first SX_HEADER_LEN bytes
} SxAsmResult;

typedef struct {
	uint8_t buf[SX_MAX_FRAME];
	uint16_t len;
	uint8_t state;
} SxAssembler;

void sxa_reset(SxAssembler *a);
SxAsmResult sxa_feed(SxAssembler *a, uint8_t byte);

#endif /* SYSEX_CODEC_H_ */
