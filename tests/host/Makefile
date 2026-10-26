# Host-side unit tests for the HAL-free SysEx codec. Uses the system C compiler.
#   make -C tests/host        build and run
#   make -C tests/host clean
ROOT    := ../..
CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
CFLAGS  += -I$(ROOT)/Core/Inc
BUILD   := build

.PHONY: test clean
test: $(BUILD)/test_sysex_codec
	./$(BUILD)/test_sysex_codec

$(BUILD)/test_sysex_codec: test_sysex_codec.c $(ROOT)/Core/Src/Sysex/SysexCodec.c $(ROOT)/Core/Inc/Sysex/SysexCodec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_sysex_codec.c $(ROOT)/Core/Src/Sysex/SysexCodec.c

clean:
	rm -rf $(BUILD)
