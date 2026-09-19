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

static void cmd_int() {
	pio_sm_put_blocking(pio0, sm_int, INT_PULSE_CYCLES);
	Serial.println("INT pulse");
}

static void arm_jam(const uint8_t *seq, uint8_t len) {
	jam_len = 0; // disarm while updating
	jam_pos = 0;
	for (uint8_t i = 0; i < len; i++)
		jam_seq[i] = seq[i];
	jam_len = len; // publish last
}

static void cmd_boot() {
	Serial.println("boot: READY high + INT pulse, RST 0 jammed on the next fetch");
	static const uint8_t rst0[] = {0x05};
	arm_jam(rst0, 1);
	pio_sm_set_pins_with_mask(pio0, sm_step, 1u << PIN_READY, 1u << PIN_READY);
	pio_sm_put_blocking(pio0, sm_int, INT_PULSE_CYCLES);
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
	pio_sm_put_blocking(pio0, sm_int, INT_PULSE_CYCLES);
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
