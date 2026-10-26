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

enum SxCapability {
	SX_CAP_KITS			= 1 << 0,
	SX_CAP_PATTERNS		= 1 << 1,
	SX_CAP_GLOBALS		= 1 << 2,
	SX_CAP_PARAMS		= 1 << 3,
	SX_CAP_ECHO			= 1 << 4,
	SX_CAP_SAVE			= 1 << 5,
	SX_CAP_CPU_METER	= 1 << 6,
	SX_CAP_DRUM_ENGINE	= 1 << 7,
	SX_CAP_FX_FREEZE	= 1 << 8,
	SX_CAP_SEQ_CONDITIONS	= 1 << 9,
	SX_CAP_BEAT_REPEAT	= 1 << 10,
};

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
	SX_CMD_CONDITION_REQ	= 0x23,
	SX_CMD_CONDITION_DUMP	= 0x24,
	SX_CMD_CONDITION_SET	= 0x25,
	SX_CMD_GLOBALS_REQ		= 0x30,
	SX_CMD_GLOBALS_DUMP		= 0x31,
	SX_CMD_PARAM_GET		= 0x40,
	SX_CMD_PARAM_SET		= 0x41,
	SX_CMD_PARAM_VALUE		= 0x42,
	SX_CMD_PARAM_NOTIFY		= 0x43,
	SX_CMD_ECHO_SET			= 0x44,
	SX_CMD_EVENT			= 0x45,
	SX_CMD_FX_FREEZE		= 0x46,
	SX_CMD_SEQ_BEAT_REPEAT	= 0x47,
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

//---------------------------------------------------------------------------
// STATUS_REPLY payload

// STATUS_REQUEST flags (optional u7 payload)
#define SX_STATUS_RESET_PEAK	0x01	// clear CPU peak and underruns after replying

typedef struct {
	uint8_t kit;
	uint8_t pattern;
	uint8_t running;
	uint8_t echo;
	// CPU meter, see docs/performance.md. Loads in 1/10 % of the audio block time.
	uint8_t hasCpu;		// reader: 1 if the reply contained the CPU fields
	uint16_t cpuNow;
	uint16_t cpuPeak;
	uint16_t cpuVoices;
	uint16_t cpuMixer;
	uint16_t cpuFx;
	uint16_t underruns;
} SxStatus;

// writes all STATUS_REPLY fields (the CPU fields are always written, hasCpu is ignored)
void sx_writeStatus(SxWriter *w, const SxStatus *s);
// reads a STATUS_REPLY payload. Replies from firmware without the CPU meter
// end after the first four fields; then hasCpu is 0 and the CPU fields are 0.
// Extra trailing bytes are ignored. Returns 0, or -1 if the payload is too short.
int8_t sx_readStatus(SxReader *r, SxStatus *s);

//---------------------------------------------------------------------------
// Kit data

// Applies received kit parameter bytes the way the SD loader
// (preset_readDrumsetData) treats a .snd file. The first min(n, dstLen)
// bytes are copied unchanged, then each modulation-target index listed in
// modIdx that is >= numModTargets is set to 0. Other values are NOT clamped,
// so a kit sounds the same whether it came from SD or over SysEx.
// dst bytes at positions >= n keep their old value.
void sx_kitApply(uint8_t *dst, uint16_t dstLen, const uint8_t *src, uint16_t n,
		const uint16_t *modIdx, uint8_t numModIdx, uint8_t numModTargets);

#endif /* SYSEX_CODEC_H_ */
