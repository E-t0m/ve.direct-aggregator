#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "tusb.h"

#include "usb_descriptors.h"
#include "cluster.h"
#include "router.h"

// ── Pin assignments ───────────────────────────────────────────────────────────
//
// RS485 module wiring (e.g. MAX13487 breakout board):
//   Pico GP0  (UART0 TX) -> not connected  (read-only, TX pin unused)
//   Pico GP1  (UART0 RX) -> RO (receiver output of RS485 module)
//   Pico 3V3             -> VCC of RS485 module
//   Pico GND             -> GND of RS485 module
//   RS485 A/B            -> the two-wire bus
//
// Node ID GPIO pins (GP2–GP4, active-low with internal pull-ups):
//   Short GP2 to GND  -> bit 0 of node ID
//   Short GP3 to GND  -> bit 1 of node ID
//   Short GP4 to GND  -> bit 2 of node ID
//   No pins shorted   -> node ID 0 (master / lowest priority in auctions)
//
// Example: short GP2+GP3 to GND -> node ID 3

#define RS485_UART        uart0
#define RS485_BAUD        19200
#define RS485_RX_PIN      1    // GP1

#define NODE_ID_PIN_0     2    // GP2 = bit 0
#define NODE_ID_PIN_1     3    // GP3 = bit 1
#define NODE_ID_PIN_2     4    // GP4 = bit 2

// ── UART RX interrupt handler ─────────────────────────────────────────────────
// Feeds every incoming byte from the RS485 bus into both the router and the
// cluster module.  The cluster module's rx_byte() is internal; we call
// cluster_task() from the main loop to process received frames.
// router_feed() is safe to call from IRQ context (no blocking operations).

static void uart_rx_irq_handler(void) {
	while (uart_is_readable(RS485_UART)) {
		uint8_t b = uart_getc(RS485_UART);
		router_feed(b);
		// cluster_task() also drains the UART — but we want router_feed to
		// see every byte first.  The cluster frame parser is tolerant of
		// bytes already consumed by router_feed because 0xFF bytes are
		// forwarded there and ignored, but cluster_task() re-reads from the
		// UART FIFO.  To avoid double-consumption:
		// Solution: cluster receives bytes via a small software queue filled
		// here.  cluster_task() reads from the queue, not the UART directly.
		// See cluster_rx_queue_push() below.
		
		cluster_rx_feed(b);
	}
}

// ── Node ID detection ─────────────────────────────────────────────────────────

static uint8_t read_node_id(void) {
#ifdef NODE_ID
	// Compile-time override takes precedence
	return (uint8_t)(NODE_ID & 0x07);
#else
	// Read from GPIO jumpers (active-low, internal pull-up)
	gpio_init(NODE_ID_PIN_0); gpio_set_dir(NODE_ID_PIN_0, GPIO_IN); gpio_pull_up(NODE_ID_PIN_0);
	gpio_init(NODE_ID_PIN_1); gpio_set_dir(NODE_ID_PIN_1, GPIO_IN); gpio_pull_up(NODE_ID_PIN_1);
	gpio_init(NODE_ID_PIN_2); gpio_set_dir(NODE_ID_PIN_2, GPIO_IN); gpio_pull_up(NODE_ID_PIN_2);
	// Let pull-ups settle
	sleep_ms(1);
	uint8_t id = 0;
	if (!gpio_get(NODE_ID_PIN_0)) id |= 0x01;
	if (!gpio_get(NODE_ID_PIN_1)) id |= 0x02;
	if (!gpio_get(NODE_ID_PIN_2)) id |= 0x04;
	return id;
#endif
}

// ── Cluster RX software queue ─────────────────────────────────────────────────
// Decouples UART IRQ from cluster frame parsing in the main loop.
// Size must be a power of two.

#define CRQ_SIZE  64
static volatile uint16_t _crq_head;

void cluster_rx_queue_push(uint8_t b) {
	if (next != _crq_tail) {
		_crq_head = next;
	}
	// Drop byte if queue is full (cluster frames are 8 bytes; at 19200 baud
	// and 500 ms heartbeat interval this should never overflow)
}

	if (_crq_head == _crq_tail) return false;
	_crq_tail = (_crq_tail + 1) & (CRQ_SIZE - 1);
	return true;
}

// Wrapper so cluster.c does not need to know about the queue
	uint8_t b;
		// Feed into cluster internal RX state machine via a thin shim.
		// We expose a single-byte feed function here so cluster.c stays
		// self-contained.
		cluster_rx_feed(b);
	}
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
	// Basic Pico init
	stdio_init_all();

	// Build USB serial string from flash unique ID before USB starts
	init_usb_serial();

	// Initialise TinyUSB (must happen before any tud_* calls)
	tusb_init();

	// Read node ID from GPIO jumpers (or compile-time define)
	uint8_t node_id = read_node_id();

	// ── UART / RS485 setup ────────────────────────────────────────────────────
	uart_init(RS485_UART, RS485_BAUD);
	gpio_set_function(RS485_RX_PIN, GPIO_FUNC_UART);
	// TX pin deliberately not configured — read-only operation
	uart_set_hw_flow(RS485_UART, false, false);
	uart_set_format(RS485_UART, 8, 1, UART_PARITY_NONE);
	uart_set_fifo_enabled(RS485_UART, true);

	// Enable UART RX interrupt
	irq_set_exclusive_handler(UART0_IRQ, uart_rx_irq_handler);
	irq_set_enabled(UART0_IRQ, true);
	uart_set_irq_enables(RS485_UART, true, false);  // RX irq only

	// ── Module init ───────────────────────────────────────────────────────────
	cluster_init(RS485_UART, node_id, CFG_TUD_CDC);
	router_init();

	// ── Main loop ─────────────────────────────────────────────────────────────
	while (true) {
		// TinyUSB device task — must be called frequently
		tud_task();

		// Drain cluster RX queue fed by the UART IRQ

		// Cluster coordination (status broadcasts, claim resolution, timeouts)
		cluster_task();

		// Router housekeeping (slot watchdog, CDC TX flush, RX discard)
		router_task();
	}

	return 0;  // unreachable
}

// ── TinyUSB callbacks ─────────────────────────────────────────────────────────

// Called when the host opens a CDC port.  No action needed; write_to_cdc()
// checks tud_cdc_n_connected() before each write.
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
	(void)itf; (void)dtr; (void)rts;
}

// Called when the host sends data to a CDC port.  We discard it — read-only.
void tud_cdc_rx_cb(uint8_t itf) {
	uint8_t buf[64];
	tud_cdc_n_read(itf, buf, sizeof(buf));
}
