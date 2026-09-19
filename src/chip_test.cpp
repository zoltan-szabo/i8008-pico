// Chip test: functional and timing test of the 8008 in the socket.
//
// Every test is a small 8008 program generated into the emulated RAM. The
// chip runs it at full speed and reports results with OUT, which the bus
// engine logs; this side compares the log with a reference model. Flags
// are made visible by the FLAGOUT subroutine, a tree of conditional jumps
// ending in OUT 8..23: the port number encodes C Z S P, the data is A.
// The bus engine also measures every instruction's length in T-states,
// checked against the datasheet at the end. See docs/chip-testing.md.

#include "main.h"

#include <stdarg.h>

#include "hardware/clocks.h"

#define PROG_BASE 0x0C00    // test programs; clear of every walking-bit address
#define FLAGOUT_ADDR 0x0B00 // flag reporting subroutine
#define SCRATCH 0x3F00      // operand bytes for the M forms
#define PORT_FLAGS 8        // 8..23: OUT A, port - 8 = C Z S P
#define PORT_REPORT 24      // plain value report
#define PORT_DONE 31
#define DONE_VALUE 0xDD     // OUT 31 with this value ends a test program
#define IDLE_MS 200         // no result for this long: the program is lost

#define EXP_MAX 2048

enum { rA, rB, rC, rD, rE, rH, rL, rM };
enum { opAD, opAC, opSU, opSB, opND, opXR, opOR, opCP };
enum { cndC, cndZ, cndS, cndP };

struct Flags {
	bool c, z, s, p;
};
struct Rec {
	uint8_t port, value;
};
struct Ctx { // what an expected result is about, for failure messages
	uint8_t op, a, b, c;
};

static const char *const ALU_NAME[8] = {"ADD", "ADC", "SUB", "SBB", "AND", "XOR", "OR", "CMP"};

// ---------------------------------------------------------------------------
// Program image and emitter
// ---------------------------------------------------------------------------

static uint8_t img[RAM_SIZE];
static uint16_t pc;

static void e_byte(uint8_t v) { img[pc++ & RAM_MASK] = v; }

static uint16_t e_addr(uint16_t a) { // returns where the address went, for patching
	uint16_t at = pc;
	e_byte(a & 0xFF);
	e_byte((a >> 8) & 0x3F);
	return at;
}

static void patch(uint16_t at, uint16_t target) {
	img[at] = target & 0xFF;
	img[at + 1] = (target >> 8) & 0x3F;
}

static void e_lrr(uint8_t d, uint8_t s) { e_byte(0xC0 | d << 3 | s); }
static void e_lri(uint8_t r, uint8_t v) { e_byte(0x06 | r << 3); e_byte(v); }
static void e_alu_r(uint8_t op, uint8_t s) { e_byte(0x80 | op << 3 | s); }
static void e_alu_i(uint8_t op, uint8_t v) { e_byte(0x04 | op << 3); e_byte(v); }
static void e_inr(uint8_t r) { e_byte(r << 3); }
static void e_dcr(uint8_t r) { e_byte(r << 3 | 1); }
static void e_rot(uint8_t n) { e_byte(0x02 | n << 3); }
static uint16_t e_jmp(uint16_t a) { e_byte(0x44); return e_addr(a); }
static uint16_t e_jc(bool t, uint8_t c, uint16_t a) { e_byte(0x40 | t << 5 | c << 3); return e_addr(a); }
static void e_cal(uint16_t a) { e_byte(0x46); e_addr(a); }
static void e_cc(bool t, uint8_t c, uint16_t a) { e_byte(0x42 | t << 5 | c << 3); e_addr(a); }
static void e_ret() { e_byte(0x07); }
static void e_rc(bool t, uint8_t c) { e_byte(0x03 | t << 5 | c << 3); }
static void e_io(uint8_t port) { e_byte(0x41 | port << 1); } // INP 0-7, OUT 8-31
static void e_set_hl(uint16_t a) { e_lri(rH, a >> 8); e_lri(rL, a & 0xFF); }

static void e_set_carry(bool c) {
	if (c) {
		e_lri(rA, 0xFF);
		e_alu_i(opAD, 0x01); // A = 0, C = 1
	} else {
		e_lri(rA, 0x00);
		e_alu_r(opND, rA); // A = 0, C = 0
	}
}

// Leaves the one flag in the wanted state (the others follow from A).
static void e_set_flag(uint8_t cond, bool v) {
	switch (cond) {
	case cndC: e_set_carry(v); return;
	case cndZ: e_lri(rA, v ? 0x00 : 0x01); break;
	case cndS: e_lri(rA, v ? 0x80 : 0x01); break;
	default: e_lri(rA, v ? 0x03 : 0x01); break; // P: even parity sets it
	}
	e_alu_r(opOR, rA);
}

static void e_flagout() { e_cal(FLAGOUT_ADDR); }

static void e_done() {
	e_lri(rA, DONE_VALUE);
	e_io(PORT_DONE);
	e_byte(0x00); // HLT
}

