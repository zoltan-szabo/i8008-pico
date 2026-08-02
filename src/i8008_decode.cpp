#include "i8008_decode.h"

#include <stdio.h>
#include <string.h>

const char *const STATE_NAME[8] = {"WAIT", "T2", "T1", "T1I", "T3", "T5", "STOP", "T4"};
const char *const CYCLE_NAME[4] = {"PCI", "PCC", "PCR", "PCW"};

static const char REG[9] = "ABCDEHLM";
static const char COND[5] = "CZSP";

const char *i8008_mnemonic(uint8_t op, char *out) {
	uint8_t mid = (op >> 3) & 7;
	uint8_t lo = op & 7;

	switch (op >> 6) {
	case 0b00:
		switch (lo) {
		case 0: // INr (00 000 00x is HLT)
			if (mid == 0)
				strcpy(out, "HLT");
			else
				sprintf(out, "IN%c", REG[mid]);
			break;
		case 1: // DCr
			if (mid == 0)
				strcpy(out, "HLT");
			else
				sprintf(out, "DC%c", REG[mid]);
			break;
		case 2: { // rotates
			static const char *const ROT[4] = {"RLC", "RRC", "RAL", "RAR"};
			strcpy(out, mid < 4 ? ROT[mid] : "?");
			break;
		}
		case 3: // RFc / RTc
			sprintf(out, "R%c%c", (mid & 4) ? 'T' : 'F', COND[mid & 3]);
			break;
		case 4: { // ALU immediate
			static const char *const ALI[8] = {"ADI", "ACI", "SUI", "SBI", "NDI", "XRI", "ORI", "CPI"};
			strcpy(out, ALI[mid]);
			break;
		}
		case 5:
			sprintf(out, "RST %d", mid);
			break;
		case 6: // LrI / LMI
			sprintf(out, "L%cI", REG[mid]);
			break;
		default:
			strcpy(out, "RET");
			break;
		}
		break;
	case 0b01:
		if (op & 1) { // INP / OUT: 01 RRM MM1, ports 0-7 are INP
			uint8_t port = (op >> 1) & 0x1F;
			sprintf(out, port < 8 ? "INP %d" : "OUT %d", port);
		} else {
			switch (lo) {
			case 0:
				sprintf(out, "J%c%c", (mid & 4) ? 'T' : 'F', COND[mid & 3]);
				break;
			case 2:
				sprintf(out, "C%c%c", (mid & 4) ? 'T' : 'F', COND[mid & 3]);
				break;
			case 4:
				strcpy(out, "JMP");
				break;
			case 6:
				strcpy(out, "CAL");
				break;
			default:
				strcpy(out, "?");
				break;
			}
		}
		break;
	case 0b10: { // ALU register/memory
		static const char *const ALR[8] = {"AD", "AC", "SU", "SB", "ND", "XR", "OR", "CP"};
		sprintf(out, "%s%c", ALR[mid], REG[lo]);
		break;
	}
	default: // Lds (11 111 111 is HLT)
		if (op == 0xFF)
			strcpy(out, "HLT");
		else
			sprintf(out, "L%c%c", REG[mid], REG[lo]);
		break;
	}
	return out;
}
