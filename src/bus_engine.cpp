// Core 1: the real-time bus engine.
//
// Consumes capture samples from the DMA ring buffer, tracks the 8008 machine
// cycle (address latch at T1/T1I, cycle type at T2), serves instruction and
// data bytes from i8008_ram through the bus_write state machine, commits PCW
// writes back to RAM, and forwards every state transition to core 0 as an
// event for display.

#include "main.h"

static uint32_t rd_idx = 0;

static uint16_t addr = 0;
static uint8_t cycle = CYC_PCI;
static bool int_ack = false; // current cycle started with T1I
static bool driving = false; // a byte is queued/being driven by bus_write

static void handle_sample(uint32_t raw) {
	event_t ev;
	uint8_t st = raw & 7;
	uint8_t bus = (raw >> 3) & 0xFF;

	ev.raw = (uint16_t)raw;
	ev.state = st;
	ev.served = 0;
	ev.flags = 0;

	switch (st) {
	case ST_T1:
		addr = (addr & 0x3F00) | bus;
		int_ack = false;
		break;

	case ST_T1I:
		addr = (addr & 0x3F00) | bus;
		int_ack = true;
		ev.flags |= EV_INTACK;
		break;

	case ST_T2:
		addr = (addr & 0x00FF) | ((uint16_t)(bus & 0x3F) << 8); // D6/D7 are the cycle code, not address
		cycle = bus >> 6;
		if (int_ack)
			ev.flags |= EV_INTACK;
		if (cycle == CYC_PCI || cycle == CYC_PCR) {
			uint8_t data = int_ack ? INT_JAM_INSTRUCTION : i8008_ram[addr & RAM_MASK];
			if (!pio_sm_is_tx_fifo_full(pio1, sm_bus_write)) {
				pio_sm_put(pio1, sm_bus_write, data);
				driving = true;
				ev.served = data;
				ev.flags |= EV_SERVED;
			}
		}
		break;

	case ST_T3:
		if (driving) {
			// Data has been on the bus through T3; let bus_write release it
			// at the next state boundary.
			pio1->irq_force = 1u << BUS_DRIVE_IRQ;
			driving = false;
		}
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
	if (!queue_try_add(&event_queue, &ev))
		evq_dropped++;
}

void setup1() {
	// All hardware is configured by core 0; loop1 waits for hw_ready.
}

void loop1() {
	if (!hw_ready)
		return;

	uint32_t wr = (dma_channel_hw_addr(dma_ch)->write_addr - (uint32_t)capture_ring) >> 2;
	wr &= RING_WORDS - 1;
	while (rd_idx != wr) {
		handle_sample(capture_ring[rd_idx]);
		rd_idx = (rd_idx + 1) & (RING_WORDS - 1);
	}
}