// FLAGOUT: test C, Z, S, P in turn; each leaf outputs A to 8 + CZSP.
static void gen_flagout(int level, uint8_t bits) {
	static const uint8_t order[4] = {cndC, cndZ, cndS, cndP};
	if (level == 4) {
		e_io(PORT_FLAGS + bits);
		e_ret();
		return;
	}
	uint16_t at = e_jc(true, order[level], 0);
	gen_flagout(level + 1, bits);
	patch(at, pc);
	gen_flagout(level + 1, bits | (8 >> level));
}

// ---------------------------------------------------------------------------
// Reference model
// ---------------------------------------------------------------------------

static Flags zsp(uint8_t v, bool c) {
	return {c, v == 0, (v & 0x80) != 0, !(__builtin_popcount(v) & 1)};
}

static uint8_t alu(uint8_t op, uint8_t a, uint8_t b, bool cin, Flags *f) {
	unsigned r;
	bool c;
	switch (op) {
	case opAD: r = a + b; c = r > 0xFF; break;
	case opAC: r = a + b + cin; c = r > 0xFF; break;
	case opSU:
	case opCP: r = a - b; c = a < b; break;
	case opSB: r = a - b - cin; c = a < b + cin; break;
	case opND: r = a & b; c = false; break;
	case opXR: r = a ^ b; c = false; break;
	default: r = a | b; c = false; break;
	}
	*f = zsp(r & 0xFF, c);
	return op == opCP ? a : r & 0xFF;
}

static uint8_t flag_bits(Flags f) { return f.c << 3 | f.z << 2 | f.s << 1 | f.p; }

// ---------------------------------------------------------------------------
// Expectations and outcome
// ---------------------------------------------------------------------------

static Rec exp_rec[EXP_MAX];
static Ctx exp_ctx[EXP_MAX];
static uint16_t n_exp;
static Ctx ctx;
static Rec got[EXP_MAX];
static uint16_t n_got;
static uint32_t n_extra;
static struct {
	uint16_t addr;
	uint8_t value;
} exp_wr[64];
static uint8_t n_wr;

static uint32_t checks, fails;
static char msgs[3][112];
static uint8_t n_msgs;

static void set_ctx(uint8_t op, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0) { ctx = {op, a, b, c}; }

static void expect(uint8_t port, uint8_t value) {
	if (n_exp < EXP_MAX) {
		exp_ctx[n_exp] = ctx;
		exp_rec[n_exp++] = {port, value};
	}
}

static void expect_flags(uint8_t a, Flags f) { expect(PORT_FLAGS + flag_bits(f), a); }

static void expect_write(uint16_t addr, uint8_t v) { exp_wr[n_wr++] = {addr, v}; }

static void fail(const char *fmt, ...) {
	fails++;
	if (n_msgs < 3) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(msgs[n_msgs++], sizeof msgs[0], fmt, ap);
		va_end(ap);
	}
}

static void rec_str(char *out, size_t n, Rec r) {
	if (r.port >= PORT_FLAGS && r.port < PORT_FLAGS + 16) {
		uint8_t f = r.port - PORT_FLAGS;
		snprintf(out, n, "A=%02X C%uZ%uS%uP%u", r.value, f >> 3 & 1, f >> 2 & 1, f >> 1 & 1, f & 1);
	} else {
		snprintf(out, n, "OUT %u=%02X", r.port, r.value);
	}
}

static void ctx_str(char *out, size_t n, Ctx c) {
	char mn[8];
	snprintf(out, n, "%s (%02X) a=%02X b=%02X c=%u", i8008_mnemonic(c.op, mn), c.op, c.a, c.b, c.c);
}

// ---------------------------------------------------------------------------
// Running programs on the chip
// ---------------------------------------------------------------------------

// Stopped = no instruction fetch for 3 ms (a running chip fetches every
// ~50 us).
static bool wait_stopped(uint32_t timeout_ms) {
	uint32_t t0 = millis(), since = t0, last = pci_count;
	while (millis() - t0 < timeout_ms) {
		if (pci_count != last) {
			last = pci_count;
			since = millis();
		} else if (millis() - since >= 3) {
			return true;
		}
	}
	return false;
}

static void stop_chip() {
	pio_sm_set_pins_with_mask(pio0, sm_step, 1u << PIN_READY, 1u << PIN_READY);
	memset(i8008_ram, 0x00, RAM_SIZE); // HLT everywhere: whatever runs, stops
	wait_stopped(100);
}

// Jam a JMP into the next fetch; the INT wakes a halted chip for it.
static void start_at(uint16_t addr) {
	uint8_t jmp[3] = {0x44, (uint8_t)(addr & 0xFF), (uint8_t)(addr >> 8)};
	arm_jam(jmp, 3);
	int_pulse();
}

static void resume_with(uint8_t op) {
	arm_jam(&op, 1);
	int_pulse();
}

static bool log_pop(Rec *r) {
	uint32_t t = io_log_tail;
	if (t == io_log_head)
		return false;
	uint16_t e = io_log[t];
	io_log_tail = (t + 1) & (IO_LOG_SIZE - 1);
	*r = {(uint8_t)(e >> 8), (uint8_t)e};
	return true;
}

enum { COLLECT_TIMEOUT, COLLECT_COUNT, COLLECT_DONE };

