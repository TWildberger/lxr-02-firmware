/*
 * Host-side unit tests for the SysEx codec (Core/Src/Sysex/SysexCodec.c).
 * Build and run on the development machine: make -C tests/host
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Sysex/SysexCodec.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
		checks++; \
		if(!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
	} while(0)

#define CHECK_BYTES(got, gotLen, ...) do { \
		const uint8_t exp_[] = { __VA_ARGS__ }; \
		checks++; \
		if((gotLen) != sizeof(exp_) || memcmp((got), exp_, sizeof(exp_)) != 0) { \
			failures++; \
			printf("FAIL %s:%d: bytes differ\n  got:", __FILE__, __LINE__); \
			for(int i_ = 0; i_ < (int)(gotLen); i_++) printf(" %02X", (got)[i_]); \
			printf("\n  exp:"); \
			for(int i_ = 0; i_ < (int)sizeof(exp_); i_++) printf(" %02X", exp_[i_]); \
			printf("\n"); \
		} \
	} while(0)

//---------------------------------------------------------------------------
static void test_pack_example(void)
{
	const uint8_t raw[] = { 0x81, 0x02, 0xFF };
	uint8_t out[8];
	const uint16_t n = sx_pack(raw, sizeof(raw), out);
	CHECK_BYTES(out, n, 0x01, 0x02, 0x7F, 0x05);
	CHECK(sx_packedLen(3) == 4);

	uint8_t back[8];
	CHECK(sx_unpack(out, n, back, sizeof(back)) == 3);
	CHECK(memcmp(back, raw, 3) == 0);
}
//---------------------------------------------------------------------------
static void test_packed_len(void)
{
	CHECK(sx_packedLen(0) == 0);
	CHECK(sx_packedLen(1) == 2);
	CHECK(sx_packedLen(7) == 8);
	CHECK(sx_packedLen(8) == 10);
	CHECK(sx_packedLen(196) == 224);
	CHECK(sx_packedLen(255) == 292);	// kit dump: name[8] + 247
	CHECK(sx_packedLen(129) == 148);
}
//---------------------------------------------------------------------------
static void test_pack_roundtrip(void)
{
	uint8_t raw[400], packed[480], back[400];
	srand(1234);
	for(uint16_t len = 0; len <= 300; len++)
	{
		for(uint16_t i = 0; i < len; i++) raw[i] = (uint8_t)rand();
		if(len > 0) raw[0] = 0xFF;
		const uint16_t n = sx_pack(raw, len, packed);
		CHECK(n == sx_packedLen(len));
		int high = 0;
		for(uint16_t i = 0; i < n; i++) high |= packed[i] & 0x80;
		CHECK(high == 0);
		memset(back, 0xAA, sizeof(back));
		CHECK(sx_unpack(packed, n, back, sizeof(back)) == len);
		CHECK(memcmp(back, raw, len) == 0);
	}
}
//---------------------------------------------------------------------------
static void test_unpack_errors(void)
{
	uint8_t out[16];
	// lone MSB byte after a full group
	const uint8_t lone[] = { 1, 2, 3, 4, 5, 6, 7, 0, 0 };
	CHECK(sx_unpack(lone, sizeof(lone), out, sizeof(out)) == -1);
	// single byte = lone MSB byte
	const uint8_t one[] = { 0 };
	CHECK(sx_unpack(one, 1, out, sizeof(out)) == -1);
	// MSB bits set for bytes that don't exist
	const uint8_t badMsb[] = { 0x10, 0x02 };
	CHECK(sx_unpack(badMsb, sizeof(badMsb), out, sizeof(out)) == -1);
	// data byte with bit 7 set
	const uint8_t high[] = { 0x80, 0x00 };
	CHECK(sx_unpack(high, sizeof(high), out, sizeof(out)) == -1);
	// output too small
	const uint8_t ok[] = { 1, 2, 3, 0 };
	CHECK(sx_unpack(ok, sizeof(ok), out, 2) == -1);
	CHECK(sx_unpack(ok, sizeof(ok), out, 3) == 3);
	// empty input
	CHECK(sx_unpack(ok, 0, out, sizeof(out)) == 0);
}
//---------------------------------------------------------------------------
static void test_checksum(void)
{
	const uint8_t d[] = { 0x7F, 0x01, 0x01 };
	CHECK(sx_checksum(d, sizeof(d)) == 0x7F);
	CHECK(sx_checksum(d, 0) == 0);
	const uint8_t e[] = { 0x7F, 0x01, 0x41, 0x03, 0x00, 0x64, 0x00 };
	CHECK(sx_checksum(e, sizeof(e)) == 0x58);
}
//---------------------------------------------------------------------------
static void test_writer_doc_examples(void)
{
	uint8_t buf[SX_MAX_FRAME];
	SxWriter w;

	// identity request (docs/sysex.md)
	sxw_begin(&w, buf, sizeof(buf), 0x7F, SX_CMD_IDENTITY_REQ);
	uint16_t n = sxw_end(&w);
	CHECK_BYTES(buf, n, 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x01, 0x7F, 0xF7);

	// request the edit buffer
	sxw_begin(&w, buf, sizeof(buf), 0x7F, SX_CMD_KIT_REQ);
	sxw_u7(&w, 0x7F);
	n = sxw_end(&w);
	CHECK_BYTES(buf, n, 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x10, 0x7F, 0x11, 0xF7);

	// set parameter 3 to 100
	sxw_begin(&w, buf, sizeof(buf), 0x7F, SX_CMD_PARAM_SET);
	sxw_u14(&w, 3);
	sxw_u14(&w, 100);
	n = sxw_end(&w);
	CHECK_BYTES(buf, n, 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x41, 0x03, 0x00, 0x64, 0x00, 0x58, 0xF7);
}
//---------------------------------------------------------------------------
static void test_writer_fields(void)
{
	uint8_t buf[SX_MAX_FRAME];
	SxWriter w;
	sxw_begin(&w, buf, sizeof(buf), 0x03, SX_CMD_ACK);
	sxw_u7(&w, 0xFF);			// masked to 7 bits
	sxw_u14(&w, 3657);			// 0x0E49 -> 49 1C
	const uint8_t raw[] = { 0x80 };
	sxw_packed(&w, raw, 1);
	const uint16_t n = sxw_end(&w);
	CHECK(n == 6 + 1 + 2 + 2 + 2);
	CHECK(buf[3] == 0x03);
	CHECK(buf[6] == 0x7F);
	CHECK(buf[7] == 0x49 && buf[8] == 0x1C);
	CHECK(buf[9] == 0x00 && buf[10] == 0x01);
	CHECK(buf[n - 2] == sx_checksum(&buf[3], (uint16_t)(n - 5)));
	CHECK(buf[n - 1] == 0xF7);

	SxFrame f;
	CHECK(sx_parseFrame(buf, n, &f) == SX_PARSE_OK);
	CHECK(f.cmd == SX_CMD_ACK && f.devByte == 3 && f.version == SX_PROTOCOL_VERSION);
	SxReader r;
	sxr_init(&r, f.payload, f.payloadLen);
	CHECK(sxr_u7(&r) == 0x7F);
	CHECK(sxr_u14(&r) == 3657);
	uint8_t back[4];
	CHECK(sx_unpack(sxr_ptr(&r), sxr_remaining(&r), back, sizeof(back)) == 1);
	CHECK(back[0] == 0x80);
	CHECK(!r.err);
}
//---------------------------------------------------------------------------
static void test_writer_overflow(void)
{
	uint8_t buf[SX_MAX_FRAME];
	uint8_t raw[400] = { 0 };
	SxWriter w;

	// largest pattern chunk fits: 6 + 6 + 224 + 2 = 238
	sxw_begin(&w, buf, sizeof(buf), 0, SX_CMD_PATTERN_CHUNK);
	for(int i = 0; i < 4; i++) sxw_u7(&w, 0);
	sxw_u14(&w, 3657);
	sxw_packed(&w, raw, 196);
	CHECK(sxw_end(&w) == 238);

	// kit dump fits: 6 + 4 + 292 + 2 = 304
	sxw_begin(&w, buf, sizeof(buf), 0, SX_CMD_KIT_DUMP);
	sxw_u7(&w, 0x7F);
	sxw_u7(&w, 0);
	sxw_u14(&w, 247);
	sxw_packed(&w, raw, 255);
	CHECK(sxw_end(&w) == 304);

	// too much data -> 0
	sxw_begin(&w, buf, sizeof(buf), 0, SX_CMD_KIT_DUMP);
	sxw_packed(&w, raw, 300);
	CHECK(sxw_end(&w) == 0);

	sxw_begin(&w, buf, 10, 0, SX_CMD_PARAM_VALUE);
	sxw_u14(&w, 1);
	sxw_u14(&w, 2);
	CHECK(sxw_end(&w) == 0);
}
//---------------------------------------------------------------------------
static void test_parse(void)
{
	SxFrame f;
	const uint8_t ok[] = { 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x01, 0x7F, 0xF7 };
	CHECK(sx_parseFrame(ok, sizeof(ok), &f) == SX_PARSE_OK);
	CHECK(f.payloadLen == 0 && f.cmd == 0x01);

	const uint8_t badCs[] = { 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x01, 0x7E, 0xF7 };
	CHECK(sx_parseFrame(badCs, sizeof(badCs), &f) == SX_PARSE_BAD_CHECKSUM);
	CHECK(f.cmd == 0x01 && f.devByte == 0x7F);

	const uint8_t otherMfr[] = { 0xF0, 0x41, 0x4C, 0x7F, 0x01, 0x01, 0x7F, 0xF7 };
	CHECK(sx_parseFrame(otherMfr, sizeof(otherMfr), &f) == SX_PARSE_IGNORE);
	const uint8_t otherModel[] = { 0xF0, 0x7D, 0x00, 0x7F, 0x01, 0x01, 0x7F, 0xF7 };
	CHECK(sx_parseFrame(otherModel, sizeof(otherModel), &f) == SX_PARSE_IGNORE);
	const uint8_t tooShort[] = { 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0xF7 };
	CHECK(sx_parseFrame(tooShort, sizeof(tooShort), &f) == SX_PARSE_IGNORE);
	const uint8_t noEnd[] = { 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x01, 0x7F, 0x00 };
	CHECK(sx_parseFrame(noEnd, sizeof(noEnd), &f) == SX_PARSE_IGNORE);
	const uint8_t highByte[] = { 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x01, 0x80, 0x7F, 0xF7 };
	CHECK(sx_parseFrame(highByte, sizeof(highByte), &f) == SX_PARSE_IGNORE);

	const uint8_t uid[] = { 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7 };
	CHECK(sx_parseFrame(uid, sizeof(uid), &f) == SX_PARSE_UNIVERSAL_ID);
	CHECK(f.devByte == 0x7F);
	const uint8_t uidOther[] = { 0xF0, 0x7E, 0x7F, 0x06, 0x02, 0xF7 };
	CHECK(sx_parseFrame(uidOther, sizeof(uidOther), &f) == SX_PARSE_IGNORE);
}
//---------------------------------------------------------------------------
static void test_device_id(void)
{
	CHECK(sx_isForDevice(0x7F, 1));
	CHECK(sx_isForDevice(0x7F, 16));
	CHECK(sx_isForDevice(0x00, 1));
	CHECK(!sx_isForDevice(0x01, 1));
	CHECK(sx_isForDevice(0x0F, 16));
	CHECK(!sx_isForDevice(0x0F, 15));
}
//---------------------------------------------------------------------------
static void test_reader(void)
{
	const uint8_t p[] = { 0x05, 0x49, 0x1C };
	SxReader r;
	sxr_init(&r, p, sizeof(p));
	CHECK(sxr_remaining(&r) == 3);
	CHECK(sxr_u7(&r) == 5);
	CHECK(sxr_u14(&r) == 3657);
	CHECK(!r.err);
	CHECK(sxr_remaining(&r) == 0);
	CHECK(sxr_u7(&r) == 0);
	CHECK(r.err);

	sxr_init(&r, p, 1);
	sxr_u14(&r);
	CHECK(r.err);
}
//---------------------------------------------------------------------------
static SxAsmResult feed_all(SxAssembler *a, const uint8_t *b, int n, int *frames)
{
	SxAsmResult last = SXA_NONE;
	for(int i = 0; i < n; i++)
	{
		const SxAsmResult res = sxa_feed(a, b[i]);
		if(res != SXA_NONE) { last = res; if(frames) (*frames)++; }
	}
	return last;
}
//---------------------------------------------------------------------------
static void test_assembler(void)
{
	static SxAssembler a;
	sxa_reset(&a);

	// plain frame with realtime bytes inside
	const uint8_t in[] = { 0xF8, 0xF0, 0x7D, 0xF8, 0x4C, 0x7F, 0x01, 0xFE, 0x01, 0x7F, 0xF7 };
	int frames = 0;
	CHECK(feed_all(&a, in, sizeof(in), &frames) == SXA_FRAME);
	CHECK(frames == 1);
	CHECK_BYTES(a.buf, a.len, 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x01, 0x7F, 0xF7);

	// stray data and F7 outside a frame
	const uint8_t stray[] = { 0x12, 0x34, 0xF7, 0x90, 0x40, 0x7F };
	frames = 0;
	feed_all(&a, stray, sizeof(stray), &frames);
	CHECK(frames == 0);

	// status byte aborts a frame
	const uint8_t aborted[] = { 0xF0, 0x7D, 0x4C, 0x90, 0x40, 0x7F, 0xF7 };
	frames = 0;
	feed_all(&a, aborted, sizeof(aborted), &frames);
	CHECK(frames == 0);

	// a new F0 restarts the frame
	const uint8_t restart[] = { 0xF0, 0x01, 0x02, 0xF0, 0x7D, 0x4C, 0x7F, 0x01, 0x01, 0x7F, 0xF7 };
	frames = 0;
	CHECK(feed_all(&a, restart, sizeof(restart), &frames) == SXA_FRAME);
	CHECK(frames == 1 && a.len == 8);

	// exactly SX_MAX_FRAME bytes is accepted
	sxa_reset(&a);
	CHECK(sxa_feed(&a, 0xF0) == SXA_NONE);
	const uint8_t hdr[] = { 0x7D, 0x4C, 0x05, 0x01, 0x21 };
	for(unsigned i = 0; i < sizeof(hdr); i++) sxa_feed(&a, hdr[i]);
	for(int i = 0; i < SX_MAX_FRAME - 7; i++) CHECK(sxa_feed(&a, 0x11) == SXA_NONE);
	CHECK(sxa_feed(&a, 0xF7) == SXA_FRAME);
	CHECK(a.len == SX_MAX_FRAME);

	// one byte more overflows, header kept for the NAK
	sxa_reset(&a);
	sxa_feed(&a, 0xF0);
	for(unsigned i = 0; i < sizeof(hdr); i++) sxa_feed(&a, hdr[i]);
	for(int i = 0; i < SX_MAX_FRAME - 6 + 50; i++) sxa_feed(&a, 0x11);
	CHECK(sxa_feed(&a, 0xF7) == SXA_OVERFLOW);
	CHECK(a.buf[0] == 0xF0 && a.buf[1] == 0x7D && a.buf[2] == 0x4C && a.buf[3] == 0x05 && a.buf[5] == 0x21);

	// and recovers for the next frame
	frames = 0;
	CHECK(feed_all(&a, in, sizeof(in), &frames) == SXA_FRAME);
	CHECK(frames == 1 && a.len == 8);
}
//---------------------------------------------------------------------------
// writer -> byte stream -> assembler -> parser -> reader, like a host upload
static void test_end_to_end(void)
{
	static SxAssembler a;
	uint8_t frame[SX_MAX_FRAME];
	uint8_t raw[196];
	for(int i = 0; i < 196; i++) raw[i] = (uint8_t)(i * 37 + 5);

	SxWriter w;
	sxw_begin(&w, frame, sizeof(frame), 0x7F, SX_CMD_PATTERN_CHUNK);
	sxw_u7(&w, 12);
	sxw_u7(&w, SX_FLAG_SAVE);
	sxw_u7(&w, 0);
	sxw_u7(&w, 19);
	sxw_u14(&w, 3657);
	sxw_packed(&w, raw, sizeof(raw));
	const uint16_t n = sxw_end(&w);
	CHECK(n == 238);

	sxa_reset(&a);
	int frames = 0;
	CHECK(feed_all(&a, frame, n, &frames) == SXA_FRAME);
	CHECK(frames == 1 && a.len == n);

	SxFrame f;
	CHECK(sx_parseFrame(a.buf, a.len, &f) == SX_PARSE_OK);
	CHECK(f.cmd == SX_CMD_PATTERN_CHUNK);
	SxReader r;
	sxr_init(&r, f.payload, f.payloadLen);
	CHECK(sxr_u7(&r) == 12);
	CHECK(sxr_u7(&r) == SX_FLAG_SAVE);
	CHECK(sxr_u7(&r) == 0);
	CHECK(sxr_u7(&r) == 19);
	CHECK(sxr_u14(&r) == 3657);
	uint8_t back[196];
	CHECK(sx_unpack(sxr_ptr(&r), sxr_remaining(&r), back, sizeof(back)) == 196);
	CHECK(memcmp(back, raw, sizeof(raw)) == 0);

	// corrupt one payload byte -> checksum error
	a.buf[20] ^= 0x01;
	CHECK(sx_parseFrame(a.buf, a.len, &f) == SX_PARSE_BAD_CHECKSUM);
}
//---------------------------------------------------------------------------
int main(void)
{
	test_pack_example();
	test_packed_len();
	test_pack_roundtrip();
	test_unpack_errors();
	test_checksum();
	test_writer_doc_examples();
	test_writer_fields();
	test_writer_overflow();
	test_parse();
	test_device_id();
	test_reader();
	test_assembler();
	test_end_to_end();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
