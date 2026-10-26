/*
 * SysexHandler.c
 *
 * LXR-02 SysEx protocol, see docs/sysex.md.
 *
 * Bytes are collected by the MIDI ISRs into one ring per port. Everything else
 * (frame assembly, request handling, replies, parameter echo) runs from
 * sysex_tick() in the main loop, the same context as the menu and the preset
 * manager whose functions it calls.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */
#include "Sysex/SysexHandler.h"
#include "Sysex/SysexCodec.h"

#include <string.h>

#include "main.h"
#include "menu/menu.h"
#include "menu/CcNr2Text.h"
#include "Preset/PresetManager.h"
#include "Preset/KitEngineFormat.h"
#include "Soundengine/Sequencer/sequencer.h"
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/MIDI/Uart.h"
#include "Soundengine/USB/usb_manager.h"
#include "Soundengine/CpuMeter.h"
#include "Soundengine/FxBus.h"
#include "frontPanelParser_ui.h"

#ifndef LXR_FW_MAJOR
#define LXR_FW_MAJOR 1
#endif
#ifndef LXR_FW_MINOR
#define LXR_FW_MINOR 7
#endif
#ifndef LXR_FW_PATCH
#define LXR_FW_PATCH 0
#endif
#ifndef LXR_FW_VERSION_STRING
#define LXR_FW_VERSION_STRING "1.7-sx"
#endif

#define PORT_DIN			0x01
#define PORT_USB			0x02

#define RX_RING_SIZE		512
#define RX_RING_MASK		(RX_RING_SIZE - 1)
#define RX_BYTES_PER_TICK	1024

#define NAME_LEN			8
#define KIT_DATA_LEN		(NAME_LEN + END_OF_SOUND_PARAMETERS + KIT_ENGINE_TAG_SIZE)
#define NUM_GLOBALS			(NUM_PARAMS - PAR_BEGINNING_OF_GLOBALS)
#define MAX_PARAMS_PER_MSG	64

#define PATTERN_BODY_LEN	(NUM_TRACKS * (NUM_STEPS * sizeof(Step) + NUM_STEPS * sizeof(StepTiming)) \
							+ NUM_TRACKS * NUM_BARS * sizeof(uint16_t) + 1 + NUM_TRACKS + 1)
#define PATTERN_DATA_LEN	(NAME_LEN + PATTERN_BODY_LEN)
#define PATTERN_CHUNK_LEN	196
#define PATTERN_NUM_CHUNKS	((PATTERN_DATA_LEN + PATTERN_CHUNK_LEN - 1) / PATTERN_CHUNK_LEN)

#define ECHO_INTERVAL_MS	20
#define ECHO_MAX_PAIRS		24

_Static_assert(sizeof(Step) == 7, "pattern layout changed");
_Static_assert(sizeof(StepTiming) == 1, "pattern layout changed");
_Static_assert(PATTERN_DATA_LEN == 3657, "pattern layout changed, update docs/sysex.md");
_Static_assert(PATTERN_NUM_CHUNKS < 128, "too many pattern chunks");
_Static_assert(NUM_PARAMS < 16384, "parameter ids must fit u14");

//---------------------------------------------------------------------------
typedef struct {
	uint8_t data[RX_RING_SIZE];
	volatile uint16_t wr;
	volatile uint16_t rd;
} RxRing;

static RxRing sx_rxDin;
static RxRing sx_rxUsb;
static uint8_t sx_dinFrameOpen;

static SxAssembler sx_asmDin;
static SxAssembler sx_asmUsb;

// outgoing frame, sent to every port in sx_txPending
static uint8_t sx_txBuf[SX_MAX_FRAME];
static uint16_t sx_txLen;
static uint16_t sx_txDinPos;
static uint8_t sx_txDinStarted;
static uint8_t sx_txPending;
static SxWriter sx_w;

// port of the most recent valid request, 0 = none yet
static uint8_t sx_hostPorts;
static uint8_t sx_replyPorts;

// pattern dump streaming / upload staging
static uint8_t sx_patternBuf[PATTERN_DATA_LEN];
static uint8_t sx_streamActive;
static uint8_t sx_streamSlot;
static uint8_t sx_streamIdx;
static uint8_t sx_streamPorts;

static uint8_t sx_rxPatActive;
static uint8_t sx_rxPatSlot;
static uint8_t sx_rxPatFlags;
static uint8_t sx_rxPatNextIdx;
static uint8_t sx_rxPatCount;
static uint16_t sx_rxPatTotal;
static uint16_t sx_rxPatReceived;

static uint8_t sx_scratch[SX_MAX_FRAME];

// parameter echo
static uint8_t sx_shadow[NUM_PARAMS];
static volatile uint8_t sx_extChange[NUM_PARAMS];
static uint8_t sx_echoWasOn;
static uint32_t sx_lastScan;
static uint8_t sx_lastKit;
static uint8_t sx_lastPattern;

