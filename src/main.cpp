// Core 0: hardware bring-up, serial CLI and trace display.

#include "main.h"

queue_t event_queue;
volatile uint32_t evq_dropped = 0;
volatile bool hw_ready = false;
uint32_t capture_ring[RING_WORDS] __attribute__((aligned(RING_WORDS * sizeof(uint32_t))));
int dma_ch = -1;
uint sm_clk, sm_step, sm_int, sm_capture, sm_bus_write;

static uint bus_write_offset;
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

static void cmd_boot() {
	Serial.println("boot: READY high + INT pulse (RST 0 will be jammed)");
	pio_sm_set_pins_with_mask(pio0, sm_step, 1u << PIN_READY, 1u << PIN_READY);
	pio_sm_put_blocking(pio0, sm_int, INT_PULSE_CYCLES);
}

static void cmd_dump() {
	for (uint16_t a = 0; a < 0x80; a += 16) {
		Serial.printf("%04X:", a);
		for (uint16_t i = 0; i < 16; i++)
			Serial.printf(" %02X", i8008_ram[a + i]);
		Serial.println();
	}
}

// Force bus_write back to a released, idle state (recovery after a desync,
// e.g. a serve that never met its T3).
static void cmd_bus_reset() {
	pio_sm_set_enabled(pio1, sm_bus_write, false);
	pio_sm_clear_fifos(pio1, sm_bus_write);
	pio_interrupt_clear(pio1, BUS_DRIVE_IRQ);
	pio_sm_set_consecutive_pindirs(pio1, sm_bus_write, PIN_DATA_0, 8, false);
	pio_sm_restart(pio1, sm_bus_write);
	pio_sm_exec(pio1, sm_bus_write, pio_encode_jmp(bus_write_offset));
	pio_sm_set_enabled(pio1, sm_bus_write, true);
	Serial.println("bus_write reset, bus released");
}

static void cmd_help() {
	Serial.println("i8008-pico commands:");
	Serial.println("  b  boot: READY high + INT pulse");
	Serial.println("  s  single step (one machine cycle)");
	Serial.println("  g  go / free run (READY high)");
	Serial.println("  w  wait / halt (READY low)");
	Serial.println("  i  INT pulse");
	Serial.println("  x  dump RAM 0x0000-0x007F");
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
		if (e.flags & EV_SERVED)
			snprintf(note, sizeof note, "%s 0x%04X  serve 0x%02X%s", CYCLE_NAME[e.cycle], e.addr,
			         e.served, (e.flags & EV_INTACK) ? " (RST 0 jam)" : "");
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
	queue_init(&event_queue, sizeof(event_t), 512);

	setup_clock();
	setup_single_step();
	setup_int_request();
	setup_bus_write();
	setup_capture(); // last: capture + DMA start once everything else is up

	hw_ready = true;

	Serial.println("i8008-pico support rig ready ('h' for help)");
}

void loop() {
	event_t ev;
	static uint32_t dropped_seen = 0;

	while (queue_try_remove(&event_queue, &ev))
		print_event(ev);

	if (evq_dropped != dropped_seen) {
		dropped_seen = evq_dropped;
		Serial.printf("!! event queue overflow, %lu dropped total\n", (unsigned long)dropped_seen);
	}

	if (Serial.available()) {
		char command = Serial.read();
		switch (command) {
		case 'b': cmd_boot(); break;
		case 's': cmd_step(); break;
		case 'g': cmd_run(); break;
		case 'w': cmd_halt(); break;
		case 'i': cmd_int(); break;
		case 'x': cmd_dump(); break;
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
