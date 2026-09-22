/* The Morse table (SPEC-DUPLEX.md "Tabla Morse").
 *
 * The dictionary below is written out again on purpose: validating the
 * component's table against itself would prove nothing, so this is an
 * independent transcription, the same trick the bench practice's Python
 * tests use.
 */
#include "esps_morse_table.h"
#include "harness.h"

#include <string.h>

static const struct {
    char c;
    const char *code;
} INDEPENDENT[] = {
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},   {'E', "."},
    {'F', "..-."},  {'G', "--."},   {'H', "...."},  {'I', ".."},    {'J', ".---"},
    {'K', "-.-"},   {'L', ".-.."},  {'M', "--"},    {'N', "-."},    {'O', "---"},
    {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
    {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},  {'Y', "-.--"},
    {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
    {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
};
#define N_INDEPENDENT (sizeof(INDEPENDENT) / sizeof(INDEPENDENT[0]))

int test_morse_table_all(void) {
    int fails = 0;

    ESPS_CHECK_EQ(&fails, esps_morse_table_size(), N_INDEPENDENT);

    for (size_t i = 0; i < N_INDEPENDENT; i++) {
        ESPS_CHECK_EQ(&fails, esps_morse_decode(INDEPENDENT[i].code), INDEPENDENT[i].c);
        const char *code = esps_morse_encode(INDEPENDENT[i].c);
        ESPS_CHECK(&fails, code != NULL && strcmp(code, INDEPENDENT[i].code) == 0);
    }

    /* Lower case encodes the same as upper: an operator typing a message
     * into a tool should not have to shout. */
    const char *s_lower = esps_morse_encode('s');
    ESPS_CHECK(&fails, s_lower != NULL && strcmp(s_lower, "...") == 0);

    /* Every row round-trips, and no two rows share a code. */
    for (size_t i = 0; i < esps_morse_table_size(); i++) {
        const esps_morse_entry_t *e = esps_morse_table_at(i);
        ESPS_CHECK(&fails, e != NULL);
        if (e == NULL) {
            continue;
        }
        ESPS_CHECK_EQ(&fails, esps_morse_decode(e->code), e->c);
        for (size_t j = i + 1; j < esps_morse_table_size(); j++) {
            const esps_morse_entry_t *o = esps_morse_table_at(j);
            ESPS_CHECK(&fails, o != NULL && strcmp(e->code, o->code) != 0);
            ESPS_CHECK(&fails, o != NULL && e->c != o->c);
        }
    }

    /* Out of range is NULL, not a wild read. */
    ESPS_CHECK(&fails, esps_morse_table_at(esps_morse_table_size()) == NULL);
    ESPS_CHECK(&fails, esps_morse_table_at((size_t)-1) == NULL);

    /* Rejections: nothing here may be answered from the table. */
    ESPS_CHECK_EQ(&fails, esps_morse_decode(NULL), '\0');
    ESPS_CHECK_EQ(&fails, esps_morse_decode(""), '\0');
    ESPS_CHECK_EQ(&fails, esps_morse_decode("......."), '\0');  /* 7 dots: no letter */
    ESPS_CHECK_EQ(&fails, esps_morse_decode(".-x"), '\0');       /* not a Morse code  */
    ESPS_CHECK_EQ(&fails, esps_morse_decode(" "), '\0');
    ESPS_CHECK(&fails, esps_morse_encode('#') == NULL);
    ESPS_CHECK(&fails, esps_morse_encode('\0') == NULL);

    return fails;
}