// Gather results until n of them are in, the done marker arrives, or
// nothing has come for IDLE_MS.
static int collect(uint16_t n) {
	uint32_t last = millis();
	while (millis() - last < IDLE_MS) {
		Rec r;
		while (log_pop(&r)) {
			last = millis();
			if (r.port == PORT_DONE && r.value == DONE_VALUE)
				return COLLECT_DONE;
			if (n_got < EXP_MAX)
				got[n_got++] = r;
			else
				n_extra++;
			if (n_got >= n)
				return COLLECT_COUNT;
		}
	}
	return COLLECT_TIMEOUT;
}

static void load_and_start() {
	stop_chip();
	memcpy(i8008_ram, img, RAM_SIZE);
	io_log_tail = io_log_head;
	n_got = 0;
	n_extra = 0;
	start_at(PROG_BASE);
}

static bool run_program() {
	load_and_start();
	return collect(0xFFFF) == COLLECT_DONE;
}

static void begin() {
	memset(img, 0x00, RAM_SIZE); // HLT: a lost program stops
	pc = FLAGOUT_ADDR;
	gen_flagout(0, 0);
	pc = PROG_BASE;
	n_exp = 0;
	n_wr = 0;
	set_ctx(0);
}

static void compare() {
	char e[24], g[24], c[48];
	checks += n_exp;
	uint16_t n = n_got < n_exp ? n_got : n_exp;
	for (uint16_t i = 0; i < n; i++) {
		if (got[i].port == exp_rec[i].port && got[i].value == exp_rec[i].value)
			continue;
		rec_str(e, sizeof e, exp_rec[i]);
		rec_str(g, sizeof g, got[i]);
		ctx_str(c, sizeof c, exp_ctx[i]);
		fail("%s: expected %s, got %s", c, e, g);
	}
	if (n_got < n_exp) {
		ctx_str(c, sizeof c, exp_ctx[n_got]);
		fail("only %u of %u results; next was %s", n_got, n_exp, c);
	} else if (n_got > n_exp || n_extra) {
		fail("%lu results more than expected", (unsigned long)(n_got - n_exp + n_extra));
	}
}

