#pragma once

// ── TinyUSB device configuration ─────────────────────────────────────────────

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE

// Use the Pico's built-in USB hardware
#define CFG_TUSB_MCU            OPT_MCU_RP2040

// OS abstraction: none (bare metal)
#define CFG_TUSB_OS             OPT_OS_NONE

// Debug level: 0 = off
#define CFG_TUSB_DEBUG          0

// ── CDC configuration ─────────────────────────────────────────────────────────

// 7 CDC interfaces = maximum for USB Full Speed within endpoint limits.
// Each CDC port consumes:
//   - 1x Interrupt IN  (notifications, 8 bytes)
//   - 1x Bulk IN       (data to host)
//   - 1x Bulk OUT      (data from host, used for read-only discard)
// Total: 3 endpoints per port × 7 ports = 21 endpoints + EP0 = 22.
// USB FS allows 16 endpoint addresses per direction; shared across
// IN and OUT that gives us 15 usable pairs. 7 CDC ports fit comfortably.
#define CFG_TUD_CDC             7

// RX buffer per CDC port (host -> device, we discard but must buffer)
#define CFG_TUD_CDC_RX_BUFSIZE  64

// TX buffer per CDC port (device -> host, holds one full VE.Direct block)
// Largest VE.Direct block observed: ~200 bytes. 512 bytes = 2 blocks headroom.
#define CFG_TUD_CDC_TX_BUFSIZE  512

// ── Unused device classes ─────────────────────────────────────────────────────
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_MSC             0
#define CFG_TUD_VENDOR          0
#define CFG_TUD_NET             0
