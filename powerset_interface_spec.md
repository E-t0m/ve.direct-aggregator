# VE.Direct Aggregator — Powerset Interface Specification

**Serial command interface of `readtext_sendhex` firmware · v2.0 · 2026**

---

## Overview

The powerset interface is a line-based serial protocol on the same port as the aggregated VE.Direct text stream. The host sends SET or HEX commands, the firmware replies per device.

The text stream uses `Checksum\t` as block end — no double newline, no separator between blocks. Only the port of the affected device is paused during a command (~50–100 ms typical). All other ports continue normally.

---

## Serial Parameters

| Parameter | Value |
|-----------|-------|
| Baud rate | `BAUD_OUT` (default `BAUD_VEDIRECT` = 19200) |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Line ending | `\n` |

---

## Commands

All commands are plain ASCII lines terminated with `\n`.

### SET — limit MPPT charge power by watts

```
SET <id> <watts>\n
SET ALL <watts>\n
```

| Field | Description |
|-------|-------------|
| `id` | Device SER#, e.g. `HQ2529K6QK4`, or `ALL` |
| `watts` | Target power limit in watts (integer ≥ 0). `0` stops charging. |

The firmware converts watts to amps using the last known `Vbat` from the text stream and writes register `0x2015` (Charge Current Limit, 0.1A units). After writing, the value is verified by re-reading the register. Text mode is **not** explicitly restored after commands — the device returns to text mode on its own, avoiding spurious HEX async-frame bursts.

`SET ALL` sends the HEX command to all direct MPPT devices simultaneously (pseudo-multicast), then verifies and replies one by one.

**Note:** SET is MPPT-specific. Register `0x2015` exists only on MPPT chargers. Sending SET to a non-MPPT ID forwards upstream or returns `ERR timeout`.

### HEX — send arbitrary VE.Direct HEX string

```
HEX <id> <hex_string>\n
HEX ALL <hex_string>\n
```

| Field | Description |
|-------|-------------|
| `id` | Device SER# or `ALL` |
| `hex_string` | Complete VE.Direct HEX line, e.g. `:154` |

The firmware passes the string verbatim — no parsing, no validation. The host is responsible for correct HEX formatting including checksum. The firmware does not restore text mode after HEX — devices return to text mode on their own.

Works with any VE.Direct device that supports the HEX protocol.

---

## Replies

One reply line per affected device, sent on the output TX. The reply identifier is SER#.

### SET — success

```
OK <id> <watts>W <amps>A\n
```

Example: `OK HQ2529K6QK4 500W 19.5A`

### SET — timeout

```
ERR <id> timeout\n
```

No HEX ACK within `HEX_TIMEOUT` (500 ms). Text mode restored regardless.

### SET — verify mismatch

```
ERR <id> verify set=<X>A rb=<Y>A\n
```

Register written but readback differs. Text mode restored regardless.

### HEX — reply

```
HEX_REPLY <id> :<hex_response>\n
```

Raw VE.Direct HEX response from the device, prefixed with device identifier.

### HEX — timeout

```
ERR <id> timeout\n
```

---

## Behaviour

**Watts-to-amps conversion:** `reg = round(watts / Vbat × 10)` — register unit 0.1A. `Vbat` is learned continuously from the text stream. Until first `Vbat` is received, `VBAT_FALLBACK` is used (default 24V).

**Device routing:** the firmware learns SER# for each directly connected device from passing blocks. Commands are matched by SER#. Unknown identifiers are forwarded on all ports (fallback). Device entries expire after `PID_TIMEOUT` ms (10s) of inactivity.

**Port isolation:** only the port of the affected device pauses during a command. During `wait_hex_reply`, all other ports are continuously drained (bytes discarded) to prevent hardware UART buffer overflow. This keeps the hardware buffers from filling during the `HEX_TIMEOUT` window.

**Text mode restore:** not explicitly performed. Devices return to text mode on their own after HEX commands, avoiding spurious async-frame bursts.

**hex_busy guarantee:** `hex_busy[idx]` is always cleared before returning from `exec_set` / `exec_hex`, including all timeout and error paths. No port can remain locked after a failed command.

**Cascade routing:** in a cascade topology, commands for unknown identifiers are forwarded on all ports. Upstream MCUs that know the device execute the command locally and return the reply.

---

## HEX Sequence (per device, SET command)

```
1.  send HEX SET  :8<reg_lo><reg_hi>00<val_lo><val_hi><cs>\n
2.  wait for ACK                              (400ms timeout → ERR timeout)
3.  send HEX GET  :7<reg_lo><reg_hi>00<cs>\n
4.  wait for GET reply                        (400ms timeout → ERR timeout)
5.  compare readback                          (mismatch → ERR verify)
6.  send OK / ERR on output
```

Typical duration: ~50–100 ms. Maximum: 2 x `HEX_TIMEOUT` = 800ms.

---

## Register Reference

| Register | Name | Unit | Notes |
|----------|------|------|-------|
| `0x2015` | Charge Current Limit | 0.1A | Volatile — unlimited write cycles. Used by SET. |
| `0xEDF0` | Charger Maximum Current | 0.1A | Non-volatile — limited write cycles. Avoid for frequent updates. |

---

## Timing

| Event | Duration |
|-------|----------|
| SET single device | ~50–100 ms typical, 1s max |
| SET ALL (N devices) | ~50–100 ms send, then N × verify sequentially |
| HEX single device | ~50–100 ms typical, 400ms max |
| Text stream gap (affected port only) | same as command duration |
| Other ports during command | hardware buffer drained — no data loss |

---

## Error Handling

| Condition | Firmware behaviour |
|-----------|-------------------|
| No HEX ACK within `HEX_TIMEOUT` (400ms) | `ERR <id> timeout`, text mode restored, `hex_busy` cleared |
| Verify mismatch | `ERR <id> verify`, text mode restored, `hex_busy` cleared |
| Unknown identifier | Command forwarded on all ports |
| Vbat not yet received | `VBAT_FALLBACK` used for conversion |
| HEX string without trailing `\n` | Firmware appends `\n` |

---

## De-Aggregation and Venus OS / Cerbo GX

The aggregated stream can be split back into individual virtual serial ports
using `vedirect_deaggregator.py`. Each MPPT appears as its own `/dev/pts/N`
port, which Venus OS and Cerbo GX can register as separate VE.Direct devices.

See `deaggregator_spec.md` for setup details.

---

## DS18B20 Temperature Sensor (optional)

One or more DS18B20 1-Wire sensors can be connected to a single digital pin
(`TEMP_PIN`, default D2). Each sensor is emitted as a pseudo VE.Direct block:

```
PID     0x9999
SER#    TEMP-P2-S0
FW      100
TEMP    23.50
Checksum  <byte>
```

The de-aggregator creates a virtual port for each sensor automatically.
Venus OS sees them as independent devices.

**Wiring (3-wire, any number of sensors on one pin):**
- VCC -> 5V
- GND -> GND
- DATA -> TEMP_PIN, with 4.7k pull-up resistor between 5V and DATA

**Configuration in firmware:**
```c
#define TEMP_ENABLE  1      // 0 = disabled
#define TEMP_PIN     2      // digital pin for 1-Wire DATA
#define TEMP_INTERVAL 5000  // readout interval in ms
```

No sensor connected -> `temp_count = 0` -> no blocks emitted, no overhead.

**Required libraries:** OneWire + DallasTemperature (Arduino Library Manager).
