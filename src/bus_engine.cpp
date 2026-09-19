// Core 1: the real-time bus engine.
//
// Consumes capture samples from the DMA ring buffer, tracks the 8008 machine
// cycle (address latch at T1/T1I, cycle type at T2), serves instruction and
// data bytes from i8008_ram through the bus_write state machine, commits PCW
// writes back to RAM, handles PCC cycles (INP served from io_in, OUT
// recorded in io_out), and forwards every state transition to core 0 as an
// event for display.

#include "main.h"

event_t evq_buf[EVQ_SIZE];
volatile uint32_t evq_head = 0, evq_tail = 0;

// Jam sequence (see main.h): armed by 'b' (RST 0) and 'j' (JMP addr).
// Serving it keyed on cycle type instead of T1I also sidesteps the marginal
// S2 line, which can make an interrupt acknowledge read as a plain T1.
volatile uint8_t jam_seq[3];
volatile uint8_t jam_len = 0, jam_pos = 0;

volatile uint8_t io_in[IO_IN_PORTS];
volatile uint8_t io_out[IO_PORTS];
volatile uint32_t io_out_count[IO_PORTS];

uint16_t io_log[IO_LOG_SIZE];
volatile uint32_t io_log_head = 0, io_log_tail = 0;
volatile bool trace_enabled = true;
volatile uint32_t pci_count = 0;
volatile uint8_t op_states_min[256], op_states_max[256];
volatile bool op_timing_reset = false;

static uint32_t sample_count = 0;  // one per clock period
static uint32_t last_pci_t2 = 0;   // sample_count at the previous fetch
static uint8_t cur_op = 0;         // opcode of the instruction being timed
static bool timing_valid = false;
static bool cur_jam = false;       // rig-injected: not timed (see chip_test.cpp)

static uint32_t rd_idx = 0;

static uint16_t addr = 0;
static uint8_t cycle = CYC_PCI;
static bool int_ack = false; // current cycle started with T1I

// RAM-resident minimal bus_write reset for the T1I path: the full
// bus_write_reset() lives in flash and would stall right before the
// time-critical RST jam serve. The SM is idle at T1I (any drive window
// expired states ago), so drain + restart + jump is enough.
static void __not_in_flash_func(bus_write_reset_fast)() {
	pio_sm_set_enabled(pio1, sm_bus_write, false);
	pio_sm_clear_fifos(pio1, sm_bus_write);
	pio_sm_restart(pio1, sm_bus_write);
	pio_sm_exec(pio1, sm_bus_write, pio_encode_jmp(bus_write_offset));
	pio_sm_set_enabled(pio1, sm_bus_write, true);
}

