# Host unit tests for the pure-C components (esps_proto, and the pure half of
# esps_dio). This is the CI-relevant gate for the codecs: they must build and
# run with plain gcc, with no ESP-IDF anywhere on the path, so a contributor
# without the ESP32 toolchain still exercises the frame formats on every
# change. ASan+UBSan are non-negotiable here: a codec that rejects malformed
# input by returning an error code is only proven safe if a sanitizer confirms
# it never touched memory past the check.

CC ?= gcc
PROTO_DIR := ../../components/esps_proto
DIO_DIR := ../../components/esps_dio
CFLAGS := -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
          -I$(PROTO_DIR)/include -I$(DIO_DIR)/include
# esps_proto and esps_dio are forbidden from allocating memory, so
# LeakSanitizer adds no coverage and cannot run under ptrace-based agent
# sandboxes. ASan and UBSan remain active for the buffer/undefined-behaviour
# checks this gate exists for.
ASAN_OPTIONS ?= detect_leaks=0

SRC := $(PROTO_DIR)/src/crc16.c $(PROTO_DIR)/src/cobs.c $(PROTO_DIR)/src/enlp.c \
       $(DIO_DIR)/src/crc8.c $(DIO_DIR)/src/frame.c $(DIO_DIR)/src/stats.c \
       $(DIO_DIR)/src/testgen.c $(DIO_DIR)/src/pins.c $(DIO_DIR)/src/policy.c
TEST_SRC := test_crc16.c test_cobs.c test_enlp.c \
            test_dio_crc8.c test_dio_frame.c test_dio_stats.c test_dio_testgen.c test_dio_pins.c \
            test_dio_policy.c \
            main.c
BIN := esps_proto_tests

.PHONY: test clean

test: $(BIN)
	ASAN_OPTIONS=$(ASAN_OPTIONS) ./$(BIN)

$(BIN): $(SRC) $(TEST_SRC) harness.h dio_helpers.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(TEST_SRC)

clean:
	rm -f $(BIN)
