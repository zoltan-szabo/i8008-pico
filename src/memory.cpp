#include "main.h"

uint8_t i8008_ram[RAM_SIZE];

// Default test program: count in A, store every value to 0x0040.
// Exercises PCI (fetch), PCR (immediate bytes) and PCW (the LMA write).
static const uint8_t boot_program[] = {
	0xC0, 0xC0, 0xC0, // 0x0000  LAA x3       landing pad after RST 0
	0x2E, 0x00,       // 0x0003  LHI 0x00
	0x36, 0x40,       // 0x0005  LLI 0x40
	0x06, 0x00,       // 0x0007  LAI 0x00
	0x04, 0x01,       // 0x0009  ADI 0x01     <- loop
	0xF8,             // 0x000B  LMA          ram[HL] = A
	0x44, 0x09, 0x00, // 0x000C  JMP 0x0009
};

void i8008_ram_load() {
	memset(i8008_ram, 0xC0, RAM_SIZE); // 0xC0 = LAA, the canonical 8008 NOP
	memcpy(i8008_ram, boot_program, sizeof(boot_program));
}