static void __not_in_flash_func(handle_sample)(uint32_t raw) {
	static uint8_t prev_st = 0xFF;
	static uint8_t phase = 0;
	static uint16_t prev_raw = 0xFFFF;
	event_t ev;
	uint8_t st = raw & 7;
	uint8_t bus = (raw >> 3) & 0xFF;

	sample_count++;
	ev.raw = (uint16_t)raw;
	ev.state = st;
	ev.served = 0;
	ev.flags = 0;

	// Capture pushes one sample per clock period = two per T-state. The
	// 8008's outputs lag: the bus still shows the PREVIOUS state's value on
	// the first sample of a state and settles by the second. All cycle
	// logic therefore runs on phase 2 only.
	// T1 and T1I count as one state: they differ only in the marginal S2
	// line, and a T1 whose first sample reads T1I would otherwise never
	// reach phase 2 and its low address byte would be lost.
	uint8_t phase_key = st == ST_T1I ? ST_T1 : st;
	if (phase_key != prev_st) {
		prev_st = phase_key;
		phase = 1;
	} else if (phase < 0xFF) {
		phase++;
	}
	if (phase != 2) {
		ev.addr = addr;
		ev.cycle = cycle;
		goto queue_event;
	}

	switch (st) {
	case ST_T1:
		addr = (addr & 0x3F00) | bus;
		int_ack = false;
		break;

	case ST_T1I:
		bus_write_reset_fast(); // discard any byte stranded by a HLT before the RST jam
		addr = (addr & 0x3F00) | bus;
		int_ack = true;
		ev.flags |= EV_INTACK;
		break;

	case ST_T2:
		addr = (addr & 0x00FF) | ((uint16_t)(bus & 0x3F) << 8); // D6/D7 are the cycle code, not address
		cycle = bus >> 6;
		if (int_ack)
			ev.flags |= EV_INTACK;
		if (cycle == CYC_PCC) {
			// addr now holds A (from T1) in the low byte and the
			// instruction's port bits in the high byte
			uint8_t port = IO_PORT(bus);
			if (port < IO_IN_PORTS) {
				// INP: same serve path and deadline as a PCR read
				uint8_t data = io_in[port];
				if (!pio_sm_is_tx_fifo_full(pio1, sm_bus_write)) {
					pio_sm_put(pio1, sm_bus_write, data);
					ev.served = data;
					ev.flags |= EV_SERVED;
				}
			} else {
				// OUT: the accumulator went out at T1, nothing to drive
				io_out[port] = addr & 0xFF;
				io_out_count[port]++;
				ev.flags |= EV_OUT;
				uint32_t h = io_log_head;
				uint32_t n = (h + 1) & (IO_LOG_SIZE - 1);
				if (n != io_log_tail) {
					io_log[h] = (uint16_t)(port << 8 | (addr & 0xFF));
					io_log_head = n;
				}
			}
		} else if (cycle == CYC_PCI || cycle == CYC_PCR) {
			uint8_t data;
			if (jam_pos > 0 && jam_pos < jam_len) {
				// jam in progress: address bytes ride the PCR cycles
				data = jam_seq[jam_pos++];
				ev.flags |= EV_JAM;
			} else if (jam_len != 0 && jam_pos == 0 && cycle == CYC_PCI) {
				// jam starts on an instruction fetch
				data = jam_seq[0];
				jam_pos = 1;
				ev.flags |= EV_JAM;
			} else if (int_ack) {
				data = INT_JAM_INSTRUCTION; // bare INT with nothing armed: RST 0
				ev.flags |= EV_JAM;
			} else {
				data = i8008_ram[addr & RAM_MASK];
			}
			if (jam_pos >= jam_len) {
				jam_len = 0;
				jam_pos = 0;
			}
			if (!pio_sm_is_tx_fifo_full(pio1, sm_bus_write)) {
				pio_sm_put(pio1, sm_bus_write, data);
				ev.served = data;
				ev.flags |= EV_SERVED;
			}
			if (cycle == CYC_PCI) {
				// after the serve: timing bookkeeping must not delay it
				if (op_timing_reset) {
					timing_valid = false;
					op_timing_reset = false;
				}
				if (timing_valid && !cur_jam) {
					uint32_t states = (sample_count - last_pci_t2) >> 1;
					if (states > 255)
						states = 255;
					if (states < op_states_min[cur_op])
						op_states_min[cur_op] = states;
					if (states > op_states_max[cur_op])
						op_states_max[cur_op] = states;
				}
				timing_valid = true;
				last_pci_t2 = sample_count;
				cur_op = data;
				cur_jam = (ev.flags & EV_JAM) != 0;
				pci_count++;
			}
		}
		break;

	case ST_T3:
		if (cycle == CYC_PCW) {
			i8008_ram[addr & RAM_MASK] = bus;
			ev.flags |= EV_WRITE;
		}
		int_ack = false;
		break;

	default:
		break;
	}

	ev.addr = addr;
	ev.cycle = cycle;

queue_event:
	if (!trace_enabled)
		return;
	// Identical consecutive samples (idle WAIT/STOP periods) are not worth
	// displaying; annotated phase-2 events always pass (flags set).
	if (ev.raw == prev_raw && ev.flags == 0)
		return;
	prev_raw = ev.raw;

	uint32_t h = evq_head;
	uint32_t n = (h + 1) & (EVQ_SIZE - 1);
	if (n == evq_tail) {
		evq_dropped++;
	} else {
		evq_buf[h] = ev;
		evq_head = n; // publish after the payload is written (in-order stores on M0+)
	}
}

void setup1() {
	// All hardware is configured by core 0; loop1 waits for hw_ready.
}

// Never returns: keeps core 1 in a tight RAM-resident poll so the serve
// latency between a captured T2 and the byte reaching bus_write stays in
// the sub-microsecond range (flash/XIP stalls would push it past T3).
void __not_in_flash_func(loop1)() {
	while (!hw_ready)
		;
	while (true) {
		uint32_t wr = (dma_channel_hw_addr(dma_ch)->write_addr - (uint32_t)capture_ring) >> 2;
		wr &= RING_WORDS - 1;
		while (rd_idx != wr) {
			handle_sample(capture_ring[rd_idx]);
			rd_idx = (rd_idx + 1) & (RING_WORDS - 1);
		}
	}
}
