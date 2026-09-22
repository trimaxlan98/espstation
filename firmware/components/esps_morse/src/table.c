/* Linear search over 36 rows. A sorted table plus a binary search would be
 * fewer comparisons, but this runs once per decoded letter -- a handful of
 * times a second at human keying speed -- and the linear form keeps the table
 * readable in the order a person checks it against a Morse chart.
 */
#include "esps_morse_table.h"

#include <string.h>

static const esps_morse_entry_t TABLE[] = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},   {".", 'E'},
    {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},  {"..", 'I'},    {".---", 'J'},
    {"-.-", 'K'},   {".-..", 'L'},  {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},
    {".--.", 'P'},  {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},  {"-.--", 'Y'},
    {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
    {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
};

#define N_TABLE (sizeof(TABLE) / sizeof(TABLE[0]))

size_t esps_morse_table_size(void) {
    return N_TABLE;
}

const esps_morse_entry_t *esps_morse_table_at(size_t i) {
    if (i >= N_TABLE) {
        return NULL;
    }
    return &TABLE[i];
}

char esps_morse_decode(const char *code) {
    if (code == NULL || code[0] == '\0') {
        return '\0';
    }
    /* Reject anything that is not a code before searching: strcmp would say
     * "not found" anyway, but a caller passing garbage should not depend on
     * the table's contents for that answer. */
    for (const char *p = code; *p != '\0'; p++) {
        if (*p != '.' && *p != '-') {
            return '\0';
        }
    }
    for (size_t i = 0; i < N_TABLE; i++) {
        if (strcmp(TABLE[i].code, code) == 0) {
            return TABLE[i].c;
        }
    }
    return '\0';
}

const char *esps_morse_encode(char c) {
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    for (size_t i = 0; i < N_TABLE; i++) {
        if (TABLE[i].c == c) {
            return TABLE[i].code;
        }
    }
    return NULL;
}
