# VE.Direct Aggregator — Technical Specification

**Firmware · Arduino Mega 2560 / Teensy 4.1 · v1.6 · 2026**

---

## Overview

The VE.Direct Aggregator reads multiple Victron VE.Direct devices simultaneously and merges their data streams into a single serial output. The output is a plain sequential VE.Direct text stream — standard format, multiple devices on one connection.

**Supported devices:** any device using VE.Direct text protocol — MPPT solar chargers, BMV battery monitors, SmartShunt, Phoenix inverters, Blue Smart chargers, Orion DC/DC converters.

**Power control** (`readtext_sendhex` only): a bidirectional command channel allows setting the maximum charge power of MPPT chargers. Commands are routed through cascades, replies are returned.

**Intended receiver:** a Linux system (e.g. Raspberry Pi) with a parser that identifies devices by `PID` + `SER#`. The output is **not compatible** with Cerbo GX / Venus GX — these expect one device per VE.Direct port.

---

## Firmware Variants

| File | Hardware | Inputs | Output | Features |
|------|----------|--------|--------|----------|
| `vedirect_readtext.ino` | Mega 2560 | 3 | TX0 / USB | Text aggregation |
| `vedirect_readtext_sendhex.ino` | Mega 2560 | 3 | TX0 / USB | Text + SET + HEX |
| `vedirect_readtext_teensy41.ino` | Teensy 4.1 | 7 | TX8 or USB | Text aggregation |
| `vedirect_readtext_sendhex_teensy41.ino` | Teensy 4.1 | 7 | TX8 or USB | Text + SET + HEX |

All ports on all variants are treated identically — direct devices and upstream aggregators both send VE.Direct blocks and are handled the same way.

**Mega TX0 / USB:** Serial0 TX (pin 1) and the USB port share the same chip — either connection works with identical firmware. USB cable → host sees `/dev/ttyUSB0` or `/dev/ttyACM0`.

**Teensy output:** selected by `#define OUTPUT_USB` in firmware header:
```c
#define OUTPUT_USB 0   // TX8 pin
#define OUTPUT_USB 1   // SerialUSB native USB → /dev/ttyACM0
```

**Using `readtext_sendhex` everywhere:** since it is a superset of `readtext` — the SET/HEX channel does nothing when unused — it can be flashed on all MCUs without functional difference.

---

## Baud Rate Constants

| Constant | Value | Usage |
|----------|-------|-------|
| `BAUD_VEDIRECT` | 19200 | All direct VE.Direct devices — fixed by Victron |
| `BAUD_UPSTREAM` | 115200 | MCU-to-MCU links in cascade topology |
| `BAUD_OUT` | 19200 | Output — set to `BAUD_UPSTREAM` for cascade output |

Individual input ports are configured via `port_baud[]`:
```c
// Port 3 receives from an upstream MCU at 115200 baud:
uint32_t port_baud[3] = {BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_UPSTREAM};
```

---

## Output Format

Plain sequential VE.Direct text stream. Each block sent immediately when the `Checksum\t` line is received — one block at a time, no mixing.

```
PID\t0xA060\r\n
FW\t174\r\n
SER#\tHQ2529K6QK4\r\n
V\t52010\r\n
...
Checksum\t<byte>\r\n
PID\t0xA060\r\n       ← next block starts immediately
...
```

**Block end:** `Checksum\t` line — not double `\n`. There is no separator between blocks.

**ALIVE signal:** `ALIVE\r\n` is sent if no block has been sent for `ALIVE_TIMEOUT` ms (default 10s). Signals that the MCU is running but no device data is available.

**Device identification:** receivers must use `PID` + `SER#` combined. Multiple devices of the same type share the same `PID`.

**After sending** each block, the firmware discards any bytes that arrived in the hardware UART buffer during transmission — preventing partial blocks from being re-read as new data.

### Receiver compatibility

| Receiver | Compatible | Notes |
|----------|-----------|-------|
| `ve_aggregator` Python module | ✓ | Full support |
| Any VE.Direct text parser | ✓ | Standard format, ignores ALIVE |
| Cerbo GX / Venus GX | ✗ | Expects one device per port |

---

## Features

### Multi-channel input

All hardware UARTs polled simultaneously. Each port buffers one complete block independently (`BUF_SIZE` = 300 bytes). A slow or absent device does not block others.

### Block detection

Block end is detected by tracking the start of each line (`line_start[]` array). When `\n` is received, the current line is compared to `"Checksum\t"`. This correctly handles `\r\n` line endings without any special `\r` stripping.

