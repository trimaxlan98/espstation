/* Minimal header-only assert harness for the esps_proto host tests. No
 * external test framework: each test file owns a local failure counter
 * (passed by pointer) rather than a shared global, so files can be compiled
 * and linked together without symbol clashes.
 */
#ifndef ESPS_TEST_HARNESS_H
#define ESPS_TEST_HARNESS_H

#include <stdio.h>

#define ESPS_CHECK(failp, cond)                                                      \
    do {                                                                             \
        if (!(cond)) {                                                               \
            (*(failp))++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                            \
    } while (0)

#define ESPS_CHECK_EQ(failp, a, b)                                                   \
    do {                                                                             \
        long long _a = (long long)(a);                                              \
        long long _b = (long long)(b);                                              \
        if (_a != _b) {                                                              \
            (*(failp))++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n", __FILE__,        \
                    __LINE__, #a, _a, #b, _b);                                       \
        }                                                                            \
    } while (0)

#endif /* ESPS_TEST_HARNESS_H */