//---------------------------------------------------------------------------
static inline uint32_t sx_irqSave(void)
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	return primask;
}
static inline void sx_irqRestore(uint32_t primask)
{
	__set_PRIMASK(primask);
}
//---------------------------------------------------------------------------
static inline uint8_t sx_enabled(void)
{
	return activeParameters[PAR_SYSEX_ENABLE] != 0;
}
static inline uint8_t sx_deviceId(void)
{
	const uint8_t id = activeParameters[PAR_SYSEX_DEVICE_ID];
	return (id >= 1 && id <= 16) ? id : 1;
}
static inline uint8_t sx_ownDevByte(void)
{
	return (uint8_t)(sx_deviceId() - 1);
}
//---------------------------------------------------------------------------
static void sx_ringPush(RxRing *r, uint8_t b)
{
	const uint16_t next = (r->wr + 1) & RX_RING_MASK;
	if(next == r->rd) return; // full, byte lost (checksum/timeout tells the host)
	r->data[r->wr] = b;
	r->wr = next;
}
//---------------------------------------------------------------------------
static uint8_t sx_ringPop(RxRing *r, uint8_t *b)
{
	const uint16_t rd = r->rd;
	if(rd == r->wr) return 0;
	*b = r->data[rd];
	r->rd = (rd + 1) & RX_RING_MASK;
	return 1;
}
//---------------------------------------------------------------------------
void sysex_rxDinMessage(uint8_t status, uint8_t isSysexByte)
{
	if(!sx_enabled()) return;
	if(!isSysexByte && status >= 0xF8) return;

	// uart_processMidi() runs in the main loop and in the TIM2 ISR
	const uint32_t primask = sx_irqSave();
	if(isSysexByte)
	{
		if(sx_dinFrameOpen) sx_ringPush(&sx_rxDin, status);
	}
	else if(status == 0xF0)
	{
		sx_dinFrameOpen = 1;
		sx_ringPush(&sx_rxDin, status);
	}
	else if(sx_dinFrameOpen)
	{
		// F7 ends the frame, any other status byte aborts it
		sx_dinFrameOpen = 0;
		sx_ringPush(&sx_rxDin, status);
	}
	sx_irqRestore(primask);
}
//---------------------------------------------------------------------------
void sysex_rxBytesUsb(const uint8_t *bytes, uint8_t n)
{
	// keep forwarding SysEx with USB->MIDI routing, like the stock firmware
	if(midiParser_isUsb2MidiRouted())
	{
		for(uint8_t i = 0; i < n; i++) uart_sendMidiByte(bytes[i]);
	}
	if(!sx_enabled()) return;
	// only producer is the USB ISR
	for(uint8_t i = 0; i < n; i++) sx_ringPush(&sx_rxUsb, bytes[i]);
}
//---------------------------------------------------------------------------
void sysex_markExternalChange(uint16_t paramNr)
{
	if(paramNr < NUM_PARAMS) sx_extChange[paramNr] = 1;
}
//---------------------------------------------------------------------------
static void sx_syncShadow(void)
{
	memcpy(sx_shadow, activeParameters, NUM_PARAMS);
	memset((void *)sx_extChange, 0, NUM_PARAMS);
	sx_lastKit = menu_loadedPresetNr[CURRENT_KIT];
	sx_lastPattern = seq_activePattern;
}
//---------------------------------------------------------------------------
void sysex_init(void)
{
	// the RX rings may already be in use by the MIDI interrupts (zero initialised)
	sxa_reset(&sx_asmDin);
	sxa_reset(&sx_asmUsb);
	sx_txPending = 0;
	sx_txLen = 0;
	sx_hostPorts = 0;
	sx_streamActive = 0;
	sx_rxPatActive = 0;
	sx_echoWasOn = 0;
	sx_syncShadow();
}

//---------------------------------------------------------------------------
// TX
//---------------------------------------------------------------------------
static inline uint8_t sx_txBusy(void)
{
	return sx_txPending || sx_streamActive;
}
//---------------------------------------------------------------------------
static void sx_begin(uint8_t cmd)
{
	sxw_begin(&sx_w, sx_txBuf, sizeof(sx_txBuf), sx_ownDevByte(), cmd);
}
//---------------------------------------------------------------------------
static void sx_send(uint8_t ports)
{
	const uint16_t len = sxw_end(&sx_w);
	if(!len || !ports) return;
	sx_txLen = len;
	sx_txDinPos = 0;
	sx_txDinStarted = 0;
	sx_txPending = ports;
}
//---------------------------------------------------------------------------
static void sx_ack(uint8_t cmd, uint16_t arg)
{
	sx_begin(SX_CMD_ACK);
	sxw_u7(&sx_w, cmd);
	sxw_u14(&sx_w, arg);
	sx_send(sx_replyPorts);
}
//---------------------------------------------------------------------------
static void sx_nak(uint8_t cmd, uint8_t err, uint16_t arg)
{
	sx_begin(SX_CMD_NAK);
	sxw_u7(&sx_w, cmd);
	sxw_u7(&sx_w, err);
	sxw_u14(&sx_w, arg);
	sx_send(sx_replyPorts);
}
//---------------------------------------------------------------------------
static void sx_event(uint8_t type, uint16_t arg, uint8_t ports)
{
	sx_begin(SX_CMD_EVENT);
	sxw_u7(&sx_w, type);
	sxw_u14(&sx_w, arg);
	sx_send(ports);
}
//---------------------------------------------------------------------------
static void sx_txPump(void)
{
	if(sx_txPending & PORT_USB)
	{
		if(!usb_isConfigured())
			sx_txPending &= (uint8_t)~PORT_USB;
		else if(usb_sendSysex(sx_txBuf, sx_txLen))
			sx_txPending &= (uint8_t)~PORT_USB;
	}

	if(sx_txPending & PORT_DIN)
	{
		if(!sx_txDinStarted) sx_txDinStarted = uart_sysexTxBegin();
		if(sx_txDinStarted)
		{
			while(sx_txDinPos < sx_txLen && uart_sysexTxPut(sx_txBuf[sx_txDinPos]))
				sx_txDinPos++;
			if(sx_txDinPos >= sx_txLen)
			{
				uart_sysexTxEnd();
				sx_txDinStarted = 0;
				sx_txPending &= (uint8_t)~PORT_DIN;
			}
		}
	}
}

