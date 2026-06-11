#pragma once

#include "tusb.h"

// ── Endpoint numbers ──────────────────────────────────────────────────────────
// EP0 is reserved by USB. We assign one Interrupt IN + one Bulk IN/OUT pair
// per CDC port. Layout (endpoint address = number | direction bit):
//
//   EP1  IN  -> CDC0 notification
//   EP2  IN  -> CDC0 data TX
//   EP2  OUT -> CDC0 data RX
//   EP3  IN  -> CDC1 notification
//   EP4  IN  -> CDC1 data TX
//   EP4  OUT -> CDC1 data RX
//   ... and so on for CDC2..CDC6

// Total configuration descriptor length:
// 9 (config) + 7 * [8 (IAD) + 9 (comm iface) + 5 (header) + 5 (call mgmt)
//                   + 4 (ACM) + 5 (union) + 7 (notif EP)
//                   + 9 (data iface) + 7 (bulk IN EP) + 7 (bulk OUT EP)]
// = 9 + 7 * 66 = 9 + 462 = 471
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

// String descriptor indices
enum {
	STR_IDX_LANGID     = 0,
	STR_IDX_MANUF      = 1,
	STR_IDX_PRODUCT    = 2,
	STR_IDX_SERIAL     = 3,
	STR_IDX_CDC0       = 4,
	STR_IDX_CDC1       = 5,
	STR_IDX_CDC2       = 6,
	STR_IDX_CDC3       = 7,
	STR_IDX_CDC4       = 8,
	STR_IDX_CDC5       = 9,
	STR_IDX_CDC6       = 10,
};

// Provided to TinyUSB via callbacks defined in usb_descriptors.c
extern uint8_t const desc_configuration[];
extern char const   *string_desc_arr[];
