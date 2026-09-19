// Core 0: hardware bring-up, serial CLI and trace display.

#include "main.h"

volatile uint32_t evq_dropped = 0;
volatile bool hw_ready = false;
uint32_t capture_ring[RING_WORDS] __attribute__((aligned(RING_WORDS * sizeof(uint32_t))));
int dma_ch = -1;
uint sm_clk, sm_step, sm_int, sm_capture, sm_bus_write;
uint bus_write_offset;
static uint32_t seq = 0;
static bool raw_debug = false;

// ---------------------------------------------------------------------------
// State machine setup
// ---------------------------------------------------------------------------

static void setup_clock() {
	uint offset = pio_add_program(pio0, &clk_program);
	sm_clk = pio_claim_unused_sm(pio0, true);
	pio_sm_config c = clk_program_get_default_config(offset);
	sm_config_set_set_pins(&c, PIN_CLK1, 2);
	sm_config_set_clkdiv_int_frac(&c, CLK_DIV, 0);
	pio_gpio_init(pio0, PIN_CLK1);
	pio_gpio_init(pio0, PIN_CLK2);
	pio_sm_set_consecutive_pindirs(pio0, sm_clk, PIN_CLK1, 2, true);
	pio_sm_init(pio0, sm_clk, offset, &c);
	pio_sm_set_enabled(pio0, sm_clk, true);
}

static void setup_single_step() {
	uint offset = pio_add_program(pio0, &single_step_program);
	sm_step = pio_claim_unused_sm(pio0, true);
	pio_sm_config c = single_step_program_get_default_config(offset);
	sm_config_set_set_pins(&c, PIN_READY, 1);
	pio_gpio_init(pio0, PIN_READY);
	pio_sm_set_consecutive_pindirs(pio0, sm_step, PIN_READY, 1, true);
	pio_sm_init(pio0, sm_step, offset, &c);
	pio_sm_set_enabled(pio0, sm_step, true);
}

static void setup_int_request() {
	uint offset = pio_add_program(pio0, &int_request_program);
	sm_int = pio_claim_unused_sm(pio0, true);
	pio_sm_config c = int_request_program_get_default_config(offset);
	sm_config_set_set_pins(&c, PIN_INT, 1);
	pio_gpio_init(pio0, PIN_INT);
	pio_sm_set_consecutive_pindirs(pio0, sm_int, PIN_INT, 1, true);
	pio_sm_init(pio0, sm_int, offset, &c);
	pio_sm_set_enabled(pio0, sm_int, true);
}

static void setup_bus_write() {
	bus_write_offset = pio_add_program(pio1, &bus_write_program);
	sm_bus_write = pio_claim_unused_sm(pio1, true);
	pio_sm_config c = bus_write_program_get_default_config(bus_write_offset);
	sm_config_set_out_pins(&c, PIN_DATA_0, 8);
	sm_config_set_out_shift(&c, true, false, 32);
	sm_config_set_in_pins(&c, PIN_STATE_2);      // state-line polling for the T3 window
	sm_config_set_in_shift(&c, false, false, 32); // shift left: S2 lands on bit 0, matches T3_RAW
	sm_config_set_sideset_pins(&c, PIN_BUS_WRITE);
	for (uint pin = PIN_DATA_0; pin <= PIN_DATA_7; pin++)
		pio_gpio_init(pio1, pin);
	pio_gpio_init(pio1, PIN_BUS_WRITE);
	pio_sm_set_consecutive_pindirs(pio1, sm_bus_write, PIN_DATA_0, 8, false); // inputs until a byte is served
	pio_sm_set_pindirs_with_mask(pio1, sm_bus_write, 1u << PIN_BUS_WRITE, 1u << PIN_BUS_WRITE);
	pio_sm_init(pio1, sm_bus_write, bus_write_offset, &c);
	pio_sm_set_enabled(pio1, sm_bus_write, true);
}

