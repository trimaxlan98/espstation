# Host unit tests for esps_proto. This is the CI-relevant gate for the codec:
# it must build and run with plain gcc, with no ESP-IDF anywhere on the path,
# so a contributor without the ESP32 toolchain still exercises the frame
# format on every change. ASan+UBSan are non-negotiable here: a codec that
# rejects malformed input by returning an error code is only proven safe if a
# sanitizer confirms it never touched memory past the check.

CC ?= gcc
PROTO_DIR := ../../components/esps_proto
CFLAGS := -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I$(PROTO_DIR)/include
# esps_proto is forbidden from allocating memory, so LeakSanitizer adds no
# coverage and cannot run under ptrace-based agent sandboxes. ASan and UBSan
# remain active for the buffer/undefined-behaviour checks this gate exists for.
ASAN_OPTIONS ?= detect_leaks=0

SRC := $(PROTO_DIR)/src/crc16.c $(PROTO_DIR)/src/cobs.c $(PROTO_DIR)/src/enlp.c
TEST_SRC := test_crc16.c test_cobs.c test_enlp.c main.c
BIN := esps_proto_tests

.PHONY: test clean

test: $(BIN)
	ASAN_OPTIONS=$(ASAN_OPTIONS) ./$(BIN)

$(BIN): $(SRC) $(TEST_SRC) harness.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(TEST_SRC)

clean:
	rm -f $(BIN)
