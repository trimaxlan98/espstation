/* The International Morse table, as data rather than a chain of comparisons
 * (SPEC-DUPLEX.md "Tabla Morse"). Letters A-Z and digits 0-9: the set the
 * practice uses and the set the golden vectors cover.
 *
 * Pure C11: only <stddef.h> and <stdint.h> reach this file. No allocation, no
 * logging, no ESP-IDF -- it must build under plain gcc for test/host/.
 */
#ifndef ESPS_MORSE_TABLE_H
#define ESPS_MORSE_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest code in the table is five symbols (the digits); the decoder allows
 * eight before declaring a code overflowed, which is what makes a jammed key
 * produce one "unknown" instead of a silent truncation. */
#define ESPS_MORSE_MAX_SYMBOL 8u

typedef struct {
    const char *code; /* e.g. "...", NUL-terminated, only '.' and '-' */
    char c;           /* the character it decodes to                  */
} esps_morse_entry_t;

/* Number of rows. Exposed so a test can assert the table did not shrink. */
size_t esps_morse_table_size(void);

/* Row i, or NULL if i is out of range. */
const esps_morse_entry_t *esps_morse_table_at(size_t i);

/* The character for a code, or '\0' when the code is not in the table (which
 * includes NULL, the empty string, and anything containing a character other
 * than '.' or '-'). */
char esps_morse_decode(const char *code);

/* The code for a character, or NULL when it has none. Case-insensitive for
 * letters, so 's' and 'S' both give "...". */
const char *esps_morse_encode(char c);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_MORSE_TABLE_H */