//---------------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------------
static uint8_t sx_isModTarget(uint16_t id)
{
	const uint8_t dt = parameter_dtypes[id] & 0xFF;
	return dt == DTYPE_TARGET_SELECTION_VELO || dt == DTYPE_TARGET_SELECTION_LFO || dt == DTYPE_AUTOM_TARGET;
}
//---------------------------------------------------------------------------
static uint8_t sx_clampValue(uint16_t id, uint16_t value)
{
	if(sx_isModTarget(id))
		return value < getNumModTargets() ? (uint8_t)value : 0;
	return menu_clampParamValue(id, value);
}
//---------------------------------------------------------------------------
// the indices preset_readDrumsetData validates when loading a .snd file
static const uint16_t sx_kitModIdx[] = {
	PAR_VEL_DEST_1, PAR_VEL_DEST_1 + 1, PAR_VEL_DEST_1 + 2,
	PAR_VEL_DEST_1 + 3, PAR_VEL_DEST_1 + 4, PAR_VEL_DEST_1 + 5,
	PAR_TARGET_LFO1, PAR_TARGET_LFO1 + 1, PAR_TARGET_LFO1 + 2,
	PAR_TARGET_LFO1 + 3, PAR_TARGET_LFO1 + 4, PAR_TARGET_LFO1 + 5,
};
_Static_assert(PAR_VEL_DEST_6 == PAR_VEL_DEST_1 + 5, "velocity targets must be contiguous");
_Static_assert(PAR_TARGET_LFO6 == PAR_TARGET_LFO1 + 5, "LFO targets must be contiguous");
//---------------------------------------------------------------------------
static void sx_copyName(char *dst, const uint8_t *src)
{
	for(uint8_t i = 0; i < NAME_LEN; i++)
	{
		const uint8_t c = src[i];
		dst[i] = (c < 0x20 || c > 0x7E) ? ' ' : (char)c;
	}
}
//---------------------------------------------------------------------------
static void sx_sendModTarget(uint8_t cc, uint8_t index, uint8_t modTarget)
{
	const uint8_t value = (uint8_t)pgm_read_word(&modTargets[modTarget].param);
	const uint8_t upper = (uint8_t)(((value & 0x80) >> 7) | ((index & 0x3f) << 1));
	frontPanel_sendData(cc, upper, value & 0x7f);
}
//---------------------------------------------------------------------------
// applies one (already clamped) value to the edit buffer/globals, like a front panel edit
static void sx_applyParam(uint16_t id, uint8_t value)
{
	activeParameters[id] = value;

	switch(parameter_dtypes[id] & 0xFF)
	{
	case DTYPE_TARGET_SELECTION_VELO:
		sx_sendModTarget(CC_VELO_TARGET, (uint8_t)(id - PAR_VEL_DEST_1), value);
		break;
	case DTYPE_TARGET_SELECTION_LFO:
		sx_sendModTarget(CC_LFO_TARGET, (uint8_t)(id - PAR_TARGET_LFO1), value);
		menu_editBufferReplaced();
		break;
	case DTYPE_VOICE_LFO:
		// the target is stored as its own parameter, re-send it
		sx_sendModTarget(CC_LFO_TARGET, (uint8_t)(id - PAR_VOICE_LFO1),
				activeParameters[PAR_TARGET_LFO1 + (id - PAR_VOICE_LFO1)]);
		break;
	default:
		if(id < END_OF_SOUND_PARAMETERS)
		{
			uint8_t out = value;
			if(id < PAR_AUDIO_OUT1 || id > PAR_AUDIO_OUT6)
				out = preset_getMorphValue(id, activeParameters[PAR_MORPH]);
			if(id < 128) frontPanel_sendData(MIDI_CC, (uint8_t)id, out);
			else frontPanel_sendData(CC_2, (uint8_t)(id - 128), out);
		}
		else
		{
			menu_parseGlobalParam(id, value);
		}
		break;
	}
}
//---------------------------------------------------------------------------
static uint8_t sx_isReadOnly(uint16_t id)
{
	if(id >= END_OF_SOUND_PARAMETERS && id < PAR_BEGINNING_OF_GLOBALS) return 1;
	return id == PAR_SYSEX_ENABLE || id == PAR_SYSEX_DEVICE_ID;
}
//---------------------------------------------------------------------------
// copies a pattern slot to/from the wire layout (docs/file-formats.md, pattern body)
static void sx_patternTransfer(uint8_t slot, uint8_t *body, uint8_t toDevice)
{
	uint16_t o = 0;
#define SX_SEG(ptr, n) do { \
		if(toDevice) memcpy((void *)(ptr), &body[o], (n)); \
		else memcpy(&body[o], (const void *)(ptr), (n)); \
		o += (n); } while(0)

	for(uint8_t trk = 0; trk < NUM_TRACKS; trk++)
	{
		SX_SEG(&seq_patternSet.seq_subStepPattern[slot][trk][0], sizeof(Step) * NUM_STEPS);
		SX_SEG(&seq_patternSet.seq_stepTiming[slot][trk][0], sizeof(StepTiming) * NUM_STEPS);
	}
	for(uint8_t trk = 0; trk < NUM_TRACKS; trk++)
	{
		for(uint8_t bar = 0; bar < NUM_BARS; bar++)
		{
			uint16_t *mainSteps = &seq_patternSet.seq_mainSteps[bar][slot][trk];
			if(toDevice) *mainSteps = (uint16_t)(body[o] | (body[o + 1] << 8));
			else
			{
				body[o] = (uint8_t)(*mainSteps & 0xFF);
				body[o + 1] = (uint8_t)(*mainSteps >> 8);
			}
			o += 2;
		}
	}
	SX_SEG(&seq_patternSet.shuffle[slot], 1);
	SX_SEG(&seq_patternSet.length[slot][0], NUM_TRACKS);
	SX_SEG(&seq_patternSet.kitNr[slot], 1);
#undef SX_SEG

	if(toDevice && seq_patternSet.kitNr[slot] >= NUM_KIT_SLOTS)
		seq_patternSet.kitNr[slot] = 0; // used as an index into the kit table
}