### Cascadability

MCUs can be connected in star or cascade topology without code changes. MCUs with no devices act as transparent relays. The only limit is timing relative to the 1-second transmit interval.

### Power control (readtext_sendhex)

SET commands received on output RX are translated to VE.Direct HEX and sent to the matching MPPT charger. Only the affected port pauses (~50–100 ms). All other ports continue normally.

### PID routing (readtext_sendhex)

Each MCU learns which port each device PID is reachable on from passing blocks (`MAX_ROUTES` = 12 per port). Known PIDs are routed directly. Unknown PIDs are forwarded on all ports. PIDs expire after `PID_TIMEOUT` ms (default 10s).

---

## Device Compatibility

| Device | Text stream | HEX passthrough | SET command |
|--------|-------------|-----------------|-------------|
| MPPT Solar Charger | ✓ | ✓ full | ✓ |
| BMV Battery Monitor | ✓ | ✓ read/write | — |
| SmartShunt | ✓ | ✓ read | — |
| Phoenix Inverter | ✓ | ✓ on/off, mode | — |
| Blue Smart Charger | ✓ | ✓ mode, current | — |
| Orion DC/DC Converter | ✓ | ✓ on/off | — |

---

## Supported Topologies

### Direct

```
Device 1 ──► [Mega] RX1 ──► output      Device 1 ──► [Teensy] RX1
Device 2 ──► [Mega] RX2                 ...
Device 3 ──► [Mega] RX3                 Device 7 ──► [Teensy] RX7 ──► output
```

### Star

```
Device 1─3 ──► [Mega] ──┐
Device 4─6 ──► [Mega] ──┼──► [Mega] ──► output
Device 7─9 ──► [Mega] ──┘
```

With `readtext_sendhex` the connections are bidirectional — SET commands forward, replies return.

### Cascade (115200 baud output)

```
Device 1─3 ──► [Mega] ──► [Mega] ──► [Mega] ──► [Mega] ──► output
                3            6            9           12 blocks
```

### Mixed topology

Mega and Teensy can be freely mixed. Signal level note: Mega output (5V) → Teensy input requires BSS138 level shifter. Teensy output (3.3V) → Mega input needs no shifter (Mega RX is 5V-tolerant).

```
Device 1─7  ──► [Teensy] ──┐
Device 8─10 ──► [Mega]   ──┼──► [Mega] ──► output  (115200 baud)
Device 11─13──► [Mega]   ──┘
```

---

## Timing Budget

### Output at 19200 baud

| Devices | Time | Utilisation | Headroom |
|---------|------|-------------|----------|
| 1 | ~83 ms | 8 % | 917 ms |
| 3 | ~249 ms | 25 % | 751 ms |
| 7 | ~581 ms | 58 % | 419 ms |
| 9 | ~747 ms | 75 % | 253 ms |
| 12 | ~996 ms | ~100 % ⚠ | — |

### Output at 115200 baud

| Devices | Time | Utilisation | Headroom |
|---------|------|-------------|----------|
| 9 | ~126 ms | 13 % | 874 ms |
| 13 | ~182 ms | 18 % | 818 ms |
| 21 | ~294 ms | 29 % | 706 ms |
| 49 | ~686 ms | 69 % | 314 ms |

### Optimal topology for maximum device count

| Baud out | Topology | MCUs | Max. devices |
|----------|----------|------|--------------|
| 19200 | Mega direct | 1 | 3 |
| 19200 | Teensy direct | 1 | 7 |
| 19200 | Mega star | 4 | 9 |
| 115200 | Mega cascade | 4 | 12 |
| 115200 | Mixed star (1× Teensy + 2× Mega → 1 central) | 4 | 13 |
| 115200 | Teensy star (3× Teensy → 1 central) | 4 | 21 |
| 115200 | Teensy 2-level star (7× Teensy → 1 central) | 8 | 49 |

---

## Hardware & Logic Levels

| Platform | Logic level | Level shifter |
|----------|-------------|---------------|
| Arduino Mega 2560 | 5V | Not needed |
| Teensy 4.1 | 3.3V | BSS138 per RX input (+ TX for readtext_sendhex) |

Recommended: BSS138-based bidirectional 4-channel module. One module covers 4 RX inputs.

---

## Pin Assignment

### Arduino Mega 2560 — all variants

