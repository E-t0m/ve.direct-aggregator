#include "cluster.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include <string.h>
#include <stdio.h>

// ── CRC-8/MAXIM ───────────────────────────────────────────────────────────────
// Polynomial 0x31, init 0x00. Small table-free implementation.

static uint8_t crc8(const uint8_t *data, size_t len) {
	uint8_t crc = 0x00;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

// ── Module state ──────────────────────────────────────────────────────────────

static uart_inst_t  *_uart;
static uint8_t       _node_id;
static uint8_t       _slots_total;
static uint8_t       _slots_used;
static uint8_t       _slots_free;

static node_status_t _nodes[CLUSTER_MAX_NODES];
static ser_entry_t   _devs[CLUSTER_MAX_DEVS];
static uint8_t       _dev_count;

// Receive state machine
typedef enum { RX_WAIT_MARKER, RX_BODY } rx_state_t;
static rx_state_t  _rx_state = RX_WAIT_MARKER;
static uint8_t     _rx_buf[CLUSTER_FRAME_LEN];
static uint8_t     _rx_pos;

// Timing
static uint32_t    _last_status_ms;

// Pending claim: non-empty while we are waiting our deterministic delay
// before broadcasting a CLAIM frame
static char        _pending_claim_ser[CLUSTER_SER_LEN];
static uint32_t    _claim_send_at_ms;

// ── Internal helpers ──────────────────────────────────────────────────────────

static void send_frame(uint8_t type, const char *ser) {
	cluster_frame_t f;
	memset(&f, 0, sizeof(f));
	f.marker     = CLUSTER_MARKER;
	f.type       = type;
	f.node_id    = _node_id;
	f.slots_used = _slots_used;
	f.slots_free = _slots_free;
	if (ser) {
		strncpy((char *)f.ser, ser, CLUSTER_SER_WIRE);
	}
	// CRC covers bytes 1 through (CLUSTER_FRAME_LEN - 2), i.e. everything
	// except the marker and the crc byte itself
	f.crc8 = crc8((uint8_t *)&f + 1, CLUSTER_FRAME_LEN - 2);
	uart_write_blocking(_uart, (uint8_t *)&f, sizeof(f));
}

static void process_frame(const cluster_frame_t *f) {
	// Validate CRC
	uint8_t expected = crc8((uint8_t *)f + 1, CLUSTER_FRAME_LEN - 2);
	if (f->crc8 != expected) return;
	if (f->node_id >= CLUSTER_MAX_NODES) return;

	uint32_t now = to_ms_since_boot(get_absolute_time());

	// Extract null-terminated SER# from wire representation
	char ser[CLUSTER_SER_LEN];
	memset(ser, 0, sizeof(ser));
	memcpy(ser, f->ser, CLUSTER_SER_WIRE);

	switch (f->type) {

		case CLUSTER_FRAME_STATUS:
			_nodes[f->node_id].alive        = true;
			_nodes[f->node_id].slots_used   = f->slots_used;
			_nodes[f->node_id].slots_free   = f->slots_free;
			_nodes[f->node_id].last_seen_ms = now;
			break;

		case CLUSTER_FRAME_CLAIM:
			// Another node is claiming this serial number.
			// If we have a pending claim for the same SER# and the sender
			// has a lower node_id than us, surrender — they win.
			if (_pending_claim_ser[0] != '\0' &&
			    strncmp(_pending_claim_ser, ser, CLUSTER_SER_LEN) == 0 &&
			    f->node_id < _node_id) {
				_pending_claim_ser[0] = '\0';
			}
			// Record ownership
			for (int i = 0; i < _dev_count; i++) {
				if (strncmp(_devs[i].ser, ser, CLUSTER_SER_LEN) == 0) {
					_devs[i].owner = f->node_id;
					return;
				}
			}
			// New entry
			if (_dev_count < CLUSTER_MAX_DEVS) {
				strncpy(_devs[_dev_count].ser, ser, CLUSTER_SER_LEN - 1);
				_devs[_dev_count].owner = f->node_id;
				_dev_count++;
			}
			break;

		case CLUSTER_FRAME_RELEASE:
			for (int i = 0; i < _dev_count; i++) {
				if (strncmp(_devs[i].ser, ser, CLUSTER_SER_LEN) == 0 &&
				    _devs[i].owner == f->node_id) {
					_devs[i].owner = CLUSTER_SER_UNOWNED;
				}
			}
			break;
	}
}

// Internal single-byte RX state machine (fed via cluster_rx_feed)
static void rx_byte(uint8_t b) {
	switch (_rx_state) {
		case RX_WAIT_MARKER:
			if (b == CLUSTER_MARKER) {
				_rx_buf[0] = b;
				_rx_pos    = 1;
				_rx_state  = RX_BODY;
			}
			break;

		case RX_BODY:
			_rx_buf[_rx_pos++] = b;
			if (_rx_pos == CLUSTER_FRAME_LEN) {
				cluster_frame_t *f = (cluster_frame_t *)_rx_buf;
				// Ignore frames we sent ourselves
				if (f->node_id != _node_id) {
					process_frame(f);
				}
				_rx_state = RX_WAIT_MARKER;
			}
			break;
	}
}

static void check_dead_nodes(uint32_t now) {
	for (int n = 0; n < CLUSTER_MAX_NODES; n++) {
		if (!_nodes[n].alive) continue;
		if ((now - _nodes[n].last_seen_ms) > CLUSTER_NODE_TIMEOUT_MS) {
			_nodes[n].alive      = false;
			_nodes[n].slots_free = 0;
			// Release all serial numbers owned by the dead node so
			// other nodes can reclaim them
			for (int i = 0; i < _dev_count; i++) {
				if (_devs[i].owner == (uint8_t)n) {
					_devs[i].owner = CLUSTER_SER_UNOWNED;
				}
			}
		}
	}
}

// ── Public API ────────────────────────────────────────────────────────────────

void cluster_init(uart_inst_t *uart, uint8_t node_id, uint8_t slots_total) {
	_uart         = uart;
	_node_id      = node_id;
	_slots_total  = slots_total;
	_slots_used   = 0;
	_slots_free   = slots_total;
	_dev_count    = 0;
	_pending_claim_ser[0] = '\0';

	memset(_nodes, 0, sizeof(_nodes));
	memset(_devs,  0, sizeof(_devs));
	for (int i = 0; i < CLUSTER_MAX_DEVS; i++) {
		_devs[i].owner = CLUSTER_SER_UNOWNED;
	}

	_last_status_ms = to_ms_since_boot(get_absolute_time());
}

void cluster_rx_feed(uint8_t b) {
	rx_byte(b);
}

bool cluster_task(void) {
	uint32_t now      = to_ms_since_boot(get_absolute_time());
	bool     did_work = false;

	// Periodic status broadcast
	if ((now - _last_status_ms) >= CLUSTER_STATUS_INTERVAL_MS) {
		send_frame(CLUSTER_FRAME_STATUS, NULL);
		_last_status_ms = now;
		did_work = true;
	}

	// Check for node timeouts
	check_dead_nodes(now);

	// Send pending claim when our deterministic delay window has passed
	if (_pending_claim_ser[0] != '\0' && now >= _claim_send_at_ms) {
		uint8_t owner = cluster_ser_owner(_pending_claim_ser);
		if (owner == CLUSTER_SER_UNOWNED && _slots_free > 0) {
			send_frame(CLUSTER_FRAME_CLAIM, _pending_claim_ser);
			// Record the claim locally
			bool found = false;
			for (int i = 0; i < _dev_count; i++) {
				if (strncmp(_devs[i].ser, _pending_claim_ser, CLUSTER_SER_LEN) == 0) {
					_devs[i].owner = _node_id;
					found = true;
					break;
				}
			}
			if (!found && _dev_count < CLUSTER_MAX_DEVS) {
				strncpy(_devs[_dev_count].ser, _pending_claim_ser, CLUSTER_SER_LEN - 1);
				_devs[_dev_count].owner = _node_id;
				_dev_count++;
			}
			did_work = true;
		}
		_pending_claim_ser[0] = '\0';
	}

	return did_work;
}

bool cluster_claim_ser(const char *ser) {
	if (!ser || ser[0] == '\0') return false;

	uint8_t owner = cluster_ser_owner(ser);
	if (owner == _node_id)           return true;   // already ours
	if (owner != CLUSTER_SER_UNOWNED) return false; // someone else has it
	if (_slots_free == 0)             return false;  // no room

	// Schedule claim after node-ID-proportional delay
	uint32_t now = to_ms_since_boot(get_absolute_time());
	strncpy(_pending_claim_ser, ser, CLUSTER_SER_LEN - 1);
	_pending_claim_ser[CLUSTER_SER_LEN - 1] = '\0';
	_claim_send_at_ms = now + (uint32_t)(_node_id * CLUSTER_CLAIM_DELAY_BASE_MS);
	return true;  // claim initiated; caller re-checks cluster_i_own() later
}

uint8_t cluster_ser_owner(const char *ser) {
	for (int i = 0; i < _dev_count; i++) {
		if (strncmp(_devs[i].ser, ser, CLUSTER_SER_LEN) == 0) {
			return _devs[i].owner;
		}
	}
	return CLUSTER_SER_UNOWNED;
}

bool cluster_i_own(const char *ser) {
	return cluster_ser_owner(ser) == _node_id;
}

void cluster_release_ser(const char *ser) {
	for (int i = 0; i < _dev_count; i++) {
		if (strncmp(_devs[i].ser, ser, CLUSTER_SER_LEN) == 0 &&
		    _devs[i].owner == _node_id) {
			_devs[i].owner = CLUSTER_SER_UNOWNED;
			send_frame(CLUSTER_FRAME_RELEASE, ser);
			return;
		}
	}
}

void cluster_update_slots(uint8_t used, uint8_t free) {
	_slots_used = used;
	_slots_free = free;
}

uint8_t cluster_slots_free(void) {
	return _slots_free;
}

node_status_t const* cluster_node_status(void) {
	return _nodes;
}

ser_entry_t const* cluster_ser_table(void) {
	return _devs;
}
