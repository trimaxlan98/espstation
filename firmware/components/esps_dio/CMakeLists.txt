# esps_dio has two halves, and the split is load-bearing.
#
# The pure C11 half (crc8, frame, stats, testgen, pins, policy) has no ESP-IDF
# dependency and is compiled verbatim by test/host/ under plain gcc with
# ASan+UBSan. It is listed first so it stays obvious which files must remain
# portable: frame formats, CRC, BER accounting, link hysteresis, rate limiting
# and scheduling are all exercised on the host, not on a bench.
#
# The ESP-IDF half (esps_dio.c) owns only what genuinely needs the driver:
# GPIO configuration, interrupt handlers, bit-bang timing and a FreeRTOS task.
#
# linker.lf places the interrupt-reachable pure objects in IRAM; see that file
# for why that is a correctness requirement and not an optimisation. Every
# ESP-IDF dependency is private, because esps_dio.h exposes none of them —
# main.c includes it without inheriting ESP-IDF types.
idf_component_register(
    SRCS "src/crc8.c" "src/frame.c" "src/stats.c" "src/testgen.c" "src/pins.c" "src/policy.c"
         "src/esps_dio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esps_core driver esp_timer esp_hw_support freertos hal soc log
    LDFRAGMENTS "linker.lf")