// Everything in RAM must be as loaded, plus exactly the expected writes.
static void check_ram() {
	for (uint8_t i = 0; i < n_wr; i++)
		img[exp_wr[i].addr] = exp_wr[i].value;
	checks += n_wr;
	for (uint32_t a = 0; a < RAM_SIZE; a++)
		if (i8008_ram[a] != img[a])
			fail("ram[%04lX] = %02X, expected %02X", (unsigned long)a, i8008_ram[a], img[a]);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Walking bits through A: every data line, into the chip and out again.
static void build_bus() {
	static const uint8_t pat[] = {0x00, 0xFF, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
	                              0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F, 0x55, 0xAA};
	for (uint8_t v : pat) {
		set_ctx(0x06, v);
		e_lri(rA, v);
		e_io(PORT_REPORT);
		expect(PORT_REPORT, v);
	}
}

// All 49 register-to-register moves; every register is reported after
// each, so a move that disturbs a bystander shows up too.
static void build_regs() {
	for (uint8_t d = rA; d <= rL; d++) {
		for (uint8_t s = rA; s <= rL; s++) {
			uint8_t val[7];
			for (uint8_t r = rA; r <= rL; r++) {
				val[r] = (uint8_t)((d * 7 + s) * 0x25 + r * 0x4B + 0x13);
				e_lri(r, val[r]);
			}
			e_lrr(d, s);
			val[d] = val[s];
			set_ctx(0xC0 | d << 3 | s, val[s]);
			e_io(PORT_REPORT);
			expect(PORT_REPORT, val[rA]);
			for (uint8_t r = rB; r <= rL; r++) {
				e_lrr(rA, r);
				e_io(PORT_REPORT);
				expect(PORT_REPORT, val[r]);
			}
		}
	}
}

// Walking-bit addresses over all 14 address bits, every LMr and LrM.
static void build_mem() {
	uint16_t addr[28];
	uint8_t val[28];
	for (uint8_t k = 0; k < 14; k++) {
		addr[k] = 1u << k;
		addr[14 + k] = 0x3FFF ^ (1u << k);
	}
	for (uint8_t i = 0; i < 28; i++) {
		val[i] = (uint8_t)(i * 0x3B) ^ 0xA5;
		set_ctx(0x3E, val[i]);
		e_set_hl(addr[i]);
		e_lri(rM, val[i]); // LMI
		expect_write(addr[i], val[i]);
	}
	for (uint8_t i = 0; i < 28; i++) {
		set_ctx(0xC7, val[i]);
		e_set_hl(addr[i]);
		e_lrr(rA, rM); // LAM
		e_io(PORT_REPORT);
		expect(PORT_REPORT, val[i]);
	}
	// LMr from A..E, then LMH and LML, which store the address registers
	for (uint8_t r = rA; r <= rE; r++) {
		uint8_t v = 0x61 + r * 0x22;
		set_ctx(0xF8 | r, v);
		e_set_hl(SCRATCH + 0x10 + r);
		e_lri(r, v);
		e_lrr(rM, r);
		expect_write(SCRATCH + 0x10 + r, v);
	}
	set_ctx(0xFD);
	e_set_hl(SCRATCH + 0x20);
	e_lrr(rM, rH);
	expect_write(SCRATCH + 0x20, SCRATCH >> 8);
	set_ctx(0xFE);
	e_set_hl(SCRATCH + 0x21);
	e_lrr(rM, rL);
	expect_write(SCRATCH + 0x21, 0x21);
	// LrM into B..E, then LHM and LLM, which load the address registers
	for (uint8_t r = rB; r <= rE; r++) {
		set_ctx(0xC7 | r << 3, val[r]);
		e_set_hl(addr[r]);
		e_lrr(r, rM);
		e_lrr(rA, r);
		e_io(PORT_REPORT);
		expect(PORT_REPORT, val[r]);
	}
	for (uint8_t r = rH; r <= rL; r++) {
		uint16_t a = SCRATCH + 0x30 + r;
		uint8_t v = 0x12 + r;
		set_ctx(0xC7 | r << 3, v);
		e_set_hl(a);
		e_lri(rM, v);
		expect_write(a, v);
		e_lrr(r, rM);
		e_lrr(rA, r);
		e_io(PORT_REPORT);
		expect(PORT_REPORT, v);
	}
}

// INr/DCr on B..L across the wrap and sign edges; carry must survive.
static void build_incdec() {
	static const uint8_t vals[] = {0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0xFE, 0xFF};
	for (uint8_t r = rB; r <= rL; r++) {
		for (uint8_t inc = 0; inc < 2; inc++) {
			for (uint8_t v : vals) {
				for (uint8_t cin = 0; cin < 2; cin++) {
					uint8_t op = r << 3 | !inc;
					uint8_t res = inc ? v + 1 : v - 1;
					set_ctx(op, v, 0, cin);
					e_set_carry(cin);
					e_lri(r, v);
					e_byte(op);
					e_lrr(rA, r);
					e_flagout();
					expect_flags(res, zsp(res, cin));
				}
			}
		}
	}
}

// One ALU operation in all nine forms (A..L, M, immediate), edge operands,
// both carry inputs.
static uint8_t alu_op_under_test;

static void build_alu() {
	static const uint8_t vals[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0x5A};
	uint8_t op = alu_op_under_test;
	uint8_t last_m = 0;
	bool m_used = false;
	for (uint8_t src = rA; src <= rM + 1; src++) { // rM + 1 = immediate
		for (uint8_t a : vals) {
			for (uint8_t b : vals) {
				if (src == rA && b != a)
					continue;
				for (uint8_t cin = 0; cin < 2; cin++) {
					uint8_t code = src > rM ? 0x04 | op << 3 : 0x80 | op << 3 | src;
					set_ctx(code, a, b, cin);
					e_set_carry(cin);
					if (src == rM) {
						e_set_hl(SCRATCH);
						e_lri(rM, b);
						last_m = b;
						m_used = true;
					} else if (src != rA && src <= rL) {
						e_lri(src, b);
					}
					e_lri(rA, a);
					if (src > rM)
						e_alu_i(op, b);
					else
						e_alu_r(op, src);
					e_flagout();
					Flags f;
					uint8_t res = alu(op, a, b, cin, &f);
					expect_flags(res, f);
				}
			}
		}
	}
	if (m_used)
		expect_write(SCRATCH, last_m);
}

// Rotates change only C: Z S P keep what the carry setup left (1 0 1).
static void build_rot() {
	static const uint8_t vals[] = {0x00, 0x01, 0x80, 0xFF, 0x55, 0xAA, 0x7F, 0xFE};
	for (uint8_t n = 0; n < 4; n++) {
		for (uint8_t v : vals) {
			for (uint8_t cin = 0; cin < 2; cin++) {
				uint8_t r;
				bool c;
				switch (n) {
				case 0: r = v << 1 | v >> 7; c = v >> 7; break;         // RLC
				case 1: r = v >> 1 | v << 7; c = v & 1; break;          // RRC
				case 2: r = v << 1 | cin; c = v >> 7; break;            // RAL
				default: r = v >> 1 | cin << 7; c = v & 1; break;       // RAR
				}
				set_ctx(0x02 | n << 3, v, 0, cin);
				e_set_carry(cin);
				e_lri(rA, v);
				e_rot(n);
				e_flagout();
				expect_flags(r, {c, true, false, true});
			}
		}
	}
}

// All 8 JMP encodings, then JFc/JTc on each flag, taken and not taken.
static void build_jump() {
	for (uint8_t x = 0; x < 8; x++) {
		set_ctx(0x44 | x << 3);
		e_byte(0x44 | x << 3);
		uint16_t at = e_addr(0);
		e_lri(rA, 0xBD); // must be skipped
		e_io(PORT_REPORT);
		patch(at, pc);
		e_lri(rA, 0xC0 | x);
		e_io(PORT_REPORT);
		expect(PORT_REPORT, 0xC0 | x);
	}
	for (uint8_t c = 0; c < 4; c++) {
		for (uint8_t t = 0; t < 2; t++) {
			for (uint8_t fv = 0; fv < 2; fv++) {
				uint8_t idx = c << 2 | t << 1 | fv;
				set_ctx(0x40 | t << 5 | c << 3, fv);
				e_set_flag(c, fv);
				uint16_t taken_at = e_jc(t, c, 0);
				e_lri(rA, 0x40 | idx);
				e_io(PORT_REPORT);
				uint16_t skip_at = e_jmp(0);
				patch(taken_at, pc);
				e_lri(rA, 0x80 | idx);
				e_io(PORT_REPORT);
				patch(skip_at, pc);
				expect(PORT_REPORT, (fv == t ? 0x80 : 0x40) | idx);
			}
		}
	}
}

// All 8 CAL and RET encodings, CFc/CTc and RFc/RTc both ways.
static void build_call() {
	uint16_t skip_at = e_jmp(0);
	uint16_t sub[8];
	for (uint8_t x = 0; x < 8; x++) {
		sub[x] = pc;
		e_lri(rA, 0xA0 | x);
		e_io(PORT_REPORT);
		e_byte(0x07 | x << 3); // RET encoding x
	}
	uint16_t csub = pc;
	e_lri(rA, 0x5C);
	e_io(PORT_REPORT);
	e_ret();
	patch(skip_at, pc);

	for (uint8_t x = 0; x < 8; x++) {
		set_ctx(0x46 | x << 3, x);
		e_byte(0x46 | x << 3);
		e_addr(sub[x]);
		e_lri(rA, 0xD0 | x);
		e_io(PORT_REPORT);
		expect(PORT_REPORT, 0xA0 | x);
		expect(PORT_REPORT, 0xD0 | x);
	}
	for (uint8_t c = 0; c < 4; c++) {
		for (uint8_t t = 0; t < 2; t++) {
			for (uint8_t fv = 0; fv < 2; fv++) {
				uint8_t idx = c << 2 | t << 1 | fv;
				set_ctx(0x42 | t << 5 | c << 3, fv);
				e_set_flag(c, fv);
				e_cc(t, c, csub);
				e_lri(rA, 0x60 | idx);
				e_io(PORT_REPORT);
				if (fv == t)
					expect(PORT_REPORT, 0x5C);
				expect(PORT_REPORT, 0x60 | idx);
			}
		}
	}
	for (uint8_t c = 0; c < 4; c++) {
		for (uint8_t t = 0; t < 2; t++) {
			for (uint8_t fv = 0; fv < 2; fv++) {
				uint8_t idx = c << 2 | t << 1 | fv;
				set_ctx(0x03 | t << 5 | c << 3, fv);
				uint16_t over = e_jmp(0);
				uint16_t s = pc;
				e_set_flag(c, fv);
				e_rc(t, c);
				e_lri(rA, 0x20 | idx);
				e_io(PORT_REPORT);
				e_ret();
				patch(over, pc);
				e_cal(s);
				e_lri(rA, 0x30 | idx);
				e_io(PORT_REPORT);
				if (fv != t)
					expect(PORT_REPORT, 0x20 | idx);
				expect(PORT_REPORT, 0x30 | idx);
			}
		}
	}
}

// RST 0-7 into vectors that report and return.
static void build_rst() {
	uint16_t save = pc;
	for (uint8_t n = 0; n < 8; n++) {
		pc = n * 8;
		e_lri(rA, 0x90 | n);
		e_io(PORT_REPORT);
		e_ret();
	}
	pc = save;
	for (uint8_t n = 0; n < 8; n++) {
		set_ctx(0x05 | n << 3);
		e_byte(0x05 | n << 3);
		e_lri(rA, 0xB0 | n);
		e_io(PORT_REPORT);
		expect(PORT_REPORT, 0x90 | n);
		expect(PORT_REPORT, 0xB0 | n);
	}
}

// The stack is 8 registers, one of them the PC: 7 nested calls return
// cleanly. An 8th overwrites the oldest entry, so the last RET lands on
// the address after the innermost routine's own RET.
static void build_stack() {
	uint16_t over = e_jmp(0);
	uint16_t a[9], b[9];
	for (uint8_t k = 7; k >= 1; k--) {
		a[k] = pc;
		e_lri(rA, 0xB0 | k);
		e_io(PORT_REPORT);
		if (k < 7)
			e_cal(a[k + 1]);
		e_lri(rA, 0xE0 | k);
		e_io(PORT_REPORT);
		e_ret();
	}
	uint16_t to_end = 0;
	for (uint8_t k = 8; k >= 1; k--) {
		b[k] = pc;
		e_lri(rA, 0xC0 | k);
		e_io(PORT_REPORT);
		if (k < 8) {
			e_cal(b[k + 1]);
			e_lri(rA, 0xD0 | k);
			e_io(PORT_REPORT);
			e_ret();
		} else {
			e_ret();
			e_lri(rA, 0xEE); // where the wrapped-around stack returns
			e_io(PORT_REPORT);
			to_end = e_jmp(0);
		}
	}
	patch(over, pc);

	set_ctx(0x46, 7);
	e_cal(a[1]);
	e_lri(rA, 0xE0);
	e_io(PORT_REPORT);
	for (uint8_t k = 1; k <= 7; k++)
		expect(PORT_REPORT, 0xB0 | k);
	for (uint8_t k = 7; k >= 1; k--)
		expect(PORT_REPORT, 0xE0 | k);
	expect(PORT_REPORT, 0xE0);

	set_ctx(0x46, 8);
	e_cal(b[1]);
	e_lri(rA, 0xF0); // must not run: the return address was overwritten
	e_io(PORT_REPORT);
	patch(to_end, pc);
	for (uint8_t k = 1; k <= 8; k++)
		expect(PORT_REPORT, 0xC0 | k);
	for (uint8_t k = 7; k >= 1; k--)
		expect(PORT_REPORT, 0xD0 | k);
	expect(PORT_REPORT, 0xEE);
}

// Every input and output port.
static void build_io() {
	for (uint8_t p = 0; p < IO_IN_PORTS; p++) {
		io_in[p] = 0x3C ^ (p * 0x11);
		set_ctx(0x41 | p << 1, io_in[p]);
		e_io(p);
		e_io(PORT_REPORT);
		expect(PORT_REPORT, io_in[p]);
	}
	for (uint8_t p = IO_IN_PORTS; p < IO_PORTS; p++) {
		uint8_t v = (uint8_t)(p * 0x1D) ^ 0x5A;
		if (p == PORT_DONE && v == DONE_VALUE)
			v ^= 1;
		set_ctx(0x41 | p << 1, v);
		e_lri(rA, v);
		e_io(p);
		expect(p, v);
	}
}

// HLT in all three encodings; an interrupt resumes after it, with a jammed
// NOP or a jammed RST 1 whose return must come back to the same place.
static void build_hlt() {
	uint16_t save = pc;
	pc = 0x08;
	e_lri(rA, 0x7A);
	e_io(PORT_REPORT);
	e_ret();
	pc = save;
	static const uint8_t hlt[3] = {0x00, 0x01, 0xFF};
	for (uint8_t i = 0; i < 3; i++) {
		set_ctx(hlt[i]);
		e_lri(rA, 0x71 + i);
		e_io(PORT_REPORT);
		expect(PORT_REPORT, 0x71 + i);
		e_byte(hlt[i]);
		if (i == 1)
			expect(PORT_REPORT, 0x7A);
	}
	set_ctx(0xFF);
	e_lri(rA, 0x74);
	e_io(PORT_REPORT);
	expect(PORT_REPORT, 0x74);
}

static bool run_hlt() {
	static const uint8_t resume[3] = {0xC0, 0x0D, 0xC0}; // LAA, RST 1, LAA
	static const uint8_t hlt[3] = {0x00, 0x01, 0xFF};
	static const uint16_t after[3] = {1, 2, 4};           // results before each resume
	load_and_start();
	for (uint8_t i = 0; i < 3; i++) {
		if (collect(after[i]) != COLLECT_COUNT)
			return false;
		if (!wait_stopped(50)) {
			fail("HLT %02X did not stop the chip", hlt[i]);
			return false;
		}
		Rec r;
		while (log_pop(&r)) // anything here ran past the HLT
			got[n_got < EXP_MAX ? n_got++ : n_got] = r;
		resume_with(resume[i]);
	}
	return collect(0xFFFF) == COLLECT_DONE;
}

struct Test {
	const char *id;
	const char *name;
	void (*build)();
	bool (*run)();
	uint8_t alu_op;
};

static const Test TESTS[] = {
	{"bus", "data bus walking bits", build_bus, run_program, 0},
	{"regs", "register moves", build_regs, run_program, 0},
	{"mem", "memory, address lines", build_mem, run_program, 0},
	{"incdec", "increment, decrement", build_incdec, run_program, 0},
	{"add", "ADD all forms", build_alu, run_program, opAD},
	{"adc", "ADC all forms", build_alu, run_program, opAC},
	{"sub", "SUB all forms", build_alu, run_program, opSU},
	{"sbb", "SBB all forms", build_alu, run_program, opSB},
	{"and", "AND all forms", build_alu, run_program, opND},
	{"xor", "XOR all forms", build_alu, run_program, opXR},
	{"or", "OR all forms", build_alu, run_program, opOR},
	{"cmp", "CMP all forms", build_alu, run_program, opCP},
	{"rot", "rotates", build_rot, run_program, 0},
	{"jump", "jumps", build_jump, run_program, 0},
	{"call", "calls and returns", build_call, run_program, 0},
	{"rst", "RST 0-7", build_rst, run_program, 0},
	{"stack", "stack depth and wrap", build_stack, run_program, 0},
	{"io", "input and output ports", build_io, run_program, 0},
	{"hlt", "HLT and interrupt resume", build_hlt, run_hlt, 0},
};
#define N_TESTS (sizeof TESTS / sizeof TESTS[0])

static void reset_outcome() {
	checks = 0;
	fails = 0;
	n_msgs = 0;
}

static void print_outcome(const char *id, const char *name) {
	Serial.printf("  %-7s %-26s %s  %lu checks", id, name, fails ? "FAIL" : "pass", (unsigned long)checks);
	if (fails)
		Serial.printf(", %lu failed", (unsigned long)fails);
	Serial.println();
	for (uint8_t i = 0; i < n_msgs; i++)
		Serial.printf("            %s\n", msgs[i]);
}

static bool run_test(const Test &t, bool verbose) {
	reset_outcome();
	begin();
	alu_op_under_test = t.alu_op;
	t.build();
	e_done();
	if (!t.run())
		fail("program did not finish (%u results)", n_got);
	compare();
	check_ram();
	if (verbose)
		print_outcome(t.id, t.name);
	return fails == 0;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

// Datasheet instruction lengths in T-states; conditionals as not taken /
// taken. False for HLT (its length is however long it stays halted) and
// the four undefined rotate codes.
static bool expected_states(uint8_t op, uint8_t *nt, uint8_t *tk) {
	uint8_t mid = (op >> 3) & 7, lo = op & 7;
	*nt = *tk = 5;
	switch (op >> 6) {
	case 0:
		switch (lo) {
		case 0:
		case 1: return mid != 0 && mid != rM; // INr/DCr; 00/01 are HLT, no INM/DCM
		case 2: return mid < 4;  // rotates
		case 3: *nt = 3; return true; // RFc/RTc
		case 4: *nt = *tk = 8; return true; // ALU immediate
		case 5: return true;                // RST
		case 6: *nt = *tk = mid == rM ? 9 : 8; return true; // LrI / LMI
		default: return true;               // RET
		}
	case 1:
		if (op & 1) {
			*nt = *tk = ((op >> 1) & 0x1F) < IO_IN_PORTS ? 8 : 6; // INP / OUT
			return true;
		}
		*nt = lo == 0 || lo == 2 ? 9 : 11; // Jc/Cc not taken, JMP/CAL
		*tk = 11;
		return true;
	case 2:
		if (lo == rM)
			*nt = *tk = 8;
		return true;
	default:
		if (op == 0xFF)
			return false;
		if (mid == rM)
			*nt = *tk = 7; // LMr
		else if (lo == rM)
			*nt = *tk = 8; // LrM
		return true;
	}
}

static void timing_reset() {
	for (int i = 0; i < 256; i++) {
		op_states_min[i] = 0xFF;
		op_states_max[i] = 0;
	}
	op_timing_reset = true;
}

static bool check_timing(bool verbose) {
	char mn[8];
	reset_outcome();
	for (int op = 0; op < 256; op++) {
		uint8_t nt, tk;
		if (!expected_states(op, &nt, &tk))
			continue;
		checks++;
		uint8_t lo = op_states_min[op], hi = op_states_max[op];
		if (hi == 0)
			fail("%s (%02X) never executed", i8008_mnemonic(op, mn), op);
		else if (lo != nt || hi != tk)
			fail("%s (%02X): %u..%u states, expected %u..%u", i8008_mnemonic(op, mn), op, lo, hi, nt, tk);
	}
	if (verbose)
		print_outcome("timing", "T-states per opcode");
	return fails == 0;
}

// ---------------------------------------------------------------------------
// Suites
// ---------------------------------------------------------------------------

static char failed_ids[160];

static void note_failed(const char *id) {
	size_t n = strlen(failed_ids);
	snprintf(failed_ids + n, sizeof failed_ids - n, "%s%s", n ? "," : "", id);
}

// Warm the chip up: it comes out of a clock stop confused (see README).
static bool boot_chip(uint8_t *tries) {
	for (*tries = 1; *tries <= 10; (*tries)++) {
		begin();
		e_done();
		if (run_program())
			return true;
	}
	return false;
}

static bool quick_suite(bool verbose, const char **first_fail) {
	bool pass = true;
	*first_fail = nullptr;
	timing_reset();
	for (size_t i = 0; i < N_TESTS; i++) {
		if (!run_test(TESTS[i], verbose)) {
			pass = false;
			if (!*first_fail)
				*first_fail = TESTS[i].id;
			if (verbose)
				note_failed(TESTS[i].id);
		}
	}
	stop_chip();
	if (!check_timing(verbose)) {
		pass = false;
		if (!*first_fail)
			*first_fail = "timing";
		if (verbose)
			note_failed("timing");
	}
	return pass;
}

static bool esc_pressed() {
	while (Serial.available())
		if (Serial.read() == 0x1B)
			return true;
	return false;
}

// One ALU operation over all 65536 operand pairs, looped by the chip itself:
// B = a (outer), C = b (inner), op C, FLAGOUT.
static bool exhaustive(uint8_t op, bool cin, bool *aborted) {
	char id[12], name[40];
	snprintf(id, sizeof id, "x-%s", TESTS[4 + op].id);
	snprintf(name, sizeof name, "%s r, all operands, CY=%u", ALU_NAME[op], cin);
	reset_outcome();
	begin();
	e_lri(rB, 0);
	uint16_t outer = pc;
	e_lri(rC, 0);
	uint16_t inner = pc;
	if (cin) {
		e_set_carry(true);
		e_lrr(rA, rB);
	} else {
		e_lrr(rA, rB);
		e_alu_r(opND, rA);
	}
	e_alu_r(op, rC);
	e_flagout();
	e_inr(rC);
	e_jc(false, cndZ, inner);
	e_inr(rB);
	e_jc(false, cndZ, outer);
	e_done();
	load_and_start();

	uint32_t k = 0, t0 = millis(), last = t0;
	bool finished = false;
	while (millis() - last < IDLE_MS && !finished) {
		Rec r;
		while (log_pop(&r)) {
			last = millis();
			if (r.port == PORT_DONE && r.value == DONE_VALUE) {
				finished = true;
				break;
			}
			uint8_t a = k >> 8, b = k & 0xFF;
			Flags f;
			Rec e = {0, alu(op, a, b, cin, &f)};
			e.port = PORT_FLAGS + flag_bits(f);
			if (r.port != e.port || r.value != e.value) {
				char es[24], gs[24];
				rec_str(es, sizeof es, e);
				rec_str(gs, sizeof gs, r);
				fail("a=%02X b=%02X c=%u: expected %s, got %s", a, b, cin, es, gs);
			}
			k++;
		}
		if (esc_pressed()) {
			*aborted = true;
			break;
		}
	}
	checks = k;
	if (!*aborted && k != 65536)
		fail("%lu of 65536 results", (unsigned long)k);
	check_ram();
	Serial.printf("  %-7s %-26s %s  %lu checks", id, name, fails ? "FAIL" : "pass", (unsigned long)checks);
	if (fails)
		Serial.printf(", %lu failed", (unsigned long)fails);
	Serial.printf("  (%lu s)\n", (unsigned long)(millis() - t0) / 1000);
	for (uint8_t i = 0; i < n_msgs; i++)
		Serial.printf("            %s\n", msgs[i]);
	if (fails)
		note_failed(id);
	return fails == 0;
}

static uint32_t khz(uint div) { return (clock_get_hz(clk_sys) / 4 / div + 500) / 1000; }

// Rerun the quick suite at rising clock rates until it fails.
static uint32_t clock_sweep() {
	// 133 MHz / 4 / div: 520, 536, 554, 573, 605, 639, 679, 723, 773, 811,
	// 853, 875 kHz. The top stays below where bus_write's fixed hold after
	// T3 (about 1 us) would reach into the next state.
	static const uint8_t divs[] = {64, 62, 60, 58, 55, 52, 49, 46, 43, 41, 39, 38};
	uint32_t best = khz(CLK_DIV);
	Serial.println("clock sweep (quick suite at each rate):");
	for (uint8_t d : divs) {
		set_clock_div(d);
		uint8_t tries;
		const char *why = "no response";
		bool pass = boot_chip(&tries) && quick_suite(false, &why);
		Serial.printf("  %4lu kHz  %s", (unsigned long)khz(d), pass ? "pass" : "FAIL");
		if (!pass)
			Serial.printf("  (%s)", why);
		Serial.println();
		if (!pass)
			break;
		best = khz(d);
		if (esc_pressed())
			break;
	}
	set_clock_div(CLK_DIV);
	return best;
}

// Reads a label line; false on Esc.
static bool read_label(char *buf, size_t n) {
	size_t len = 0;
	Serial.print("chip label (Enter for none, Esc cancels): ");
	while (true) {
		while (!Serial.available())
			;
		char ch = Serial.read();
		if (ch == '\r' || ch == '\n')
			break;
		if (ch == 0x1B) {
			Serial.println(" cancelled");
			return false;
		}
		if ((ch == 0x08 || ch == 0x7F) && len > 0) {
			len--;
			Serial.print("\b \b");
		} else if (ch >= 0x20 && ch < 0x7F && ch != '"' && len < n - 1) {
			buf[len++] = ch;
			Serial.print(ch);
		}
	}
	buf[len] = 0;
	Serial.println();
	return true;
}

void chip_test(bool full) {
	char label[33];
	if (!read_label(label, sizeof label))
		return;

	uint8_t saved_in[IO_IN_PORTS];
	for (uint8_t p = 0; p < IO_IN_PORTS; p++)
		saved_in[p] = io_in[p];
	trace_enabled = false;
	delay(5);
	evq_tail = evq_head; // drop trace events queued before the switch
	set_clock_div(CLK_DIV);
	failed_ids[0] = 0;
	uint32_t t0 = millis();

	Serial.printf("chip test (%s) -- %s, %lu kHz\n", full ? "full" : "quick", label[0] ? label : "unlabeled",
	              (unsigned long)khz(CLK_DIV));

	uint8_t tries;
	bool pass = boot_chip(&tries);
	uint32_t fmax = 0;
	if (!pass) {
		Serial.println("  boot    no response in 10 tries");
		note_failed("boot");
	} else {
		Serial.printf("  boot    ok after %u %s\n", tries, tries == 1 ? "try" : "tries");
		const char *first;
		pass = quick_suite(true, &first);
		bool aborted = false;
		if (full && pass) {
			for (uint8_t op = 0; op < 8 && !aborted; op++)
				for (uint8_t cin = 0; cin < (op == opAC || op == opSB ? 2 : 1) && !aborted; cin++)
					pass &= exhaustive(op, cin, &aborted);
			if (aborted) {
				Serial.println("  aborted");
				note_failed("aborted");
				pass = false;
			}
		}
		if (pass)
			fmax = clock_sweep();
	}

	stop_chip();
	i8008_ram_load();
	for (uint8_t p = 0; p < IO_IN_PORTS; p++)
		io_in[p] = saved_in[p];
	io_log_tail = io_log_head;
	trace_enabled = true;

	Serial.printf("done in %lu s; chip halted (t tests again, b runs the counter demo)\n",
	              (unsigned long)(millis() - t0) / 1000);
	Serial.printf("RESULT label=\"%s\" mode=%s result=%s failed=%s fmax_khz=", label, full ? "full" : "quick",
	              pass ? "PASS" : "FAIL", failed_ids[0] ? failed_ids : "-");
	if (fmax)
		Serial.printf("%lu\n", (unsigned long)fmax);
	else
		Serial.println("-");
}
