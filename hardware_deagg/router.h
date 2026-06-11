#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"
#include "cluster.h"

// ── Constants ─────────────────────────────────────────────────────────────────

// Maximum raw bytes in one VE.Direct text block.
// Observed maximum in the wild is ~220 bytes; 512 gives comfortable headroom.
#define ROUTER_MAX_BLOCK_BYTES  512

// How long a CDC slot may be unused before it is freed (milliseconds).
// Venus OS polls at ~1 Hz so 10 s is very conservative.
#define ROUTER_SLOT_TIMEOUT_MS  10000

// Number of CDC ports exposed by this node
#define ROUTER_CDC_PORTS        CFG_TUD_CDC

// ── Slot state ────────────────────────────────────────────────────────────────

typedef struct {
	char     ser[CLUSTER_SER_LEN]; // SER# this slot is serving (empty = free)
	uint32_t last_used_ms;         // timestamp of last block written to this slot
	uint32_t blocks_sent;          // diagnostic counter
} router_slot_t;

// ── Public API ────────────────────────────────────────────────────────────────

// Call once at startup.
void router_init(void);

// Feed a single byte from the RS485 stream.
// When a complete block is assembled the router will:
//   1. Extract the SER# field
//   2. Confirm or initiate a cluster claim for the SER#
//   3. Write the block to the corresponding CDC port once the claim is settled
void router_feed(uint8_t byte);

// Must be called from the main loop: handles slot timeouts, CDC TX flushes,
// and discards incoming host data from all CDC RX queues.
void router_task(void);

// Read-only view of all CDC slot states (for diagnostics).
router_slot_t const* router_slots(void);
