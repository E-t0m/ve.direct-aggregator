#include "tusb.h"
#include "usb_descriptors.h"
#include "pico/unique_id.h"
#include <stdio.h>
#include <string.h>

// ── Device descriptor ─────────────────────────────────────────────────────────
// Presents as a generic CDC composite device.
// VID/PID: use Raspberry Pi VID with a custom PID that won't clash with
// official Pico firmware. Change to your own VID/PID for production.

tusb_desc_device_t const desc_device = {
	.bLength            = sizeof(tusb_desc_device_t),
	.bDescriptorType    = TUSB_DESC_DEVICE,
	.bcdUSB             = 0x0200,          // USB 2.0
	.bDeviceClass       = TUSB_CLASS_MISC,
	.bDeviceSubClass    = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol    = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor           = 0x2E8A,          // Raspberry Pi
	.idProduct          = 0x000A,          // composite CDC (unofficial)
	.bcdDevice          = 0x0100,
	.iManufacturer      = STR_IDX_MANUF,
	.iProduct           = STR_IDX_PRODUCT,
	.iSerialNumber      = STR_IDX_SERIAL,
	.bNumConfigurations = 1,
};

uint8_t const* tud_descriptor_device_cb(void) {
	return (uint8_t const*)&desc_device;
}

// ── Configuration descriptor ──────────────────────────────────────────────────
// One configuration with 7 CDC interfaces, each wrapped in an IAD.
// TUD_CDC_DESCRIPTOR macro expands to IAD + comm iface + data iface.
// Endpoint numbering: each CDC port gets its own endpoint pair so they
// are truly independent and one slow host read cannot stall another port.

uint8_t const desc_configuration[] = {
	// Config header: total length, 14 interfaces (2 per CDC × 7), 1 config,
	// no string, bus-powered, 100 mA
	TUD_CONFIG_DESCRIPTOR(1, CFG_TUD_CDC * 2, 0, CONFIG_TOTAL_LEN,
	                      TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

	// CDC 0 — interfaces 0+1, endpoint addresses 0x81 (notif IN),
	//          0x82 (bulk IN), 0x02 (bulk OUT)
	TUD_CDC_DESCRIPTOR(0, STR_IDX_CDC0, 0x81, 8, 0x02, 0x82, 64),

	// CDC 1 — interfaces 2+3
	TUD_CDC_DESCRIPTOR(2, STR_IDX_CDC1, 0x83, 8, 0x04, 0x84, 64),

	// CDC 2 — interfaces 4+5
	TUD_CDC_DESCRIPTOR(4, STR_IDX_CDC2, 0x85, 8, 0x06, 0x86, 64),

	// CDC 3 — interfaces 6+7
	TUD_CDC_DESCRIPTOR(6, STR_IDX_CDC3, 0x87, 8, 0x08, 0x88, 64),

	// CDC 4 — interfaces 8+9
	TUD_CDC_DESCRIPTOR(8, STR_IDX_CDC4, 0x89, 8, 0x0A, 0x8A, 64),

	// CDC 5 — interfaces 10+11
	TUD_CDC_DESCRIPTOR(10, STR_IDX_CDC5, 0x8B, 8, 0x0C, 0x8C, 64),

	// CDC 6 — interfaces 12+13
	TUD_CDC_DESCRIPTOR(12, STR_IDX_CDC6, 0x8D, 8, 0x0E, 0x8E, 64),
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
	(void)index;
	return desc_configuration;
}

// ── String descriptors ────────────────────────────────────────────────────────

// Serial number is built at runtime from the RP2040 flash unique ID.
// Stored here after init_usb_serial() is called from main().
static char serial_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

// iInterface strings give Venus OS / udev a stable name per port.
// Format: VEDIRECT-<NodeID>-<SlotIndex>
// The node-ID suffix lets udev rules distinguish ports across Picos in a
// cluster even when all Picos share the same VID/PID.
// NODE_ID is injected at compile time via CMakeLists.txt.
#ifndef NODE_ID
	#define NODE_ID 0
#endif

#define _STR(x) #x
#define STR(x)  _STR(x)

char const *string_desc_arr[] = {
	(const char[]){0x09, 0x04},         // 0: English (0x0409)
	"Victron De-Aggregator",            // 1: Manufacturer
	"VEDirect Cluster Node " STR(NODE_ID), // 2: Product
	serial_str,                         // 3: Serial (filled at runtime)
	"VEDIRECT-" STR(NODE_ID) "-0",      // 4: CDC0
	"VEDIRECT-" STR(NODE_ID) "-1",      // 5: CDC1
	"VEDIRECT-" STR(NODE_ID) "-2",      // 6: CDC2
	"VEDIRECT-" STR(NODE_ID) "-3",      // 7: CDC3
	"VEDIRECT-" STR(NODE_ID) "-4",      // 8: CDC4
	"VEDIRECT-" STR(NODE_ID) "-5",      // 9: CDC5
	"VEDIRECT-" STR(NODE_ID) "-6",      // 10: CDC6
};

void init_usb_serial(void) {
	pico_unique_board_id_t id;
	pico_get_unique_board_id(&id);
	for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
		snprintf(serial_str + i * 2, 3, "%02X", id.id[i]);
	}
}

// TinyUSB string descriptor callback — called by USB stack on each GET_DESCRIPTOR
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
	(void)langid;

	// Buffer for one UTF-16LE string (max 31 chars + 2-byte header)
	static uint16_t desc_str[32];
	uint8_t chr_count;

	if (index == 0) {
		// Language ID descriptor
		memcpy(&desc_str[1], string_desc_arr[0], 2);
		chr_count = 1;
	} else {
		uint8_t n = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
		if (index >= n) return NULL;

		const char *str = string_desc_arr[index];
		chr_count = (uint8_t)strlen(str);
		if (chr_count > 31) chr_count = 31;

		// Convert ASCII to UTF-16LE
		for (uint8_t i = 0; i < chr_count; i++) {
			desc_str[1 + i] = str[i];
		}
	}

	// Header: length in bytes (2 per char + 2 for header), type = STRING
	desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
	return desc_str;
}
