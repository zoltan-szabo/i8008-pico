#ifndef I8008_DECODE_H
#define I8008_DECODE_H

#include <stdint.h>

// Indexed by the raw 3-bit captured state code (S2 on the lowest bit).
extern const char *const STATE_NAME[8];
// Indexed by the D7:D6 cycle-control code from T2.
extern const char *const CYCLE_NAME[4];

// Writes the 8008 mnemonic of opcode into out (at least 8 bytes) and returns out.
const char *i8008_mnemonic(uint8_t op, char *out);

#endif
