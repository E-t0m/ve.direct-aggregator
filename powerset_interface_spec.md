# VE.Direct Aggregator — Powerset Interface Specification

**Serial command interface of `multiple_powerset` firmware · v1.0 · 2026**

---

## Overview

The powerset interface is a simple line-based serial protocol. The host sends commands, the firmware replies per charger. Commands and the aggregated VE.Direct text stream share the same serial line — they do not collide because the firmware only transmits text blocks when no command is in progress.

---

## Serial Parameters

| Parameter | Value |
|-----------|-------|
| Baud rate | 19200 (must match `BAUD_OUT` in firmware) |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Line ending | `\n` |

---

## Commands

All commands are plain ASCII lines terminated with `\n`.

### SET — limit charge power by watts

```
SET <pid> <watts>\n
SET ALL <watts>\n
```

| Field | Description |
|-------|-------------|
| `pid` | Charger PID as hex string, e.g. `0xA053`, or `ALL` |
| `watts` | Target power limit in watts (integer, ≥ 0). `0` stops charging. |

Examples:
```
SET 0xA053 500\n
SET ALL 1500\n
SET ALL 0\n
```

The firmware converts watts to amps using the last known `Vbat` from the text stream and writes register `0x2015` (Charge Current Limit, 0.1A units). After writing, the value is verified by re-reading the register.

`SET ALL` sends the HEX command to all direct chargers simultaneously (pseudo-multicast), then verifies and replies one by one.

### HEX — send arbitrary VE.Direct HEX string

```
HEX <pid> <hex_string>\n
HEX ALL <hex_string>\n
```

| Field | Description |
|-------|-------------|
| `pid` | Charger PID or `ALL` |
| `hex_string` | Complete VE.Direct HEX line, e.g. `:154\n` |

The firmware passes the HEX string verbatim to the charger — no parsing, no validation. The host is responsible for correct HEX formatting including checksum.

Examples:
```
HEX 0xA053 :154\n
HEX ALL :154\n
HEX 0xA053 :70015200A3\n
HEX ALL :8001520000A3\n
```

---

## Replies

The firmware sends one reply line per affected charger.

### SET — success

```
OK <pid> <watts>W <amps>A\n
```

| Field | Description |
|-------|-------------|
| `pid` | Charger PID |
| `watts` | Watt value as sent |
| `amps` | Verified register value in amps (0.1A resolution) |

Example:
```
OK 0xA053 500W 19.5A\n
```

### SET — timeout

```
ERR <pid> timeout\n
```

No HEX ACK received within 1s. Text mode is restored regardless.

### SET — verify mismatch

```
ERR <pid> verify set=<X>A rb=<Y>A\n
```

Register was written but readback does not match. Text mode is restored regardless.

Example:
```
ERR 0xA053 verify set=19.5A rb=18.2A\n
```

### HEX — reply

```
HEX_REPLY <pid> :<hex_response>\n
```

Raw VE.Direct HEX response from the charger, prefixed with PID.

Example:
```
HEX_REPLY 0xA053 :50015200641F\n
```

### HEX — timeout

```
ERR <pid> timeout\n
```

---

## Behaviour

**Watts-to-amps conversion** — performed by the firmware using the last known `Vbat` from the text stream. The host always works in watts. Formula: `reg = round(watts / Vbat × 10)` (register unit: 0.1A).

**Vbat fallback** — if no Vbat has been received yet (~first 2s after startup), the firmware assumes 24V. SET commands during this window may be slightly inaccurate.

**Text stream gap** — while a SET or HEX command is in progress, the affected charger stops sending text frames (~1–3s). This gap is the only indication a command is in progress — no separate notification is sent.

**Text mode restore** — after every SET command the firmware sends `:154\n` to the charger to restore text mode. For HEX commands the host is responsible for restoring text mode if needed (e.g. `HEX <pid> :154\n`).

**Cascade routing** — if a PID is not known locally, the command is forwarded to upstream MCUs. Replies from upstream are passed back transparently on the output line.

---

## Error Handling

| Condition | Firmware behaviour |
|-----------|--------------------|
| Charger timeout (no HEX ACK) | `ERR <pid> timeout`, text mode restored |
| Verify mismatch | `ERR <pid> verify set=XA rb=YA`, text mode restored |
| Unknown PID | Command forwarded upstream, no local reply |
| Vbat not yet known | Fallback 24V used for conversion |
| HEX string without trailing `\n` | Firmware appends `\n` before sending |

---

## Timing

| Event | Duration |
|-------|----------|
| SET command (single charger) | ~100–200 ms + HEX reply time |
| SET ALL (N chargers) | ~(100–200 ms) × N sequentially after multicast send |
| Text stream gap per charger | ~1–3s |
| HEX command (single charger) | ~50–100 ms + HEX reply time |
