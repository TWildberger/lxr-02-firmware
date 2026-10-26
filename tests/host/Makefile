# Host-side unit tests for the HAL-free SysEx codec, CPU meter, reverb and freeze. Uses the
# system C compiler.
#   make -C tests/host        build and run
#   make -C tests/host clean
ROOT    := ../..
CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
CFLAGS  += -I$(ROOT)/Core/Inc
BUILD   := build

.PHONY: test clean
test: $(BUILD)/test_sysex_codec $(BUILD)/test_cpu_meter $(BUILD)/test_reverb $(BUILD)/test_freeze
	./$(BUILD)/test_sysex_codec
	./$(BUILD)/test_cpu_meter
	./$(BUILD)/test_reverb
	./$(BUILD)/test_freeze

$(BUILD)/test_sysex_codec: test_sysex_codec.c $(ROOT)/Core/Src/Sysex/SysexCodec.c $(ROOT)/Core/Inc/Sysex/SysexCodec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_sysex_codec.c $(ROOT)/Core/Src/Sysex/SysexCodec.c

$(BUILD)/test_cpu_meter: test_cpu_meter.c $(ROOT)/Core/Src/Soundengine/CpuMeter.c $(ROOT)/Core/Inc/Soundengine/CpuMeter.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DCPU_METER_NO_HW -o $@ test_cpu_meter.c $(ROOT)/Core/Src/Soundengine/CpuMeter.c

$(BUILD)/test_reverb: test_reverb.c $(ROOT)/Core/Src/Soundengine/DSPAudio/FX/Reverb.c $(ROOT)/Core/Inc/Soundengine/DSPAudio/FX/Reverb.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_reverb.c $(ROOT)/Core/Src/Soundengine/DSPAudio/FX/Reverb.c -lm

$(BUILD)/test_freeze: test_freeze.c $(ROOT)/Core/Src/Soundengine/DSPAudio/FX/Freeze.c $(ROOT)/Core/Inc/Soundengine/DSPAudio/FX/Freeze.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_freeze.c $(ROOT)/Core/Src/Soundengine/DSPAudio/FX/Freeze.c -lm

clean:
	rm -rf $(BUILD)
