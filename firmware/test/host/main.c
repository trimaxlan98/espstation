/* Test runner entry point. Each suite reports its own failure count; a
 * single process exit code (0 = all green) is what CI and `make test` key
 * off of.
 */
#include <stdio.h>

int test_crc16_all(void);
int test_cobs_all(void);
int test_enlp_all(void);
int test_dio_crc8_all(void);
int test_dio_frame_all(void);
int test_dio_stats_all(void);
int test_dio_testgen_all(void);
int test_dio_pins_all(void);
int test_dio_policy_all(void);
int test_morse_table_all(void);
int test_morse_decode_all(void);
int test_morse_key_all(void);

int main(void) {
    int fails = 0;

    fprintf(stderr, "== crc16 ==\n");
    fails += test_crc16_all();
    fprintf(stderr, "== cobs ==\n");
    fails += test_cobs_all();
    fprintf(stderr, "== enlp ==\n");
    fails += test_enlp_all();
    fprintf(stderr, "== dio_crc8 ==\n");
    fails += test_dio_crc8_all();
    fprintf(stderr, "== dio_frame ==\n");
    fails += test_dio_frame_all();
    fprintf(stderr, "== dio_stats ==\n");
    fails += test_dio_stats_all();
    fprintf(stderr, "== dio_testgen ==\n");
    fails += test_dio_testgen_all();
    fprintf(stderr, "== dio_pins ==\n");
    fails += test_dio_pins_all();
    fprintf(stderr, "== dio_policy ==\n");
    fails += test_dio_policy_all();
    fprintf(stderr, "== morse_table ==\n");
    fails += test_morse_table_all();
    fprintf(stderr, "== morse_decode ==\n");
    fails += test_morse_decode_all();
    fprintf(stderr, "== morse_key ==\n");
    fails += test_morse_key_all();

    if (fails == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", fails);
    return 1;
}
