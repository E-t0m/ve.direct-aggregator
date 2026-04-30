# VE.Direct Aggregator — Readtext Stream Specification

**Output stream format of `readtext` and `readtext_sendhex` firmware · v1.0 · 2026**

---

## Overview

The readtext stream is the aggregated output of the VE.Direct Aggregator firmware. It is a plain sequential VE.Direct text stream — standard VE.Direct format, multiple devices multiplexed onto a single serial connection.

The receiving end identifies individual devices by their `PID` field. No separator characters, no packet markers, no block counts are used.

---

## Serial Parameters

| Parameter | Value |
|-----------|-------|
| Baud rate | 19200 (configurable via `BAUD_OUT`) |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |

---

## Stream Format

The stream consists of back-to-back VE.Direct text blocks. Each block is sent immediately when complete — one block at a time, no mixing.

### Block structure

```
PID\t0xA053\r\n
VPV\t18240\r\n
PPV\t120\r\n
V\t25600\r\n
I\t5200\r\n
CS\t3\r\n
MPPT\t2\r\n
ERR\t0\r\n
Checksum\tX\r\n
\r\n
```

Each block:
- starts with `PID\t<hex_id>\r\n`
- ends with `\r\n\r\n` (two consecutive `\n` characters)
- contains one field per line in `NAME\tVALUE\r\n` format
- ends with a `Checksum\t<byte>\r\n` line (checksum byte, not printable)

### Multi-device stream example

```
PID\t0xA053\r\n    ┐
V\t25600\r\n       │ device 1
...\r\n\r\n        ┘
PID\t0xA060\r\n    ┐
V\t25580\r\n       │ device 2
...\r\n\r\n        ┘
PID\t0x204\r\n     ┐
V\t25590\r\n       │ device 3 (e.g. BMV)
...\r\n\r\n        ┘
PID\t0xA053\r\n    ┐
V\t25620\r\n       │ device 1 — next cycle
...\r\n\r\n        ┘
```

---

## Device Identification

Each device is uniquely identified by its `PID` field. The PID is a hex string assigned by Victron to each product family.

| PID | Device |
|-----|--------|
| `0xA040`–`0xA05F` | BlueSolar MPPT |
| `0xA060`–`0xA07F` | SmartSolar MPPT |
| `0x0203`–`0x0205` | BMV-600 series |
| `0x0300`–`0x0302` | BMV-700 series |
| `0xA381` | SmartShunt |
| `0xA290`–`0xA2B0` | Phoenix Inverter |

A complete PID list is available in the Victron VE.Direct protocol documentation.

---

## Field Reference

Common fields across VE.Direct devices. Units as received from device — see conversion notes below.

### MPPT Solar Charger

| Field | Unit | Description |
|-------|------|-------------|
| `PID` | — | Product ID (hex string) |
| `FW` | — | Firmware version |
| `SER#` | — | Serial number |
| `V` | mV | Battery voltage |
| `I` | mA | Battery current |
| `VPV` | mV | PV input voltage |
| `PPV` | W | PV input power |
| `CS` | — | Charge state (0=Off, 2=Fault, 3=Bulk, 4=Absorb, 5=Float) |
| `MPPT` | — | MPPT state (0=Off, 1=Limited, 2=Active) |
| `ERR` | — | Error code (0=No error) |
| `LOAD` | — | Load output state (ON/OFF) |
| `IL` | mA | Load current |
| `H1`–`H23` | Wh/W | Historical data |
| `HSDS` | — | Day sequence number |

### BMV Battery Monitor / SmartShunt

| Field | Unit | Description |
|-------|------|-------------|
| `V` | mV | Main battery voltage |
| `VS` | mV | Aux/starter battery voltage |
| `VM` | mV | Mid-point voltage |
| `DM` | ‰ | Mid-point deviation |
| `I` | mA | Battery current |
| `T` | °C | Battery temperature |
| `P` | W | Instantaneous power |
| `CE` | mAh | Consumed Ah |
| `SOC` | ‰ | State of charge |
| `TTG` | min | Time to go |
| `Alarm` | — | Alarm (ON/OFF) |
| `Relay` | — | Relay state (ON/OFF) |
| `AR` | — | Alarm reason (bitmask) |
| `OR` | — | Off reason (bitmask) |
| `H1`–`H18` | mAh/W | Historical data |

### Phoenix Inverter

| Field | Unit | Description |
|-------|------|-------------|
| `V` | mV | Battery voltage |
| `AC_OUT_V` | 0.01V | AC output voltage |
| `AC_OUT_I` | 0.1A | AC output current |
| `AC_OUT_S` | VA | AC output apparent power |
| `WARN` | — | Warning reason |
| `CS` | — | State |

---

## Unit Conventions

VE.Direct transmits values in scaled integer units. The `ve_aggregator` Python module converts automatically on parse:

| Raw unit | Converted to | Fields |
|----------|-------------|--------|
| mV | V (float) | `V`, `VS`, `VM`, `VPV` |
| mA | A (float) | `I`, `IL` |
| mAh | Ah (float) | `CE` |
| W | W (int) | `PPV`, `P` |
| ‰ | ‰ (int) | `SOC`, `DM` |
| — | string | `PID`, `SER#`, `FW`, `CS`, `ERR`, etc. |

---

## Timing

VE.Direct devices transmit once per second. The aggregator sends each block immediately when complete — the inter-block gap on the stream depends on how many devices are connected and their relative timing.

At 19200 baud a typical block (~200 bytes) takes ~83 ms to transmit. With N devices the stream utilisation is approximately `N × 83 ms` per second.

| Devices | Stream utilisation |
|---------|--------------------|
| 1 | ~8 % |
| 3 | ~25 % |
| 7 | ~58 % |
| 9 | ~75 % |

---

## Receiver Compatibility

| Receiver | Compatible | Notes |
|----------|-----------|-------|
| Any VE.Direct text parser | ✓ | Standard format |
| `ve_aggregator` Python module | ✓ | Full support, auto unit conversion |
| Cerbo GX / Venus GX | ✗ | Expects one device per port |

---

## Checksum

Each VE.Direct block includes a `Checksum` field. The checksum byte is chosen such that the sum of all bytes in the block (including the checksum byte itself) equals 0 modulo 256. The `Checksum` line is the last line of each block.

Parsers that do not validate the checksum can ignore the `Checksum` line.

---

## Limitations

- The stream is read-only — no commands or responses are part of the readtext stream
- For bidirectional communication (SET/HEX) use the `readtext_sendhex` firmware and refer to the Powerset Interface Specification
- Cerbo GX / Venus GX cannot be used as direct receiver
