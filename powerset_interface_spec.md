# VE.Direct Aggregator — Powerset Interface Specification

**Serial command interface of `readtext_sendhex` firmware · v1.0 · 2026**

---

## Overview

The powerset interface is a simple line-based serial protocol on the same port as the aggregated VE.Direct text stream. The host sends commands, the firmware replies per device.

The text stream is a plain sequential VE.Direct stream — blocks are sent immediately when complete, identified by their `PID` field. No separator characters are used. Block mixing is prevented by sending one complete block at a time.

Only the port of the affected device is paused during a command (~50–100 ms typical). All other ports continue reading and sending normally.

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
| `pid` | Device PID as hex string, e.g. `0xA053`, or `ALL` |
| `watts` | Target power limit in watts (integer ≥ 0). `0` stops charging. |

The firmware converts watts to amps using the last known `Vbat` and writes register `0x2015` (Charge Current Limit, 0.1A units). After writing, the value is verified by re-reading the register. The charger is then switched back to text mode automatically.

`SET ALL` sends the HEX command to all direct chargers simultaneously (pseudo-multicast), then verifies and replies one by one.

### HEX — send arbitrary VE.Direct HEX string

```
HEX <pid> <hex_string>\n
HEX ALL <hex_string>\n
```

| Field | Description |
|-------|-------------|
| `pid` | Device PID or `ALL` |
| `hex_string` | Complete VE.Direct HEX line, e.g. `:154\n` |

The firmware passes the string verbatim to the device — no parsing, no validation. The host is responsible for correct HEX formatting including checksum. Unlike `SET`, the firmware does **not** automatically restore text mode after a `HEX` command — the host must send `HEX <pid> :154\n` if needed.

---

## Replies

The firmware sends one reply line per affected device on the output TX.

### SET — success

```
OK <pid> <watts>W <amps>A\n
```

| Field | Description |
|-------|-------------|
| `pid` | Device PID |
| `watts` | Watt value as sent |
| `amps` | Verified register value in amps (0.1A resolution) |

Example: `OK 0xA053 500W 19.5A`

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

### HEX — reply

```
HEX_REPLY <pid> :<hex_response>\n
```

Raw VE.Direct HEX response from the device, prefixed with PID.

### HEX — timeout

```
ERR <pid> timeout\n
```

---

## Behaviour

**Watts-to-amps conversion:** performed by the firmware using the last known `Vbat`. The host always works in watts. Formula: `reg = round(watts / Vbat × 10)` (unit: 0.1A).

**Vbat fallback:** configurable via `VBAT_FALLBACK` (default 24V). Used until first `Vbat` value is received from the text stream (~first 2s after startup). SET commands during this window may be slightly inaccurate.

**Port isolation:** only the port of the affected device pauses during a command. All other ports continue reading and sending normally. This prevents hardware UART buffer overflow on unaffected ports.

**Text mode restore:** after every `SET` command the firmware sends `:154\n` to the charger automatically. For `HEX` commands the host is responsible.

**Cascade routing:** unknown PIDs are forwarded on the TX pins of upstream ports. Replies from upstream are passed back on the output TX transparently.

---

## HEX Sequence (per device)

```
1.  send HEX SET  :8<reg><flags><value><cs>\n
2.  wait for ACK                              (1s timeout → ERR timeout)
3.  send HEX GET  :7<reg><flags><cs>\n
4.  wait for GET reply                        (1s timeout → ERR timeout)
5.  compare readback                          (mismatch → ERR verify)
6.  send :154\n                               (restore text mode)
7.  send OK / ERR on output                   (do not wait for text resume)
```

Typical duration: ~50–100 ms. Maximum: 2× HEX_TIMEOUT (default 2s).

---

## Error Handling

| Condition | Firmware behaviour |
|-----------|--------------------|
| No HEX ACK within 1s | `ERR <pid> timeout`, text mode restored |
| Verify mismatch | `ERR <pid> verify`, text mode restored |
| Unknown PID | Command forwarded upstream, no local reply |
| Vbat not yet received | `VBAT_FALLBACK` used for conversion |
| HEX string without trailing `\n` | Firmware appends `\n` |

---

## Register Reference

| Register | Name | Unit | Notes |
|----------|------|------|-------|
| `0x2015` | Charge Current Limit | 0.1A | Volatile — unlimited write cycles. Used by SET. |
| `0xEDF0` | Charger Maximum Current | 0.1A | Non-volatile — limited write cycles. Avoid for frequent updates. |
| `0x154` (cmd) | Switch to text mode | — | Sent as `:154\n` after each SET. |

---

## Timing

| Event | Duration |
|-------|----------|
| SET single charger | ~50–100 ms typical, 2s max |
| SET ALL (N chargers) | ~50–100 ms send, then N × verify sequentially |
| HEX single device | ~50–100 ms typical, 1s max |
| Text stream gap (affected port only) | same as command duration |
| Other ports during command | unaffected — normal operation |
