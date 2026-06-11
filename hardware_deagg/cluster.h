#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hardware/uart.h"

// ── Constants ─────────────────────────────────────────────────────────────────

// Marker byte that opens every cluster frame. 0xFF never appears in a valid
// VE.Direct ASCII text block, so it is safe to use as a framing byte.
#define CLUSTER_MARKER        0xFF

// Frame type codes
#define CLUSTER_FRAME_STATUS  0x01  // periodic heartbeat: "I exist, here is my load"
#define CLUSTER_FRAME_CLAIM   0x02  // "I am taking ownership of this serial number"
#define CLUSTER_FRAME_RELEASE 0x03  // "I no longer own this serial number"

// Timing
#define CLUSTER_STATUS_INTERVAL_MS   500  // broadcast own status every 500 ms
#define CLUSTER_NODE_TIMEOUT_MS     3000  // node considered dead after 3 s silence
#define CLUSTER_CLAIM_DELAY_BASE_MS   10  // base delay before sending a claim;
                                          // actual delay = node_id * base so
                                          // lower IDs always win deterministically

// Maximum number of nodes in a cluster
#define CLUSTER_MAX_NODES     8

// Maximum number of distinct serial numbers we can track cluster-wide
// (21 from a fully loaded aggregator plus headroom)
#define CLUSTER_MAX_DEVS      32

// Maximum length of a VE.Direct SER# value including null terminator.
// Observed format: up to 12 alphanumeric characters, e.g. "HQ2242ABCDE"
#define CLUSTER_SER_LEN       13

// Sentinel: serial number slot not owned by any node
#define CLUSTER_SER_UNOWNED   0xFF

// ── Wire frame layout (16 bytes, packed) ─────────────────────────────────────
//
//  Byte  0:     CLUSTER_MARKER (0xFF)
//  Byte  1:     frame type (CLUSTER_FRAME_*)
//  Byte  2:     sender node_id (0-7)
//  Byte  3:     slots_used (0-7)
//  Byte  4:     slots_free (0-7)
//  Bytes 5-12:  ser[8] — first 8 bytes of SER# value, zero-padded
//               (0x00 for STATUS frames)
//  Byte 13:     reserved, set to 0
//  Byte 14:     reserved, set to 0
//  Byte 15:     crc8 over bytes 1-14
//
// Total: 16 bytes = ~8 ms at 19200 baud — fits in a 10 ms inter-block gap

#define CLUSTER_FRAME_LEN  16
#define CLUSTER_SER_WIRE   8   // bytes of SER# carried in the frame

typedef struct __attribute__((packed)) {
	uint8_t marker;                  // always CLUSTER_MARKER
	uint8_t type;                    // CLUSTER_FRAME_*
	uint8_t node_id;
	uint8_t slots_used;
	uint8_t slots_free;
	uint8_t ser[CLUSTER_SER_WIRE];   // first 8 bytes of SER# (0-padded)
	uint8_t reserved[2];
	uint8_t crc8;
} cluster_frame_t;

// ── Per-node state (maintained locally by every node) ─────────────────────────

typedef struct {
	bool     alive;            // has this node sent a status recently?
	uint8_t  slots_used;
	uint8_t  slots_free;
	uint32_t last_seen_ms;     // absolute timestamp of last STATUS frame
} node_status_t;

// ── Serial number ownership table ────────────────────────────────────────────
// Maps a SER# string to the node_id that claimed it, or CLUSTER_SER_UNOWNED.

typedef struct {
	char    ser[CLUSTER_SER_LEN];  // null-terminated SER# value
	uint8_t owner;                 // node_id or CLUSTER_SER_UNOWNED
} ser_entry_t;

// ── Public API ────────────────────────────────────────────────────────────────

// Call once at startup with the UART instance that drives the RS485 bus.
// node_id is read from GPIO pins (or compiled in via NODE_ID define).
void cluster_init(uart_inst_t *uart, uint8_t node_id, uint8_t slots_total);

// Must be called from the main loop; handles incoming frames and periodic
// status broadcasts. Returns true if a cluster frame was consumed.
bool cluster_task(void);

// Feed a single byte into the cluster frame parser (called from UART IRQ shim).
void cluster_rx_feed(uint8_t b);

// Called by the router when it sees a SER# for the first time.
// Returns true if this node should handle the device (claim was initiated).
// Returns false if another node already owns it or this node has no free slots.
// The caller must re-check cluster_i_own() on subsequent blocks until true.
bool cluster_claim_ser(const char *ser);

// Returns the node_id of the current owner of ser, or CLUSTER_SER_UNOWNED.
uint8_t cluster_ser_owner(const char *ser);

// Returns true if this node owns the given ser.
bool cluster_i_own(const char *ser);

// Call when a CDC slot is freed (device lost / watchdog expired).
void cluster_release_ser(const char *ser);

// Returns number of free CDC slots on this node.
uint8_t cluster_slots_free(void);

// Update slot counts (called by router after each assignment/release).
void cluster_update_slots(uint8_t used, uint8_t free);

// Read-only view of all known node and device states (for diagnostics).
node_status_t const* cluster_node_status(void);  // array[CLUSTER_MAX_NODES]
ser_entry_t   const* cluster_ser_table(void);     // array[CLUSTER_MAX_DEVS]
