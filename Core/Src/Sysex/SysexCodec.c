/*
 * SysexCodec.c
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */
#include "Sysex/SysexCodec.h"

#define SX_START	0xF0
#define SX_END		0xF7

//---------------------------------------------------------------------------
uint16_t sx_packedLen(uint16_t rawLen)
{
	return (uint16_t)(rawLen + (rawLen + 6) / 7);
}
//---------------------------------------------------------------------------
uint16_t sx_pack(const uint8_t *raw, uint16_t n, uint8_t *out)
{
	uint16_t o = 0;
	for(uint16_t i = 0; i < n; i += 7)
	{
		uint8_t msbs = 0;
		for(uint8_t k = 0; k < 7 && (i + k) < n; k++)
		{
			const uint8_t b = raw[i + k];
			out[o++] = b & 0x7F;
			if(b & 0x80) msbs |= (uint8_t)(1 << k);
		}
		out[o++] = msbs;
	}
	return o;
}
//---------------------------------------------------------------------------
int32_t sx_unpack(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t outMax)
{
	uint16_t o = 0;
	uint16_t i = 0;
	while(i < n)
	{
		// a group is up to 7 data bytes followed by the MSB byte
		uint16_t groupLen = (uint16_t)(n - i - 1);
		if(groupLen > 7) groupLen = 7;
		if(groupLen == 0) return -1; // lone MSB byte
		const uint8_t msbs = in[i + groupLen];
		if(msbs & 0x80) return -1;
		if(msbs >> groupLen) return -1;
		if(o + groupLen > outMax) return -1;
		for(uint16_t k = 0; k < groupLen; k++)
		{
			const uint8_t b = in[i + k];
			if(b & 0x80) return -1;
			out[o++] = (uint8_t)(b | (((msbs >> k) & 1) << 7));
		}
		i = (uint16_t)(i + groupLen + 1);
	}
	return o;
}
//---------------------------------------------------------------------------
uint8_t sx_checksum(const uint8_t *data, uint16_t n)
{
	uint8_t x = 0;
	for(uint16_t i = 0; i < n; i++) x ^= data[i];
	return x & 0x7F;
}
//---------------------------------------------------------------------------
static void sxw_put(SxWriter *w, uint8_t b)
{
	if(w->len < w->cap) w->buf[w->len++] = b;
	else w->overflow = 1;
}
//---------------------------------------------------------------------------
void sxw_begin(SxWriter *w, uint8_t *buf, uint16_t cap, uint8_t devByte, uint8_t cmd)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->overflow = 0;
	sxw_put(w, SX_START);
	sxw_put(w, SX_MANUFACTURER_ID);
	sxw_put(w, SX_MODEL_ID);
	sxw_put(w, devByte & 0x7F);
	sxw_put(w, SX_PROTOCOL_VERSION);
	sxw_put(w, cmd & 0x7F);
}
//---------------------------------------------------------------------------
void sxw_u7(SxWriter *w, uint8_t v)
{
	sxw_put(w, v & 0x7F);
}
//---------------------------------------------------------------------------
void sxw_u14(SxWriter *w, uint16_t v)
{
	sxw_put(w, v & 0x7F);
	sxw_put(w, (v >> 7) & 0x7F);
}
//---------------------------------------------------------------------------
void sxw_packed(SxWriter *w, const uint8_t *raw, uint16_t n)
{
	if(w->len + sx_packedLen(n) > w->cap)
	{
		w->overflow = 1;
		return;
	}
	w->len = (uint16_t)(w->len + sx_pack(raw, n, &w->buf[w->len]));
}
//---------------------------------------------------------------------------
uint16_t sxw_end(SxWriter *w)
{
	if(w->len < SX_HEADER_LEN) return 0;
	const uint8_t cs = sx_checksum(&w->buf[3], (uint16_t)(w->len - 3));
	sxw_put(w, cs);
	sxw_put(w, SX_END);
	if(w->overflow) return 0;
	return w->len;
}
//---------------------------------------------------------------------------
SxParseResult sx_parseFrame(const uint8_t *frame, uint16_t len, SxFrame *out)
{
	if(len < 2 || frame[0] != SX_START || frame[len - 1] != SX_END) return SX_PARSE_IGNORE;

	if(len == 6 && frame[1] == 0x7E && frame[3] == 0x06 && frame[4] == 0x01)
	{
		out->devByte = frame[2];
		out->version = 0;
		out->cmd = 0;
		out->payload = 0;
		out->payloadLen = 0;
		return SX_PARSE_UNIVERSAL_ID;
	}

	if(len < SX_MIN_FRAME) return SX_PARSE_IGNORE;
	if(frame[1] != SX_MANUFACTURER_ID || frame[2] != SX_MODEL_ID) return SX_PARSE_IGNORE;
	for(uint16_t i = 1; i < len - 1; i++)
	{
		if(frame[i] & 0x80) return SX_PARSE_IGNORE;
	}

	out->devByte = frame[3];
	out->version = frame[4];
	out->cmd = frame[5];
	out->payload = &frame[SX_HEADER_LEN];
	out->payloadLen = (uint16_t)(len - SX_MIN_FRAME);

	const uint8_t cs = sx_checksum(&frame[3], (uint16_t)(len - 5));
	if(cs != frame[len - 2]) return SX_PARSE_BAD_CHECKSUM;
	return SX_PARSE_OK;
}
//---------------------------------------------------------------------------
uint8_t sx_isForDevice(uint8_t devByte, uint8_t deviceId)
{
	return devByte == SX_DEVICE_BROADCAST || devByte == (uint8_t)(deviceId - 1);
}
//---------------------------------------------------------------------------
void sxr_init(SxReader *r, const uint8_t *p, uint16_t len)
{
	r->p = p;
	r->len = len;
	r->pos = 0;
	r->err = 0;
}
//---------------------------------------------------------------------------
uint8_t sxr_u7(SxReader *r)
{
	if(r->pos >= r->len)
	{
		r->err = 1;
		return 0;
	}
	return r->p[r->pos++];
}
//---------------------------------------------------------------------------
uint16_t sxr_u14(SxReader *r)
{
	const uint16_t lo = sxr_u7(r);
	const uint16_t hi = sxr_u7(r);
	return (uint16_t)(lo | (hi << 7));
}
//---------------------------------------------------------------------------
uint16_t sxr_remaining(const SxReader *r)
{
	return (uint16_t)(r->len - r->pos);
}
//---------------------------------------------------------------------------
const uint8_t *sxr_ptr(const SxReader *r)
{
	return &r->p[r->pos];
}
//---------------------------------------------------------------------------
enum {
	SXA_IDLE = 0,
	SXA_IN_FRAME,
	SXA_SKIPPING,
};
//---------------------------------------------------------------------------
void sxa_reset(SxAssembler *a)
{
	a->len = 0;
	a->state = SXA_IDLE;
}
//---------------------------------------------------------------------------
SxAsmResult sxa_feed(SxAssembler *a, uint8_t byte)
{
	if(byte >= 0xF8) return SXA_NONE; // real-time bytes may appear anywhere

	if(byte == SX_START)
	{
		a->buf[0] = byte;
		a->len = 1;
		a->state = SXA_IN_FRAME;
		return SXA_NONE;
	}

	if(byte == SX_END)
	{
		const uint8_t state = a->state;
		a->state = SXA_IDLE;
		if(state == SXA_IN_FRAME)
		{
			if(a->len >= SX_MAX_FRAME) return SXA_OVERFLOW;
			a->buf[a->len++] = byte;
			return SXA_FRAME;
		}
		if(state == SXA_SKIPPING) return SXA_OVERFLOW;
		return SXA_NONE;
	}

	if(byte & 0x80)
	{
		// any other status byte aborts the frame
		a->state = SXA_IDLE;
		return SXA_NONE;
	}

	if(a->state == SXA_IN_FRAME)
	{
		// keep one byte free for F7
		if(a->len < SX_MAX_FRAME - 1) a->buf[a->len++] = byte;
		else a->state = SXA_SKIPPING;
	}
	return SXA_NONE;
}
//---------------------------------------------------------------------------
void sx_kitApply(uint8_t *dst, uint16_t dstLen, const uint8_t *src, uint16_t n,
		const uint16_t *modIdx, uint8_t numModIdx, uint8_t numModTargets)
{
	const uint16_t count = n < dstLen ? n : dstLen;
	for(uint16_t i = 0; i < count; i++)
		dst[i] = src[i];
	for(uint8_t i = 0; i < numModIdx; i++)
	{
		if(modIdx[i] < dstLen && dst[modIdx[i]] >= numModTargets)
			dst[modIdx[i]] = 0;
	}
}
