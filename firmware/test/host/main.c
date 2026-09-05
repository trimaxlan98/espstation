/* Test runner entry point. Each suite reports its own failure count; a
 * single process exit code (0 = all green) is what CI and `make test` key
 * off of.
 */
#include <stdio.h>

int test_crc16_all(void);
int test_cobs_all(void);
int test_enlp_all(void);

int main(void) {
    int fails = 0;

    fprintf(stderr, "== crc16 ==\n");
    fails += test_crc16_all();
    fprintf(stderr, "== cobs ==\n");
    fails += test_cobs_all();
    fprintf(stderr, "== enlp ==\n");
    fails += test_enlp_all();

    if (fails == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", fails);
    return 1;
}
