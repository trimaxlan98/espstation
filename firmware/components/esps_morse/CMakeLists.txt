# esps_morse — the full-duplex Morse link (bench/practicas/morse-duplex/).
#
# Everything here is the PURE C11 half: the table, the pulse/silence state
# machine and the key debounce. It has no ESP-IDF dependency at all, which is
# why test/host/ compiles these same files verbatim under plain gcc with
# ASan+UBSan — the decoder is exercised on every change without anyone
# flashing a board.
#
# There is deliberately no ESP-IDF half yet. The GPIO configuration, the edge
# ISR, the phase task and the NDB publishing that esps_dio.c is for do not
# exist for Morse, so the boards on the bench still run the Arduino sketch and
# reach the station through the gateway's adapter
# (gateway/.../transports/morse_sketch.py, see docs/DECISIONS.md D-22).
# TODO(S3): the thresholds belong in the experiment spec delivered by EXP_SET
# and persisted to NVS, not in build constants — the same note esps_dio
# carries, for the same reason.
idf_component_register(
    SRCS "src/table.c" "src/decode.c" "src/key.c"
    INCLUDE_DIRS "include")