static void setup_capture() {
	// Pure inputs; PIO reads pad state regardless of gpio function.
	pinMode(PIN_STATE_0, INPUT);
	pinMode(PIN_STATE_1, INPUT);
	pinMode(PIN_STATE_2, INPUT);
	pinMode(PIN_SYNC, INPUT);

	uint offset = pio_add_program(pio1, &capture_program);
	sm_capture = pio_claim_unused_sm(pio1, true);
	pio_sm_config c = capture_program_get_default_config(offset);
	sm_config_set_in_pins(&c, PIN_STATE_2);
	sm_config_set_in_shift(&c, true, false, 32);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
	pio_sm_init(pio1, sm_capture, offset, &c);

	dma_ch = dma_claim_unused_channel(true);
	dma_channel_config dc = dma_channel_get_default_config(dma_ch);
	channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
	channel_config_set_read_increment(&dc, false);
	channel_config_set_write_increment(&dc, true);
	channel_config_set_ring(&dc, true, RING_BITS);
	channel_config_set_dreq(&dc, pio_get_dreq(pio1, sm_capture, false));
	dma_channel_configure(dma_ch, &dc, capture_ring, &pio1->rxf[sm_capture], 0xFFFFFFFFu, true);

	pio_sm_set_enabled(pio1, sm_capture, true);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void cmd_step() {
	pio_sm_put_blocking(pio0, sm_step, 1);
	Serial.println("step");
}

static void cmd_run() {
	pio_sm_set_pins_with_mask(pio0, sm_step, 1u << PIN_READY, 1u << PIN_READY);
	Serial.println("run: READY high");
}

static void cmd_halt() {
	pio_sm_set_pins_with_mask(pio0, sm_step, 0, 1u << PIN_READY);
	Serial.println("halt: READY low");
}

static uint clk_div = CLK_DIV;

void int_pulse() {
	pio_sm_put_blocking(pio0, sm_int, INT_PULSE_PERIODS * 4 * clk_div);
}

// Takes effect immediately; only called while the chip is stopped.
void set_clock_div(uint div) {
	clk_div = div;
	pio_sm_set_clkdiv_int_frac(pio0, sm_clk, div, 0);
}

static void cmd_int() {
	int_pulse();
	Serial.println("INT pulse");
}

void jam_prepare(const uint8_t *seq, uint8_t len) {
	jam_len = 0; // disarm while updating
	jam_pos = 0;
	jam_nwr = 0;
	jam_npci = 0;
	for (uint8_t i = 0; i < JAM_PCI_MAX; i++)
		jam_fix_pos[i] = JAM_NO_FIX;
	for (uint8_t i = 0; i < len; i++)
		jam_seq[i] = seq[i];
}

void jam_fix(uint8_t fetch, uint8_t pos, int8_t delta) {
	jam_fix_delta[fetch] = delta;
	jam_fix_pos[fetch] = pos;
}

void jam_publish(uint8_t len) {
	jam_len = len; // last: core 1 acts on it
}

void arm_jam(const uint8_t *seq, uint8_t len) {
	jam_prepare(seq, len);
	jam_publish(len);
}

static void cmd_boot() {
	Serial.println("boot: READY high + INT pulse, RST 0 jammed on the next fetch");
	static const uint8_t rst0[] = {0x05};
	arm_jam(rst0, 1);
	pio_sm_set_pins_with_mask(pio0, sm_step, 1u << PIN_READY, 1u << PIN_READY);
	int_pulse();
}

// Read up to maxdigits hex digits terminated by Enter. Echoes input; returns
// false on empty input, a non-hex character, or Esc.
static bool read_hex(const char *prompt, uint8_t maxdigits, uint16_t *out) {
	uint16_t val = 0;
	uint8_t ndigits = 0;

	Serial.print(prompt);
	while (true) {
		while (!Serial.available())
			;
		char ch = Serial.read();
		if (ch == '\r' || ch == '\n') {
			Serial.println();
			if (ndigits == 0)
				return false;
			*out = val;
			return true;
		}
		if (ch == 0x1B) { // Esc
			Serial.println(" cancelled");
			return false;
		}
		int digit;
		if (ch >= '0' && ch <= '9')
			digit = ch - '0';
		else if (ch >= 'a' && ch <= 'f')
			digit = ch - 'a' + 10;
		else if (ch >= 'A' && ch <= 'F')
			digit = ch - 'A' + 10;
		else {
			Serial.println(" not hex, cancelled");
			return false;
		}
		if (ndigits == maxdigits) {
			Serial.println(" too long, cancelled");
			return false;
		}
		Serial.print(ch);
		val = (val << 4) | digit;
		ndigits++;
	}
}

// Jam a JMP <addr>: flow change without touching the 8008's call stack
// (unlike the RST 0 boot). Pulses INT so it also works from STOPPED; in
// single-step mode, keep pressing 's' to watch the jam go in.
static void cmd_jump() {
	uint16_t target;
	if (!read_hex("addr (hex): ", 4, &target))
		return;
	target &= RAM_MASK;
	uint8_t jmp[3] = {0x44, (uint8_t)(target & 0xFF), (uint8_t)(target >> 8)};
	arm_jam(jmp, 3);
	int_pulse();
	Serial.printf("jam JMP 0x%04X armed, INT pulsed\n", target);
}

// Set the byte an INP from port 0-7 will read.
static void cmd_set_input() {
	uint16_t port, value;
	if (!read_hex("INP port (0-7): ", 1, &port))
		return;
	if (port >= IO_IN_PORTS) {
		Serial.println("no such input port");
		return;
	}
	if (!read_hex("value (hex): ", 2, &value))
		return;
	io_in[port] = value;
	Serial.printf("INP %u will read 0x%02X\n", port, value);
}

static void cmd_ports() {
	Serial.print("in: ");
	for (uint8_t p = 0; p < IO_IN_PORTS; p++)
		Serial.printf(" %u=%02X", p, io_in[p]);
	Serial.println();
	bool any = false;
	for (uint8_t p = IO_IN_PORTS; p < IO_PORTS; p++) {
		if (io_out_count[p] == 0)
			continue;
		Serial.printf("out %2u = 0x%02X  (%lu writes)\n", p, io_out[p], (unsigned long)io_out_count[p]);
		any = true;
	}
	if (!any)
		Serial.println("out: no OUT executed yet");
}

// PC, registers, flags and stack, read at the next instruction boundary
// without changing any state. Jammed in, in order:
//   LMA..LML      each memory write carries one register (recorded, not
//                 committed)
//   JTC..JTP 0    a fetch that does not follow on at +3 means taken
//   RET x7        walks down the stack: each next fetch is an entry
//   JMP e-3, CAL  x7, walking back up from the oldest entry: the CAL's
//                 three bytes bring the PC back to e, which it pushes, so
//                 every level gets its own value back
//   JMP back      to the instruction the jam cut in on
// A halted chip is woken with INT and sent back: JMP to one before the
// resume address, where a jammed HLT leaves the PC exactly as it was. A
// chip parked in WAIT on an opcode fetch has not latched the opcode yet
// (that happens at T3), so the jam takes over that very fetch and the chip
// ends up parked on it again.
enum { RD_FLAGS = 7, RD_RET = 11, RD_CLIMB = 18, RD_FETCHES = 33 }; // jam fetch indexes

static void cmd_regs() {
	static const char *const NAME = "ABCDEHL";
	bool halted = last_state == ST_STOPPED;
	bool stepping = !gpio_get(PIN_READY) && !halted;
	bool at_fetch = stepping && last_state == ST_WAIT && cur_cycle == CYC_PCI;
	uint8_t seq[JAM_MAX];
	uint8_t n = 0;
	for (uint8_t r = 0; r < 7; r++)
		seq[n++] = 0xF8 | r; // LMr
	for (uint8_t c = 0; c < 4; c++) {
		seq[n++] = 0x60 | c << 3; // JTc 0x0000
		seq[n++] = 0x00;
		seq[n++] = 0x00;
	}
	for (uint8_t k = 0; k < 7; k++)
		seq[n++] = 0x07; // RET
	uint8_t climb_pos[8];
	for (uint8_t k = 7; k >= 1; k--) {
		seq[n++] = 0x44; // JMP entry k - 3
		climb_pos[k] = n;
		seq[n++] = 0;
		seq[n++] = 0;
		seq[n++] = 0x46; // CAL 0x0000
		seq[n++] = 0;
		seq[n++] = 0;
	}
	uint8_t ret_pos = n + 1;
	seq[n++] = 0x44; // JMP back
	seq[n++] = 0;
	seq[n++] = 0;
	if (halted)
		seq[n++] = 0x00; // HLT

	bool trace_was = trace_enabled;
	trace_enabled = false;
	jam_prepare(seq, n);
	// entry k is the fetch after the k-th RET
	for (uint8_t k = 1; k <= 7; k++)
		jam_fix(RD_RET + k, climb_pos[k], -3);
	if (at_fetch) {
		// core 1 is idle in WAIT: start the jam on the parked fetch by hand
		uint16_t p = cur_addr;
		jam_seq[ret_pos] = p & 0xFF;
		jam_seq[ret_pos + 1] = p >> 8;
		jam_pci[0] = p;
		jam_npci = 1;
		jam_pos = 1;
		jam_publish(n);
		bus_write_reset(); // drop the real opcode, serve the jam's first byte
		pio_sm_put(pio1, sm_bus_write, seq[0]);
	} else {
		jam_fix(0, ret_pos, halted ? -1 : 0);
		jam_publish(n);
		if (halted)
			int_pulse();
	}
	uint32_t t0 = millis();
	if (stepping) {
		// one machine cycle per step until the jam is through, then one
		// more to park on the fetch of the instruction it cut in on
		for (int i = 0; i < 200 && jam_len != 0; i++) {
			pio_sm_put_blocking(pio0, sm_step, 1);
			delay(2);
		}
		pio_sm_put_blocking(pio0, sm_step, 1);
		delay(2);
	} else {
		while (jam_len != 0 && millis() - t0 < 200)
			;
		delay(2);
	}
	trace_enabled = trace_was;

	if (jam_len != 0 || jam_nwr != 7 || jam_npci < RD_FETCHES) {
		arm_jam(nullptr, 0);
		Serial.println("regs: the chip did not run the read-out");
		return;
	}
	uint16_t pc = jam_pci[0]; // the fetch the read-out cut in on
	Serial.printf("PC=%04X  ", pc);
	for (uint8_t r = 0; r < 7; r++)
		Serial.printf("%c=%02X ", NAME[r], jam_wr[r]);
	Serial.print(" flags ");
	for (uint8_t c = 0; c < 4; c++) {
		uint16_t fall_through = (jam_pci[RD_FLAGS + c] + 3) & RAM_MASK;
		Serial.printf("%c%u ", "CZSP"[c], jam_pci[RD_FLAGS + c + 1] != fall_through);
	}
	char mn[8];
	Serial.printf(" next: %s%s\n", i8008_mnemonic(i8008_ram[pc], mn), halted ? " (halted)" : "");
	Serial.print("stack, newest first:");
	for (uint8_t k = 1; k <= 7; k++)
		Serial.printf(" %04X", jam_pci[RD_RET + k]);
	// the first CAL of the climb must sit 3 below the oldest entry
	if (jam_pci[RD_CLIMB + 1] != ((jam_pci[RD_CLIMB] - 3) & RAM_MASK))
		Serial.print("  (climb check failed: stack may be disturbed)");
	Serial.println();
}

static void cmd_dump() {
	for (uint16_t a = 0; a < 0x100; a += 16) {
		Serial.printf("%04X:", a);
		for (uint16_t i = 0; i < 16; i++)
			Serial.printf(" %02X", i8008_ram[a + i]);
		Serial.println();
	}
}

// Force bus_write back to a released, idle state. Used by the 'z' command and
// by core 1 on every T1I, so a byte stranded by a HLT can never be delivered
// in place of the RST 0 jam after the next interrupt.
void bus_write_reset() {
	pio_sm_set_enabled(pio1, sm_bus_write, false);
	pio_sm_clear_fifos(pio1, sm_bus_write);
	pio_sm_set_consecutive_pindirs(pio1, sm_bus_write, PIN_DATA_0, 8, false);
	pio_sm_restart(pio1, sm_bus_write);
	pio_sm_exec(pio1, sm_bus_write, pio_encode_jmp(bus_write_offset));
	pio_sm_set_enabled(pio1, sm_bus_write, true);
}

static void cmd_bus_reset() {
	bus_write_reset();
	Serial.println("bus_write reset, bus released");
}

static void cmd_help() {
	Serial.println("i8008-pico commands:");
	Serial.println("  b  boot: READY high + INT pulse, RST 0 jam -> PC 0x0000");
	Serial.println("  j  jam JMP <addr> (hex entry, no stack push)");
	Serial.println("  s  single step (one machine cycle)");
	Serial.println("  g  go / free run (READY high)");
	Serial.println("  w  wait / halt (READY low)");
	Serial.println("  i  INT pulse");
	Serial.println("  x  dump RAM 0x0000-0x007F");
	Serial.println("  n  set an input port (INP 0-7) value");
	Serial.println("  p  show input ports and last OUT per port");
	Serial.println("  r  registers and flags (at the next instruction)");
	Serial.println("  t  chip test: functional + timing + clock sweep");
	Serial.println("  T  chip test with exhaustive ALU (several minutes)");
	Serial.println("  z  reset bus_write SM (release the bus)");
	Serial.println("  d  toggle raw sample dump");
	Serial.println("  h  this help");
}

// ---------------------------------------------------------------------------
// Trace display
// ---------------------------------------------------------------------------

static void print_event(const event_t &e) {
	char bin[9];
	char mn[8];
	char note[48] = "";
	uint8_t bus = (e.raw >> 3) & 0xFF;

	for (int i = 0; i < 8; i++)
		bin[i] = (bus & (0x80 >> i)) ? '1' : '0';
	bin[8] = 0;

	switch (e.state) {
	case ST_T1:
		snprintf(note, sizeof note, "addr.lo <- 0x%02X", bus);
		break;
	case ST_T1I:
		snprintf(note, sizeof note, "addr.lo <- 0x%02X  INT ACK", bus);
		break;
	case ST_T2:
		if (e.cycle == CYC_PCC) {
			uint8_t port = IO_PORT(e.addr >> 8);
			if (e.flags & EV_OUT)
				snprintf(note, sizeof note, "PCC OUT %u = 0x%02X", port, e.addr & 0xFF);
			else if (e.flags & EV_SERVED)
				snprintf(note, sizeof note, "PCC INP %u  serve 0x%02X", port, e.served);
			else
				snprintf(note, sizeof note, "PCC port %u", port);
		} else if (e.flags & EV_SERVED)
			snprintf(note, sizeof note, "%s 0x%04X  serve 0x%02X%s", CYCLE_NAME[e.cycle], e.addr,
			         e.served, (e.flags & EV_JAM) ? " (jam)" : "");
		else
			snprintf(note, sizeof note, "%s 0x%04X", CYCLE_NAME[e.cycle], e.addr);
		break;
	case ST_T3:
		if (e.flags & EV_WRITE)
			snprintf(note, sizeof note, "write ram[0x%04X] = 0x%02X", e.addr, bus);
		else if (e.cycle == CYC_PCI)
			snprintf(note, sizeof note, "fetch %s", i8008_mnemonic(bus, mn));
		else if (e.cycle == CYC_PCR)
			snprintf(note, sizeof note, "data 0x%02X", bus);
		else
			snprintf(note, sizeof note, "io 0x%02X", bus);
		break;
	default:
		break;
	}

	Serial.printf("%6lu  %-4s  %s 0x%02X  %c%c%c  %s\n", (unsigned long)seq++,
	              STATE_NAME[e.state], bin, bus,
	              (e.raw & (1u << 12)) ? 'R' : '.',  // READY
	              (e.raw & (1u << 13)) ? 'I' : '.',  // INT
	              (e.raw & (1u << 11)) ? 'D' : '.',  // Pico drives the bus
	              note);

	if (raw_debug) {
		char rb[15];
		for (int i = 0; i < 14; i++)
			rb[i] = (e.raw & (1u << (13 - i))) ? '1' : '0';
		rb[14] = 0;
		Serial.printf("        raw=%s\n", rb);
	}
}

// ---------------------------------------------------------------------------

void setup() {
	Serial.begin(115200);

	i8008_ram_load();

	setup_clock();
	setup_single_step();
	setup_int_request();
	setup_bus_write();
	setup_capture(); // last: capture + DMA start once everything else is up

	hw_ready = true;

	Serial.println("i8008-pico support rig ready ('h' for help)");
}

void loop() {
	static uint32_t dropped_seen = 0;

	// Bounded drain: in free run the queue refills faster than USB prints,
	// so an unbounded loop here would starve command processing forever.
	for (int burst = 0; burst < 32 && evq_tail != evq_head; burst++) {
		event_t ev = evq_buf[evq_tail];
		evq_tail = (evq_tail + 1) & (EVQ_SIZE - 1);
		print_event(ev);
	}

	if (evq_dropped != dropped_seen) {
		dropped_seen = evq_dropped;
		Serial.printf("!! event queue overflow, %lu dropped total\n", (unsigned long)dropped_seen);
	}

	if (Serial.available()) {
		char command = Serial.read();
		switch (command) {
		case 'b': cmd_boot(); break;
		case 'j': cmd_jump(); break;
		case 's': cmd_step(); break;
		case 'g': cmd_run(); break;
		case 'w': cmd_halt(); break;
		case 'i': cmd_int(); break;
		case 'x': cmd_dump(); break;
		case 'n': cmd_set_input(); break;
		case 'p': cmd_ports(); break;
		case 'r': cmd_regs(); break;
		case 't': chip_test(false); break;
		case 'T': chip_test(true); break;
		case 'z': cmd_bus_reset(); break;
		case 'd':
			raw_debug = !raw_debug;
			Serial.printf("raw dump %s\n", raw_debug ? "on" : "off");
			break;
		case 'h':
		case '?': cmd_help(); break;
		case '\r':
		case '\n': break;
		default:
			Serial.printf("unknown command: %c ('h' for help)\n", command);
			break;
		}
	}
}
