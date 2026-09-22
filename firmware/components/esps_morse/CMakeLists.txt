# esps_morse — the full-duplex Morse link (bench/practicas/morse-duplex/).
#
# Two halves, and the split is load-bearing, exactly as in esps_dio.
#
# The pure C11 half (table, decode, key) has no ESP-IDF dependency and is
# compiled verbatim by test/host/ under plain gcc with ASan+UBSan. It is
# listed first so it stays obvious which files must remain portable: the Morse
# table, the pulse/silence state machine and the key debounce are all
# exercised on the host, not on a bench — including the micros() and millis()
# wraps that no bench session is long enough to reach.
#
# The ESP-IDF half (esps_morse.c) owns only what genuinely needs the driver:
# GPIO configuration, the two edge ISRs and a 1 ms FreeRTOS task.
#
# There is no LDFRAGMENTS here, unlike esps_dio: its ISR runs the frame
# decoder and CRC, so those objects must live in IRAM. These ISRs only read
# the clock, read a pin and write a ring slot — they call nothing from the
# pure half — so IRAM_ATTR on the handlers is the whole requirement.
#
# TODO(S3): the thresholds belong in the experiment spec delivered by EXP_SET
# and persisted to NVS, not in build-time defaults — the same note esps_dio
# carries, for the same reason.
idf_component_register(
    SRCS "src/table.c" "src/decode.c" "src/key.c"
         "src/esps_morse.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esps_core driver esp_timer freertos log)