| Pin | Signal | Description |
|-----|--------|-------------|
| RX1 (19) | Device / Upstream 1 TX | Serial1 |
| RX2 (17) | Device / Upstream 2 TX | Serial2 |
| RX3 (15) | Device / Upstream 3 TX | Serial3 |
| TX0 (1) | Output | Serial0 — also accessible via USB |
| GND | Ground | common ground |
| 5V | Power | external DC/DC or USB |

### Arduino Mega 2560 — readtext_sendhex additions

| Pin | Signal | Description |
|-----|--------|-------------|
| TX1 (18) | Device 1 RX | HEX commands |
| TX2 (16) | Device 2 RX | HEX commands |
| TX3 (14) | Device 3 RX | HEX commands |
| RX0 (0) | Command input | SET/HEX from host |

### Teensy 4.1 — readtext variant

| Pin | Signal | Description |
|-----|--------|-------------|
| RX1–RX7 | Device 1–7 TX | Serial1–7 ⚠ BSS138 required |
| TX8 / USB | Output | Serial8 or SerialUSB |
| GND | Ground | common ground |

### Teensy 4.1 — readtext_sendhex additions

| Pin | Signal | Description |
|-----|--------|-------------|
| TX1–TX7 | Device 1–7 RX | HEX commands ⚠ BSS138 required |
| RX8 | Command input | SET/HEX from host |

### VE.Direct Connector (JST-PH 2 mm, 4-pin)

| Pin | Signal | Connection |
|-----|--------|------------|
| 1 | GND | MCU GND |
| 2 | RX (device input) | MCU TX — readtext_sendhex only |
| 3 | TX (device output) | MCU RX (via BSS138 on Teensy) |
| 4 | +5V | max 10 mA — not for MCU power |

---

## Power Supply

VE.Direct pin 4 supplies max 10 mA — not suitable for MCU power (~100 mA needed).

Power the MCU via:
- DC/DC converter from battery (8–60V input, 5V output)
- 5V USB power supply
- USB cable from host

---

## Technical Data

| Parameter | Mega 2560 | Teensy 4.1 |
|-----------|-----------|------------|
| Clock | 16 MHz | 600 MHz |
| Logic level | 5V | 3.3V |
| Level shifter | No | Yes — BSS138 per RX (+ TX for sendhex) |
| Hardware UARTs | 4 | 8 |
| Max. device inputs | 3 | 7 |
| Input baud | `BAUD_VEDIRECT` (19200) | `BAUD_VEDIRECT` (19200) |
| Output baud | `BAUD_OUT` (configurable) | `BAUD_OUT` (configurable) |
| Output connection | TX0 or USB (same chip) | TX8 or SerialUSB (`OUTPUT_USB`) |
| Block detection | `Checksum\t` line | `Checksum\t` line |
| Device key | `PID:SER#` (Python) | `PID:SER#` (Python) |
| ALIVE signal | `ALIVE\r\n` after `ALIVE_TIMEOUT` | `ALIVE\r\n` after `ALIVE_TIMEOUT` |
| HEX busy scope | per port | per port |
| SET channel | readtext_sendhex only | readtext_sendhex only |
| `VBAT_FALLBACK` | configurable (default 24V) | configurable (default 24V) |
| `PID_TIMEOUT` | configurable (default 10s) | configurable (default 10s) |
| `MAX_ROUTES` | 12 per port | 12 per port |
| `ALIVE_TIMEOUT` | 10s | 10s |
| Buffer size | 300 bytes per port | 300 bytes per port |
| CPU load (text only) | < 1 % at 3 devices | < 0.1 % at 7 devices |
| Text latency | < 1 block period (~83 ms) | < 1 block period (~83 ms) |

---

## Limitations

- Input port baud rates configurable per port via `port_baud[]` — direct VE.Direct devices use `BAUD_VEDIRECT` (19200), MCU-to-MCU cascade links use `BAUD_UPSTREAM` (115200)
- During SET/HEX only the affected device port pauses — others continue normally
- `VBAT_FALLBACK` used until first Vbat received — SET commands in first seconds may be slightly inaccurate
- VE.Direct pin 4 max 10 mA — MCU must be powered externally
- Cerbo GX / Venus GX cannot be used as direct receiver
- Multiple devices of the same type share the same PID — receivers must use `PID` + `SER#` for unique identification
- Teensy 4.1: BSS138 level shifters required on all RX inputs and TX outputs (readtext_sendhex)
- PIDs expire after `PID_TIMEOUT` ms — device swaps detected within ~1s
