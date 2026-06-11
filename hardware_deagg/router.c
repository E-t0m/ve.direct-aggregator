#include "router.h"
#include "cluster.h"
#include "pico/stdlib.h"
#include <string.h>

// ── Block assembler state ─────────────────────────────────────────────────────

static uint8_t  _block_buf[ROUTER_MAX_BLOCK_BYTES];
static uint16_t _block_len;
static uint8_t  _prev_byte;

// ── Pending block ─────────────────────────────────────────────────────────────
// When a block arrives for a SER# that has an in-flight claim (cluster_claim_ser
// returned true but cluster_i_own has not yet confirmed), we hold one block here.
// The next router_task() iteration dispatches it once ownership is confirmed.

static uint8_t  _pending_buf[ROUTER_MAX_BLOCK_BYTES];
static uint16_t _pending_len;
static char     _pending_ser[CLUSTER_SER_LEN];

// ── CDC slot table ────────────────────────────────────────────────────────────

static router_slot_t _slots[ROUTER_CDC_PORTS];

// ── Internal helpers ──────────────────────────────────────────────────────────

// Extract SER# value from a complete raw block.
// VE.Direct SER# line format: "SER#\tHQ2242ABCDE\r\n"
// Writes the value into out (null-terminated, max CLUSTER_SER_LEN bytes).
// Returns true on success.
static bool extract_ser(const uint8_t *block, uint16_t len,
                         char *out, size_t out_len) {
	for (uint16_t i = 0; i + 5 < len; i++) {
		if (block[i]   == 'S' && block[i+1] == 'E' &&
		    block[i+2] == 'R' && block[i+3] == '#' &&
		    block[i+4] == '\t') {
			uint16_t j   = i + 5;
			size_t   n   = 0;
			while (j < len && n < out_len - 1) {
				uint8_t c = block[j++];
				if (c == '\r' || c == '\n') break;
				out[n++] = (char)c;
			}
			out[n] = '\0';
			return n > 0;
		}
	}
	return false;
}

// Find the CDC slot index assigned to ser, or -1 if none on this node.
static int slot_for_ser(const char *ser) {
	for (int i = 0; i < ROUTER_CDC_PORTS; i++) {
		if (_slots[i].ser[0] != '\0' &&
		    strncmp(_slots[i].ser, ser, CLUSTER_SER_LEN) == 0) {
			return i;
		}
	}
	return -1;
}

// Assign a free CDC slot to ser. Returns slot index or -1 if full.
static int assign_slot(const char *ser) {
	for (int i = 0; i < ROUTER_CDC_PORTS; i++) {
		if (_slots[i].ser[0] == '\0') {
			strncpy(_slots[i].ser, ser, CLUSTER_SER_LEN - 1);
			_slots[i].ser[CLUSTER_SER_LEN - 1] = '\0';
			_slots[i].last_used_ms = to_ms_since_boot(get_absolute_time());
			_slots[i].blocks_sent  = 0;
			uint8_t used = 0;
			for (int j = 0; j < ROUTER_CDC_PORTS; j++) {
				if (_slots[j].ser[0] != '\0') used++;
			}
			cluster_update_slots(used, (uint8_t)(ROUTER_CDC_PORTS - used));
			return i;
		}
	}
	return -1;
}

// Free a CDC slot and notify the cluster.
static void free_slot(int idx) {
	if (idx < 0 || idx >= ROUTER_CDC_PORTS) return;
	if (_slots[idx].ser[0] == '\0') return;
	cluster_release_ser(_slots[idx].ser);
	_slots[idx].ser[0]      = '\0';
	_slots[idx].last_used_ms = 0;
	uint8_t used = 0;
	for (int i = 0; i < ROUTER_CDC_PORTS; i++) {
		if (_slots[i].ser[0] != '\0') used++;
	}
	cluster_update_slots(used, (uint8_t)(ROUTER_CDC_PORTS - used));
}

// Write a block to a CDC port. Silently drops the block if the host has not
// opened the port (Venus OS connects each ttyACM only when serial-starter
// launches the driver for it).
static void write_to_cdc(uint8_t cdc_idx, const uint8_t *data, uint16_t len) {
	if (!tud_cdc_n_connected(cdc_idx)) return;
	tud_cdc_n_write(cdc_idx, data, len);
	tud_cdc_n_write_flush(cdc_idx);
}

