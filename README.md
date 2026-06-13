# VE.Direct Aggregator — Technical Specification

**Firmware · Arduino Mega 2560 / Teensy 4.1 · v2.0 · 2026**

---

## Overview

The VE.Direct Aggregator reads multiple Victron VE.Direct devices simultaneously and merges their data streams into a single serial output. The output is a plain sequential VE.Direct text stream — standard format, multiple devices on one connection.

**Supported devices:** any device using VE.Direct text protocol — MPPT solar chargers, BMV battery monitors, SmartShunt, Phoenix inverters, Blue Smart chargers, Orion DC/DC converters.

**Power control** (`readtext_sendhex` only): a bidirectional command channel allows setting the maximum charge power of MPPT chargers. Commands are routed through cascades, replies are returned.

**Intended receiver:** a Linux system (e.g. Raspberry Pi) with a parser that identifies devices by `SER#`. The output is **not directly compatible** with Cerbo GX / Venus GX (these expect one device per VE.Direct port) -- but usable via `vedirect_deaggregator.py`.

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

**Device identification:** receivers must use `SER#` combined. Multiple devices of the same type share the same `PID`.

**TX queue:** completed blocks are immediately moved to a circular TX queue (12 slots × 300 bytes). The receive buffer is freed instantly so the next block can be read while the previous one is still being sent. Blocks are silently dropped only if the queue is full (extremely unlikely at N≤7 devices).

### Receiver compatibility

| Receiver | Compatible | Notes |
|----------|-----------|-------|
| `ve_aggregator` Python module | ✓ | Full support |
| Any VE.Direct text parser | ✓ | Standard format, ignores ALIVE |
| Cerbo GX / Venus GX | via Deagg. | `vedirect_deaggregator.py` required |

---

## Features

### Multi-channel input

All hardware UARTs polled simultaneously. Each port buffers one complete block independently (`BUF_SIZE` = 300 bytes). A slow or absent device does not block others.

### Block detection

Block end is detected by tracking the start of each line. When `\n` is received, the current line is compared to `"Checksum\t"`. This correctly handles `\r\n` line endings without any special `\r` stripping.

A compile-time check ensures `BUF_SIZE` does not exceed `SERIAL_RX_BUFFER_SIZE`:
```c
#if BUF_SIZE > SERIAL_RX_BUFFER_SIZE
#error "BUF_SIZE exceeds SERIAL_RX_BUFFER_SIZE"
#endif
```

### Cascadability

MCUs can be connected in star or cascade topology without code changes. MCUs with no devices act as transparent relays. The only limit is timing relative to the 1-second transmit interval.

### Power control (readtext_sendhex)

SET commands received on output RX are translated to VE.Direct HEX and sent to the matching MPPT charger. Only the affected port pauses (~50–100 ms). All other ports continue normally.

### SER# routing (readtext_sendhex)

Each MCU learns the SER# and PID of each directly connected device from passing blocks. Commands are matched by SER# first, then by PID — this correctly distinguishes devices that share the same PID. For upstream/cascade devices, the firmware also maintains a route table (`MAX_ROUTES` = 12 per port) of PIDs seen arriving per port. Unknown identifiers are forwarded on all ports. Entries expire after `PID_TIMEOUT` ms (default 10s) and SER# is cleared simultaneously.

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
| Device key | `SER#` (Python) | `SER#` (Python) |
| ALIVE signal | `ALIVE\r\n` after `ALIVE_TIMEOUT` | `ALIVE\r\n` after `ALIVE_TIMEOUT` |
| HEX busy scope | per port | per port |
| HEX_TIMEOUT | 500 ms | 500 ms |
| Other ports during HEX | hardware buffer drained | hardware buffer drained |
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
- During SET/HEX only the affected device port pauses — all other ports are continuously drained during `wait_hex_reply` to prevent hardware buffer overflow
- `VBAT_FALLBACK` used until first Vbat received — SET commands in first seconds may be slightly inaccurate
- VE.Direct pin 4 max 10 mA — MCU must be powered externally
- Cerbo GX / Venus GX cannot be used as direct receiver -- but fully integrable via `vedirect_deaggregator.py`
- Multiple devices of the same type share the same PID — receivers must use `SER#` for unique identification
- Teensy 4.1: BSS138 level shifters required on all RX inputs and TX outputs (readtext_sendhex)
- PIDs expire after `PID_TIMEOUT` ms — device swaps detected within ~1s

---

## De-Aggregation and Venus OS / Cerbo GX

The aggregated stream can be split back into individual virtual serial ports
using `vedirect_deaggregator.py`. Each MPPT appears as its own `/dev/pts/N`
port, which Venus OS and Cerbo GX can register as separate VE.Direct devices.

See `deaggregator_spec.md` for setup details.


### RESET command

All firmware variants respond to `RESET\n`:

1. Forward `RESET\n` to all downstream ports (`forward_all`)
2. Wait 50ms for transmission to complete
3. Trigger watchdog reset -- MCU restarts within 15ms

This propagates through the entire cascade. Send from any host:

```bash
echo "RESET" > /dev/ttyACM3
# or via RS-485:
echo "RESET" > /dev/ttyUSB0
```

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

### Remote Firmware Update via RS-485

The Mega can be updated without removal from the installation:

1. Start `avrdude` on the RPi using the existing connection (USB or RS-485):

```bash
avrdude -p atmega2560 -c arduino -P /dev/ttyUSB0 -b 115200 \
        -U flash:w:firmware.hex:i
```

2. Press the physical Reset button on the Mega while `avrdude` is running

`avrdude` retries for ~10 seconds -- the Reset button can be pressed at any
point during that window. The bootloader starts and `avrdude` flashes the
new firmware automatically.

With the `RESET` command (readtext_sendhex firmware), step 3 can be
automated -- the RPi sends `RESET\n` to trigger the bootloader without
physical access.

---

## Hardware De-Aggregator (alternative approach)

For installations without a Linux host, the
[pico_vedirect](https://github.com/E-t0m/ve.direct-aggregator/tree/main/hardware_deagg) project
implements de-aggregation directly in hardware: one or more Raspberry Pi
Pico boards sit on the RS-485 bus carrying the aggregated stream, split
incoming blocks by `SER#`, and present each device as an independent
CDC-ACM serial port directly to the Cerbo GX via USB.

Multiple Picos self-organize on the same bus (cluster coordination via
idle-gap frames), scaling beyond the 7-port limit of a single Pico. This
is a read-only approach -- HEX commands from Venus OS are discarded, and
the VE.Direct driver falls back to text mode automatically.

Use this approach when no RPi/Linux host is available between the
aggregator and the Cerbo GX. Use `vedirect_deaggregator.py` (this repo)
when a Linux host is already in the chain.