//---------------------------------------------------------------------------
// command handlers
//---------------------------------------------------------------------------
static void sx_cmdIdentity(void)
{
	static const char version[] = LXR_FW_VERSION_STRING;
	sx_begin(SX_CMD_IDENTITY_REPLY);
	sxw_u7(&sx_w, SX_PROTOCOL_VERSION);
	sxw_u7(&sx_w, LXR_FW_MAJOR);
	sxw_u7(&sx_w, LXR_FW_MINOR);
	sxw_u7(&sx_w, LXR_FW_PATCH);
	for(uint8_t i = 0; i < 8; i++)
		sxw_u7(&sx_w, i < sizeof(version) - 1 ? (uint8_t)version[i] : 0);
	sxw_u14(&sx_w, NUM_PARAMS);
	sxw_u14(&sx_w, END_OF_SOUND_PARAMETERS);
	sxw_u14(&sx_w, PAR_BEGINNING_OF_GLOBALS);
	sxw_u7(&sx_w, NUM_KIT_SLOTS);
	sxw_u7(&sx_w, NUM_PATTERN);
	sxw_u7(&sx_w, NUM_TRACKS);
	sxw_u7(&sx_w, NUM_STEPS);
	sxw_u14(&sx_w, PATTERN_DATA_LEN);
	sxw_u14(&sx_w, SX_MAX_FRAME);
	sxw_u14(&sx_w, SX_CAP_KITS | SX_CAP_PATTERNS | SX_CAP_GLOBALS | SX_CAP_PARAMS
		| SX_CAP_ECHO | SX_CAP_SAVE | SX_CAP_CPU_METER | SX_CAP_DRUM_ENGINE | SX_CAP_FX_FREEZE);
	sxw_u7(&sx_w, sx_ownDevByte());
	sxw_u7(&sx_w, activeParameters[PAR_SYSEX_ECHO] ? 1 : 0);
	sx_send(sx_replyPorts);
}
//---------------------------------------------------------------------------
static void sx_cmdUniversalIdentity(void)
{
	static const uint8_t tail[] = {
		0x06, 0x02, SX_MANUFACTURER_ID, SX_MODEL_ID, 0x00, 0x01, 0x00,
		LXR_FW_MAJOR, LXR_FW_MINOR, LXR_FW_PATCH, SX_PROTOCOL_VERSION, 0xF7 };
	sx_txBuf[0] = 0xF0;
	sx_txBuf[1] = 0x7E;
	sx_txBuf[2] = sx_ownDevByte();
	memcpy(&sx_txBuf[3], tail, sizeof(tail));
	sx_txLen = 3 + sizeof(tail);
	sx_txDinPos = 0;
	sx_txDinStarted = 0;
	sx_txPending = sx_replyPorts;
}
//---------------------------------------------------------------------------
static void sx_cmdStatus(SxReader *r)
{
	// optional flags byte (SX_STATUS_*), an empty payload means no flags
	const uint8_t flags = sxr_remaining(r) ? sxr_u7(r) : 0;

	CpuMeterReport cpu;
	cpuMeter_report(&cpuMeter, &cpu);
	const SxStatus st = {
		.kit = menu_loadedPresetNr[CURRENT_KIT],
		.pattern = seq_activePattern,
		.running = seq_isRunning() ? 1 : 0,
		.echo = activeParameters[PAR_SYSEX_ECHO] ? 1 : 0,
		.cpuNow = cpu.now,
		.cpuPeak = cpu.peak,
		.cpuVoices = cpu.section[CPU_SECTION_VOICES],
		.cpuMixer = cpu.section[CPU_SECTION_MIXER],
		.cpuFx = cpu.section[CPU_SECTION_FX],
		.underruns = cpu.underruns,
	};
	sx_begin(SX_CMD_STATUS_REPLY);
	sx_writeStatus(&sx_w, &st);
	sx_send(sx_replyPorts);

	if(flags & SX_STATUS_RESET_PEAK)
		cpuMeter_resetPeak(&cpuMeter);
}
//---------------------------------------------------------------------------
static void sx_cmdKitRequest(SxReader *r)
{
	const uint8_t slot = sxr_u7(r);
	if(r->err) { sx_nak(SX_CMD_KIT_REQ, SX_ERR_LENGTH, 0); return; }

	const uint8_t *params;
	const char *name;
	if(slot == 0x7F)
	{
		params = activeParameters;
		name = savefileNames[menu_loadedPresetNr[CURRENT_KIT] % NUM_KIT_SLOTS][KIT_NAMES];
	}
	else if(slot < NUM_KIT_SLOTS)
	{
		params = mainParameters[slot];
		name = savefileNames[slot][KIT_NAMES];
	}
	else
	{
		sx_nak(SX_CMD_KIT_REQ, SX_ERR_RANGE, slot);
		return;
	}

	memcpy(sx_scratch, name, NAME_LEN);
	memcpy(&sx_scratch[NAME_LEN], params, END_OF_SOUND_PARAMETERS);
	memcpy(&sx_scratch[NAME_LEN + END_OF_SOUND_PARAMETERS], KIT_ENGINE_TAG, KIT_ENGINE_TAG_SIZE);

	sx_begin(SX_CMD_KIT_DUMP);
	sxw_u7(&sx_w, slot);
	sxw_u7(&sx_w, 0);
	sxw_u14(&sx_w, END_OF_SOUND_PARAMETERS);
	sxw_packed(&sx_w, sx_scratch, KIT_DATA_LEN);
	sx_send(sx_replyPorts);
}
//---------------------------------------------------------------------------
static void sx_cmdKitDump(SxReader *r)
{
	const uint8_t slot = sxr_u7(r);
	const uint8_t flags = sxr_u7(r);
	const uint16_t n = sxr_u14(r);
	if(r->err) { sx_nak(SX_CMD_KIT_DUMP, SX_ERR_LENGTH, 0); return; }
	if(slot != 0x7F && slot >= NUM_KIT_SLOTS) { sx_nak(SX_CMD_KIT_DUMP, SX_ERR_RANGE, slot); return; }
	if(slot == 0x7F && (flags & SX_FLAG_SAVE)) { sx_nak(SX_CMD_KIT_DUMP, SX_ERR_RANGE, slot); return; }

	const int32_t rawLen = sx_unpack(sxr_ptr(r), sxr_remaining(r), sx_scratch, sizeof(sx_scratch));
	if(rawLen < 0 || rawLen < (int32_t)(NAME_LEN + n)) { sx_nak(SX_CMD_KIT_DUMP, SX_ERR_LENGTH, slot); return; }

	// byte-exact like an SD kit load, only invalid mod-target indices are reset
	const uint8_t *values = &sx_scratch[NAME_LEN];
	const uint8_t nmt = getNumModTargets();
	const uint8_t numModIdx = (uint8_t)(sizeof(sx_kitModIdx) / sizeof(sx_kitModIdx[0]));
	const size_t tagOffset = NAME_LEN + END_OF_SOUND_PARAMETERS;
	const size_t tagSize = n == END_OF_SOUND_PARAMETERS && rawLen >= (int32_t)tagOffset ?
		(size_t)rawLen - tagOffset : 0;

	if(slot == 0x7F)
	{
		sx_kitApply(activeParameters, END_OF_SOUND_PARAMETERS, values, n, sx_kitModIdx, numModIdx, nmt);
		kitEngine_migrate(activeParameters, END_OF_SOUND_PARAMETERS, &sx_scratch[tagOffset], tagSize);
		preset_sendDrumsetParameters();
		menu_editBufferReplaced();
		preset_touchTimer(PRESET_KIT_TIMER);
		menu_repaintAll();
	}
	else
	{
		sx_kitApply(mainParameters[slot], END_OF_SOUND_PARAMETERS, values, n, sx_kitModIdx, numModIdx, nmt);
		kitEngine_migrate(mainParameters[slot], END_OF_SOUND_PARAMETERS, &sx_scratch[tagOffset], tagSize);
		sx_copyName(savefileNames[slot][KIT_NAMES], sx_scratch);
		if(flags & SX_FLAG_SAVE)
		{
			const FRESULT res = preset_saveDrumsetSD(slot, 0);
			if(res != FR_OK) { sx_nak(SX_CMD_KIT_DUMP, SX_ERR_SD, res); return; }
		}
	}
	sx_syncShadow();
	sx_ack(SX_CMD_KIT_DUMP, slot);
}
//---------------------------------------------------------------------------
static void sx_cmdKitStore(SxReader *r)
{
	const uint8_t slot = sxr_u7(r);
	const uint8_t flags = sxr_u7(r);
	if(r->err) { sx_nak(SX_CMD_KIT_STORE, SX_ERR_LENGTH, 0); return; }
	if(slot >= NUM_KIT_SLOTS) { sx_nak(SX_CMD_KIT_STORE, SX_ERR_RANGE, slot); return; }

	const uint8_t current = menu_loadedPresetNr[CURRENT_KIT] % NUM_KIT_SLOTS;
	preset_saveDrumsetRam(slot, 0);
	if(slot != current)
		memcpy(savefileNames[slot][KIT_NAMES], savefileNames[current][KIT_NAMES], NAME_LEN);
	menu_loadedPresetNr[CURRENT_KIT] = slot;
	menu_selectedPresetNr[SAVE_TYPE_KIT] = slot;
	sx_syncShadow();

	if(flags & SX_FLAG_SAVE)
	{
		const FRESULT res = preset_saveDrumsetSD(slot, 0);
		if(res != FR_OK) { sx_nak(SX_CMD_KIT_STORE, SX_ERR_SD, res); return; }
	}
	menu_repaintAll();
	sx_ack(SX_CMD_KIT_STORE, slot);
}
//---------------------------------------------------------------------------
static void sx_cmdKitLoad(SxReader *r)
{
	const uint8_t slot = sxr_u7(r);
	if(r->err) { sx_nak(SX_CMD_KIT_LOAD, SX_ERR_LENGTH, 0); return; }
	if(slot >= NUM_KIT_SLOTS) { sx_nak(SX_CMD_KIT_LOAD, SX_ERR_RANGE, slot); return; }

	menu_loadKit(slot);
	menu_repaintAll();
	sx_syncShadow();
	sx_ack(SX_CMD_KIT_LOAD, slot);
}
//---------------------------------------------------------------------------
static void sx_cmdName(uint8_t cmd, SxReader *r)
{
	uint8_t slot = sxr_u7(r);
	if(r->err) { sx_nak(cmd, SX_ERR_LENGTH, 0); return; }
	const uint8_t requested = slot;
	if(slot == 0x7F)
		slot = (cmd == SX_CMD_KIT_NAME) ? menu_loadedPresetNr[CURRENT_KIT] : seq_activePattern;
	if(slot >= NUM_KIT_SLOTS) { sx_nak(cmd, SX_ERR_RANGE, requested); return; }

	uint8_t name[NAME_LEN];
	const int32_t rawLen = sx_unpack(sxr_ptr(r), sxr_remaining(r), name, sizeof(name));
	if(rawLen != NAME_LEN) { sx_nak(cmd, SX_ERR_LENGTH, requested); return; }

	sx_copyName(savefileNames[slot][cmd == SX_CMD_KIT_NAME ? KIT_NAMES : PATTERN_NAMES], name);
	menu_repaintAll();
	sx_ack(cmd, requested);
}
//---------------------------------------------------------------------------
static void sx_streamNextChunk(void)
{
	const uint16_t offset = (uint16_t)(sx_streamIdx * PATTERN_CHUNK_LEN);
	uint16_t n = (uint16_t)(PATTERN_DATA_LEN - offset);
	if(n > PATTERN_CHUNK_LEN) n = PATTERN_CHUNK_LEN;

	sx_begin(SX_CMD_PATTERN_CHUNK);
	sxw_u7(&sx_w, sx_streamSlot);
	sxw_u7(&sx_w, 0);
	sxw_u7(&sx_w, sx_streamIdx);
	sxw_u7(&sx_w, PATTERN_NUM_CHUNKS);
	sxw_u14(&sx_w, PATTERN_DATA_LEN);
	sxw_packed(&sx_w, &sx_patternBuf[offset], n);
	sx_send(sx_streamPorts);

	sx_streamIdx++;
	if(sx_streamIdx >= PATTERN_NUM_CHUNKS) sx_streamActive = 0;
}
//---------------------------------------------------------------------------
static void sx_cmdPatternRequest(SxReader *r)
{
	uint8_t slot = sxr_u7(r);
	if(r->err) { sx_nak(SX_CMD_PATTERN_REQ, SX_ERR_LENGTH, 0); return; }
	const uint8_t requested = slot;
	if(slot == 0x7F) slot = seq_activePattern;
	if(slot >= NUM_PATTERN) { sx_nak(SX_CMD_PATTERN_REQ, SX_ERR_RANGE, requested); return; }

	// the staging buffer is shared with uploads
	sx_rxPatActive = 0;

	memcpy(sx_patternBuf, savefileNames[slot][PATTERN_NAMES], NAME_LEN);
	const uint32_t primask = sx_irqSave();
	sx_patternTransfer(slot, &sx_patternBuf[NAME_LEN], 0);
	sx_irqRestore(primask);

	sx_streamSlot = slot;
	sx_streamIdx = 0;
	sx_streamPorts = sx_replyPorts;
	sx_streamActive = 1;
	sx_streamNextChunk();
}
//---------------------------------------------------------------------------
static void sx_cmdPatternChunk(SxReader *r)
{
	uint8_t slot = sxr_u7(r);
	const uint8_t flags = sxr_u7(r);
	const uint8_t idx = sxr_u7(r);
	const uint8_t count = sxr_u7(r);
	const uint16_t total = sxr_u14(r);
	if(r->err) { sx_rxPatActive = 0; sx_nak(SX_CMD_PATTERN_CHUNK, SX_ERR_LENGTH, 0); return; }

	if(slot == 0x7F) slot = seq_activePattern;
	if(slot >= NUM_PATTERN) { sx_rxPatActive = 0; sx_nak(SX_CMD_PATTERN_CHUNK, SX_ERR_RANGE, idx); return; }

	if(idx == 0)
	{
		if(count == 0 || total == 0) { sx_rxPatActive = 0; sx_nak(SX_CMD_PATTERN_CHUNK, SX_ERR_SEQUENCE, idx); return; }
		if(total != PATTERN_DATA_LEN) { sx_rxPatActive = 0; sx_nak(SX_CMD_PATTERN_CHUNK, SX_ERR_LENGTH, idx); return; }
		sx_rxPatActive = 1;
		sx_rxPatSlot = slot;
		sx_rxPatFlags = flags;
		sx_rxPatCount = count;
		sx_rxPatTotal = total;
		sx_rxPatReceived = 0;
		sx_rxPatNextIdx = 0;
	}
	if(!sx_rxPatActive || idx != sx_rxPatNextIdx || slot != sx_rxPatSlot
			|| count != sx_rxPatCount || total != sx_rxPatTotal || idx >= count)
	{
		sx_rxPatActive = 0;
		sx_nak(SX_CMD_PATTERN_CHUNK, SX_ERR_SEQUENCE, idx);
		return;
	}

	const int32_t rawLen = sx_unpack(sxr_ptr(r), sxr_remaining(r), &sx_patternBuf[sx_rxPatReceived],
			(uint16_t)(sx_rxPatTotal - sx_rxPatReceived));
	if(rawLen <= 0 || rawLen > PATTERN_CHUNK_LEN)
	{
		sx_rxPatActive = 0;
		sx_nak(SX_CMD_PATTERN_CHUNK, SX_ERR_LENGTH, idx);
		return;
	}
	sx_rxPatReceived = (uint16_t)(sx_rxPatReceived + rawLen);
	sx_rxPatNextIdx++;

	if(idx + 1 < count)
	{
		sx_ack(SX_CMD_PATTERN_CHUNK, idx);
		return;
	}

	// last chunk: commit
	sx_rxPatActive = 0;
	if(sx_rxPatReceived != PATTERN_DATA_LEN) { sx_nak(SX_CMD_PATTERN_CHUNK, SX_ERR_LENGTH, idx); return; }

	const uint32_t primask = sx_irqSave();
	sx_patternTransfer(slot, &sx_patternBuf[NAME_LEN], 1);
	sx_irqRestore(primask);
	sx_copyName(savefileNames[slot][PATTERN_NAMES], sx_patternBuf);

	if(slot == seq_activePattern)
	{
		// make it permanent, otherwise switching patterns restores the backup
		seq_copyPatternToBackupBuffer(slot);
		activeParameters[PAR_SHUFFLE] = seq_patternSet.shuffle[slot];
		seq_setShuffle(activeParameters[PAR_SHUFFLE] / 127.f);
	}
	if(sx_rxPatFlags & SX_FLAG_SAVE)
	{
		const FRESULT res = preset_savePatternSD(slot);
		if(res != FR_OK) { sx_nak(SX_CMD_PATTERN_CHUNK, SX_ERR_SD, res); return; }
	}
	sx_syncShadow();
	menu_repaintAll();
	sx_ack(SX_CMD_PATTERN_CHUNK, idx);
}
//---------------------------------------------------------------------------
static void sx_cmdGlobalsRequest(void)
{
	sx_begin(SX_CMD_GLOBALS_DUMP);
	sxw_u7(&sx_w, 0);
	sxw_u14(&sx_w, PAR_BEGINNING_OF_GLOBALS);
	sxw_u14(&sx_w, NUM_GLOBALS);
	sxw_packed(&sx_w, &activeParameters[PAR_BEGINNING_OF_GLOBALS], NUM_GLOBALS);
	sx_send(sx_replyPorts);
}
//---------------------------------------------------------------------------
static void sx_cmdGlobalsDump(SxReader *r)
{
	const uint8_t flags = sxr_u7(r);
	const uint16_t first = sxr_u14(r);
	const uint16_t n = sxr_u14(r);
	if(r->err) { sx_nak(SX_CMD_GLOBALS_DUMP, SX_ERR_LENGTH, 0); return; }
	if(first != PAR_BEGINNING_OF_GLOBALS) { sx_nak(SX_CMD_GLOBALS_DUMP, SX_ERR_RANGE, first); return; }

	const int32_t rawLen = sx_unpack(sxr_ptr(r), sxr_remaining(r), sx_scratch, sizeof(sx_scratch));
	if(rawLen < 0 || rawLen < (int32_t)n) { sx_nak(SX_CMD_GLOBALS_DUMP, SX_ERR_LENGTH, n); return; }

	const uint16_t count = n < NUM_GLOBALS ? n : NUM_GLOBALS;
	for(uint16_t i = 0; i < count; i++)
	{
		const uint16_t id = (uint16_t)(PAR_BEGINNING_OF_GLOBALS + i);
		if(sx_isReadOnly(id)) continue;
		sx_applyParam(id, sx_clampValue(id, sx_scratch[i]));
	}
	sx_syncShadow();
	menu_repaintAll();

	if(flags & SX_FLAG_SAVE)
	{
		const FRESULT res = preset_saveGlobals();
		if(res != FR_OK) { sx_nak(SX_CMD_GLOBALS_DUMP, SX_ERR_SD, res); return; }
	}
	sx_ack(SX_CMD_GLOBALS_DUMP, n);
}
//---------------------------------------------------------------------------
static void sx_cmdParamGet(SxReader *r)
{
	const uint16_t n = sxr_remaining(r) / 2;
	if(n == 0 || n > MAX_PARAMS_PER_MSG || (sxr_remaining(r) & 1))
	{
		sx_nak(SX_CMD_PARAM_GET, SX_ERR_LENGTH, 0);
		return;
	}
	sx_begin(SX_CMD_PARAM_VALUE);
	for(uint16_t i = 0; i < n; i++)
	{
		const uint16_t id = sxr_u14(r);
		if(id >= NUM_PARAMS) { sx_nak(SX_CMD_PARAM_GET, SX_ERR_RANGE, id); return; }
		sxw_u14(&sx_w, id);
		sxw_u14(&sx_w, activeParameters[id]);
	}
	sx_send(sx_replyPorts);
}
//---------------------------------------------------------------------------
static void sx_cmdParamSet(SxReader *r)
{
	const uint16_t n = sxr_remaining(r) / 4;
	if(n == 0 || n > MAX_PARAMS_PER_MSG || (sxr_remaining(r) & 3))
	{
		sx_nak(SX_CMD_PARAM_SET, SX_ERR_LENGTH, 0);
		return;
	}
	uint8_t touchedKit = 0;
	for(uint16_t i = 0; i < n; i++)
	{
		const uint16_t id = sxr_u14(r);
		const uint16_t value = sxr_u14(r);
		if(id >= NUM_PARAMS) { sx_nak(SX_CMD_PARAM_SET, SX_ERR_RANGE, id); break; }
		if(sx_isReadOnly(id)) { sx_nak(SX_CMD_PARAM_SET, SX_ERR_READ_ONLY, id); break; }
		const uint8_t v = sx_clampValue(id, value);
		sx_applyParam(id, v);
		sx_shadow[id] = v;
		if(id < END_OF_SOUND_PARAMETERS) touchedKit = 1;
	}
	if(touchedKit) preset_touchTimer(PRESET_KIT_TIMER);
	menu_repaint();
}
//---------------------------------------------------------------------------
static void sx_cmdEchoSet(SxReader *r)
{
	const uint8_t on = sxr_u7(r);
	if(r->err) { sx_nak(SX_CMD_ECHO_SET, SX_ERR_LENGTH, 0); return; }
	if(on > 1) { sx_nak(SX_CMD_ECHO_SET, SX_ERR_RANGE, on); return; }
	activeParameters[PAR_SYSEX_ECHO] = on;
	sx_shadow[PAR_SYSEX_ECHO] = on;
	menu_repaint();
	sx_ack(SX_CMD_ECHO_SET, on);
}
//---------------------------------------------------------------------------
// momentary performance control: no reply on success (like PARAM_SET)
static void sx_cmdFxFreeze(SxReader *r)
{
	const uint8_t value = sxr_u7(r);
	if(r->err) { sx_nak(SX_CMD_FX_FREEZE, SX_ERR_LENGTH, 0); return; }
	if(!FxBus_freezeControl(value)) sx_nak(SX_CMD_FX_FREEZE, SX_ERR_RANGE, fxBus.fxType);
}
//---------------------------------------------------------------------------
static void sx_dispatch(const SxFrame *f)
{
	SxReader r;
	sxr_init(&r, f->payload, f->payloadLen);

	switch(f->cmd)
	{
	case SX_CMD_IDENTITY_REQ:	sx_cmdIdentity(); break;
	case SX_CMD_STATUS_REQ:		sx_cmdStatus(&r); break;
	case SX_CMD_KIT_REQ:		sx_cmdKitRequest(&r); break;
	case SX_CMD_KIT_DUMP:		sx_cmdKitDump(&r); break;
	case SX_CMD_KIT_STORE:		sx_cmdKitStore(&r); break;
	case SX_CMD_KIT_LOAD:		sx_cmdKitLoad(&r); break;
	case SX_CMD_KIT_NAME:		sx_cmdName(SX_CMD_KIT_NAME, &r); break;
	case SX_CMD_PATTERN_REQ:	sx_cmdPatternRequest(&r); break;
	case SX_CMD_PATTERN_CHUNK:	sx_cmdPatternChunk(&r); break;
	case SX_CMD_PATTERN_NAME:	sx_cmdName(SX_CMD_PATTERN_NAME, &r); break;
	case SX_CMD_GLOBALS_REQ:	sx_cmdGlobalsRequest(); break;
	case SX_CMD_GLOBALS_DUMP:	sx_cmdGlobalsDump(&r); break;
	case SX_CMD_PARAM_GET:		sx_cmdParamGet(&r); break;
	case SX_CMD_PARAM_SET:		sx_cmdParamSet(&r); break;
	case SX_CMD_ECHO_SET:		sx_cmdEchoSet(&r); break;
	case SX_CMD_FX_FREEZE:		sx_cmdFxFreeze(&r); break;
	default:					sx_nak(f->cmd, SX_ERR_UNKNOWN_CMD, 0); break;
	}
}
//---------------------------------------------------------------------------
static void sx_handleAssembled(SxAssembler *a, SxAsmResult res, uint8_t port)
{
	const uint8_t devId = sx_deviceId();

	if(res == SXA_OVERFLOW)
	{
		if(a->buf[1] == SX_MANUFACTURER_ID && a->buf[2] == SX_MODEL_ID && sx_isForDevice(a->buf[3], devId))
		{
			sx_hostPorts = sx_replyPorts = port;
			sx_nak(a->buf[5], SX_ERR_LENGTH, 0);
		}
		return;
	}

	SxFrame f;
	switch(sx_parseFrame(a->buf, a->len, &f))
	{
	case SX_PARSE_UNIVERSAL_ID:
		if(sx_isForDevice(f.devByte, devId))
		{
			sx_hostPorts = sx_replyPorts = port;
			sx_cmdUniversalIdentity();
		}
		break;
	case SX_PARSE_BAD_CHECKSUM:
		if(sx_isForDevice(f.devByte, devId))
		{
			sx_replyPorts = port;
			sx_nak(f.cmd, SX_ERR_CHECKSUM, 0);
		}
		break;
	case SX_PARSE_OK:
		if(!sx_isForDevice(f.devByte, devId)) break;
		sx_hostPorts = sx_replyPorts = port;
		if(f.version != SX_PROTOCOL_VERSION && f.cmd != SX_CMD_IDENTITY_REQ)
		{
			sx_nak(f.cmd, SX_ERR_VERSION, f.version);
			break;
		}
		sx_dispatch(&f);
		break;
	default:
		break;
	}
}
//---------------------------------------------------------------------------
static uint8_t sx_pollPort(RxRing *ring, SxAssembler *a, uint8_t port)
{
	uint8_t b;
	if(!sx_ringPop(ring, &b)) return 0;
	const SxAsmResult res = sxa_feed(a, b);
	if(res != SXA_NONE) sx_handleAssembled(a, res, port);
	return 1;
}
//---------------------------------------------------------------------------
static void sx_echoTick(void)
{
	if(!activeParameters[PAR_SYSEX_ECHO])
	{
		sx_echoWasOn = 0;
		return;
	}
	const uint32_t now = HAL_GetTick();
	if(!sx_echoWasOn)
	{
		sx_echoWasOn = 1;
		sx_syncShadow();
		sx_lastScan = now;
		return;
	}
	if(now - sx_lastScan < ECHO_INTERVAL_MS) return;
	sx_lastScan = now;

	const uint8_t ports = sx_hostPorts ? sx_hostPorts : (PORT_DIN | PORT_USB);

	if(seq_activePattern != sx_lastPattern)
	{
		sx_lastPattern = seq_activePattern;
		sx_event(SX_EVENT_PATTERN_CHANGED, seq_activePattern, ports);
		return;
	}
	if(menu_loadedPresetNr[CURRENT_KIT] != sx_lastKit)
	{
		sx_syncShadow();
		sx_event(SX_EVENT_KIT_CHANGED, menu_loadedPresetNr[CURRENT_KIT], ports);
		return;
	}

	uint16_t changed[ECHO_MAX_PAIRS];
	uint8_t numChanged = 0;
	uint8_t tooMany = 0;
	for(uint16_t id = 0; id < NUM_PARAMS; id++)
	{
		if(id == END_OF_SOUND_PARAMETERS) id = PAR_BEGINNING_OF_GLOBALS; // skip step/track UI context
		const uint8_t v = activeParameters[id];
		if(sx_extChange[id])
		{
			sx_extChange[id] = 0;
			sx_shadow[id] = v;
			continue;
		}
		if(v == sx_shadow[id]) continue;
		if(numChanged < ECHO_MAX_PAIRS) changed[numChanged++] = id;
		else tooMany = 1;
	}

	if(tooMany)
	{
		sx_syncShadow();
		sx_event(SX_EVENT_KIT_CHANGED, menu_loadedPresetNr[CURRENT_KIT], ports);
		return;
	}
	if(!numChanged) return;

	sx_begin(SX_CMD_PARAM_NOTIFY);
	for(uint8_t i = 0; i < numChanged; i++)
	{
		const uint16_t id = changed[i];
		const uint8_t v = activeParameters[id];
		sx_shadow[id] = v;
		sxw_u14(&sx_w, id);
		sxw_u14(&sx_w, v);
	}
	sx_send(ports);
}
//---------------------------------------------------------------------------
void sysex_tick(void)
{
	// let MIDI bytes queued behind a SysEx frame go out
	uart_txPoll();

	if(!sx_enabled())
	{
		sx_rxDin.rd = sx_rxDin.wr;
		sx_rxUsb.rd = sx_rxUsb.wr;
		sxa_reset(&sx_asmDin);
		sxa_reset(&sx_asmUsb);
		sx_rxPatActive = 0;
		sx_streamActive = 0;
		sx_echoWasOn = 0;
		if((sx_txPending & PORT_DIN) && sx_txDinStarted)
		{
			// finish a started DIN frame so the receiver sees a complete message
			sx_txPump();
			return;
		}
		sx_txPending = 0;
		return;
	}

	sx_txPump();

	if(!sx_txPending && sx_streamActive)
	{
		sx_streamNextChunk();
		sx_txPump();
	}

	for(uint16_t i = 0; i < RX_BYTES_PER_TICK && !sx_txBusy(); i++)
	{
		uint8_t got = sx_pollPort(&sx_rxUsb, &sx_asmUsb, PORT_USB);
		if(sx_txBusy()) break;
		got |= sx_pollPort(&sx_rxDin, &sx_asmDin, PORT_DIN);
		if(!got) break;
	}

	if(!sx_txBusy()) sx_echoTick();

	if(sx_txPending) sx_txPump();
}