// Dispatch a complete assembled block.
static void dispatch_block(const uint8_t *block, uint16_t len) {
	char ser[CLUSTER_SER_LEN];
	if (!extract_ser(block, len, ser, sizeof(ser))) return;  // no SER# found

	// If another node already owns this SER#, ignore the block
	uint8_t owner = cluster_ser_owner(ser);
	if (owner != CLUSTER_SER_UNOWNED && !cluster_i_own(ser)) return;

	// Try to find an existing local slot
	int slot = slot_for_ser(ser);

	if (slot < 0) {
		// First time we see this SER# on this node
		if (!cluster_i_own(ser)) {
			bool claim_started = cluster_claim_ser(ser);
			if (!claim_started) return;  // no room or another node has it

			// Hold the block until the claim is confirmed in cluster_task()
			if (_pending_ser[0] == '\0') {
				memcpy(_pending_buf, block, len);
				_pending_len = len;
				strncpy(_pending_ser, ser, CLUSTER_SER_LEN - 1);
				_pending_ser[CLUSTER_SER_LEN - 1] = '\0';
			}
			return;
		}
		// Claim confirmed — assign a local CDC slot
		slot = assign_slot(ser);
		if (slot < 0) {
			cluster_release_ser(ser);
			return;
		}
	}

	_slots[slot].last_used_ms = to_ms_since_boot(get_absolute_time());
	_slots[slot].blocks_sent++;
	write_to_cdc((uint8_t)slot, block, len);
}

// ── Public API ────────────────────────────────────────────────────────────────

void router_init(void) {
	memset(_slots,     0, sizeof(_slots));
	memset(_block_buf, 0, sizeof(_block_buf));
	_block_len      = 0;
	_prev_byte      = 0;
	_pending_ser[0] = '\0';
	_pending_len    = 0;
}

void router_feed(uint8_t byte) {
	// VE.Direct block boundary: two consecutive newlines (\r\n\r\n).
	if (byte == '\n' && _prev_byte == '\n') {
		if (_block_len + 2 <= ROUTER_MAX_BLOCK_BYTES) {
			_block_buf[_block_len++] = '\r';
			_block_buf[_block_len++] = '\n';
		}
		if (_block_len > 4) {
			dispatch_block(_block_buf, _block_len);
		}
		_block_len = 0;
		_prev_byte = 0;
		return;
	}

	// The cluster coordination frame marker (0xFF) signals an inter-block
	// gap. Discard it here; cluster_rx_feed() handles the full cluster frame.
	if (byte == 0xFF) {
		_prev_byte = byte;
		return;
	}

	if (_block_len < ROUTER_MAX_BLOCK_BYTES) {
		_block_buf[_block_len++] = byte;
	} else {
		_block_len = 0;  // overflow: discard and resync
	}
	_prev_byte = byte;
}

void router_task(void) {
	uint32_t now = to_ms_since_boot(get_absolute_time());

	// ── Resolve pending claim ─────────────────────────────────────────────────
	if (_pending_ser[0] != '\0' && cluster_i_own(_pending_ser)) {
		int slot = slot_for_ser(_pending_ser);
		if (slot < 0) slot = assign_slot(_pending_ser);
		if (slot >= 0 && _pending_len > 0) {
			write_to_cdc((uint8_t)slot, _pending_buf, _pending_len);
			_slots[slot].blocks_sent++;
			_slots[slot].last_used_ms = now;
		}
		_pending_ser[0] = '\0';
		_pending_len    = 0;
	}

	// ── Slot watchdog ─────────────────────────────────────────────────────────
	for (int i = 0; i < ROUTER_CDC_PORTS; i++) {
		if (_slots[i].ser[0] == '\0') continue;
		if ((now - _slots[i].last_used_ms) > ROUTER_SLOT_TIMEOUT_MS) {
			free_slot(i);
		}
	}

	// ── Discard all host->device data (read-only mode) ────────────────────────
	for (uint8_t i = 0; i < ROUTER_CDC_PORTS; i++) {
		if (tud_cdc_n_available(i)) {
			uint8_t discard[64];
			tud_cdc_n_read(i, discard, sizeof(discard));
		}
	}
}

router_slot_t const* router_slots(void) {
	return _slots;
}
