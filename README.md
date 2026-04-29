# VE.Direct Aggregator — Technical Specification

**Firmware · Arduino Mega 2560 / Teensy 4.1 · v1.0 · 2026**

---

## Overview

The VE.Direct Aggregator is firmware for the Arduino Mega 2560 or Teensy 4.1 with two independent functional areas:

**Text aggregation:** Multiple Victron VE.Direct devices are read and merged into a single serial output — a plain sequential VE.Direct text stream. Any device that speaks the VE.Direct text protocol is supported without code changes, including MPPT solar chargers, BMV battery monitors, SmartShunt, Phoenix inverters, Blue Smart chargers and Orion DC/DC converters.

**Power control:** A bidirectional command channel allows setting the maximum charge power of individual or all MPPT chargers (`readtext_sendhex` only). The channel is integrated into all topologies — commands are routed through cascades, confirmations are returned. Only the port of the affected device pauses during a HEX command (~50–100 ms typical) — all other ports continue reading and sending unaffected.

**Intended receiver:** a Linux-based system (e.g. Raspberry Pi) with a parser that identifies devices by their `PID` field. The output is **not compatible** with Cerbo GX / Venus GX as a direct receiver — these expect exactly one device per VE.Direct port and ignore subsequent PIDs on the same port.

---

## Firmware Variants

Four variants are available depending on the required feature set and target hardware:

| File | Hardware | Inputs | Output | Features |
|------|----------|--------|--------|----------|
| `vedirect_readtext.ino` | Mega 2560 | 3 | TX0 / USB | Text aggregation |
| `vedirect_readtext_sendhex.ino` | Mega 2560 | 3 | TX0 / USB | Text aggregation + SET + HEX |
| `vedirect_readtext_teensy41.ino` | Teensy 4.1 | 7 | TX8 or USB | Text aggregation |
| `vedirect_readtext_sendhex_teensy41.ino` | Teensy 4.1 | 7 | TX8 or USB | Text aggregation + SET + HEX |

All ports on all variants are treated identically — direct VE.Direct devices and upstream aggregators both send valid VE.Direct blocks and are handled the same way.

**Mega TX0 / USB:** Serial0 TX (pin 1) and the USB port share the same chip (16U2 or CH340G) — either connection works with identical firmware. Simply plug in a USB cable for direct host connection, or wire TX0 externally for long cable runs. The host sees `/dev/ttyUSB0` or `/dev/ttyACM0`. `BAUD_OUT` must match on both sides.

**Teensy output selection:** controlled by `#define OUTPUT_USB` in the firmware header:
```c
#define OUTPUT_USB 0   // TX8 pin — serial output
#define OUTPUT_USB 1   // SerialUSB — native USB, host sees /dev/ttyACM0
```

**Using `readtext_sendhex` everywhere:** Since `readtext_sendhex` is a superset of `readtext` — the SET/HEX channel does nothing when unused — it can be flashed on all MCUs in a topology without functional difference. This simplifies firmware management at the cost of slightly higher code complexity.

---

## Features

### Multi-channel input

All hardware UARTs are polled simultaneously. Each channel buffers one complete VE.Direct block independently — a slow or temporarily absent device does not block the others. All ports are treated identically regardless of what is connected — direct VE.Direct devices or upstream aggregators.

### Serial output

Blocks are sent immediately when complete, one at a time — first ready port wins. If multiple ports finish simultaneously the others are queued. No block mixing is possible.

### Cascadability

Multiple aggregators can be connected in star or cascade topology without any code changes. MCUs with no devices attached act as transparent relays. The only practical limit is timing relative to the 1-second transmit interval of the devices.

### Power control via SET command

*(`readtext_sendhex` only)*

The MCU receives SET commands on the RX of its output serial port, translates them into VE.Direct HEX commands and sends them to the MPPT charger with the matching PID via the TX pin of that port. Only the affected port's text stream pauses (~50–100 ms typical). All other ports continue unaffected.

**Command format:**
```
SET <pid> <watts>\n     limit a single charger by PID
SET ALL <watts>\n       limit all chargers simultaneously
```

**Reply format:**
```
OK <pid> <watts>W <amps>A\n     setting verified by re-read
ERR <pid> timeout\n             no HEX ACK within 1s
ERR <pid> verify set=XA rb=YA\n readback mismatch
```

### Arbitrary HEX passthrough

*(`readtext_sendhex` only)*

The host can send any VE.Direct HEX string directly to a device by PID. The firmware passes it verbatim without parsing or validation. The reply is returned prefixed with the PID. The host is responsible for correct HEX formatting including checksum and for restoring text mode afterwards if needed (`HEX <pid> :154\n`).

**Command format:**
```
HEX <pid> <hex_string>\n    send to any direct VE.Direct device
HEX ALL <hex_string>\n      broadcast to all direct devices
```

**Reply format:**
```
HEX_REPLY <pid> :<hex_response>\n
ERR <pid> timeout\n
```

### Automatic watts-to-amps conversion

*(`readtext_sendhex` only)*

The MCU learns the current battery voltage (`Vbat`) from the live text stream and automatically converts the watt value of a SET command to amps with 0.1A resolution:

```
A    = watts / Vbat                  (float, e.g. 19.5 A)
reg  = round(A × 10)                 (register 0x2015, unit 0.1A)
```

Until the first Vbat value is received a configurable fallback voltage is used (`VBAT_FALLBACK`, default 24V).

### PID-based routing in cascades

*(`readtext_sendhex` only)*

Each MCU learns the PIDs of its directly connected devices from the text stream. Unknown PIDs are forwarded on the TX pins of upstream ports. Replies from upstream are passed back on the output TX.

---

## Device Compatibility

The aggregator is a universal VE.Direct stream aggregator — it buffers complete blocks and forwards them without inspecting field names or values. Any device that speaks VE.Direct text protocol works without code changes.

| Device | Text stream | HEX passthrough | SET command |
|--------|-------------|-----------------|-------------|
| MPPT Solar Charger | ✓ | ✓ full | ✓ |
| BMV Battery Monitor (600/700/702/712) | ✓ | ✓ read/write | — |
| SmartShunt | ✓ | ✓ read | — |
| Phoenix Inverter | ✓ | ✓ on/off, mode | — |
| Blue Smart Charger (AC) | ✓ | ✓ mode, current | — |
| Orion DC/DC Converter | ✓ | ✓ on/off | — |

**SET command** uses register `0x2015` (Charge Current Limit) — MPPT-specific. Sending `SET` to a non-MPPT PID forwards to upstream or returns `ERR timeout`.

Different device types can be connected simultaneously on different ports — the aggregator does not distinguish between them.

---

## Supported Topologies

### Direct — up to 3 devices on 1 Mega, or up to 7 on 1 Teensy

```
MPPT 1 ─TX──► [Mega/Teensy] RX1   ──► output
MPPT 2 ─TX──► [Mega/Teensy] RX2
MPPT 3 ─TX──► [Mega/Teensy] RX3
...
```

### Star — up to 9 devices, 3 Megas to 1 central Mega

```
MPPT 1─3 ──► [Mega] ──TTL──┐
MPPT 4─6 ──► [Mega] ──TTL──┼──► [Mega] ──► output
MPPT 7─9 ──► [Mega] ──TTL──┘
```

With `readtext_sendhex` each TTL connection is bidirectional — SET commands flow forward, OK/ERR replies flow back.

### Cascade — up to 12 devices at 115200 baud output

```
MPPT 1─3 ──► [Mega] ──► [Mega] ──► [Mega] ──► [Mega] ──► output
               3 blocks   6 blocks   9 blocks   12 blocks
```

### Relay cascade — empty MCUs as repeaters

```
MPPT 1─3 ──► [Mega] ──► [Mega empty] ──► [Mega empty] ──► output
```

MCUs with no devices attached pass all traffic through transparently. Useful when cable runs between MCUs exceed ~5m TTL range. No code changes required.

### Mixed topology

Mega and Teensy variants can be freely mixed. The protocol is hardware-agnostic — each MCU only sees bytes on its input pins.

```
MPPT 1─7  ──► [Teensy] ──TTL──┐
MPPT 8─10 ──► [Mega]   ──TTL──┼──► [Mega] ──► output
MPPT 11─13──► [Mega]   ──TTL──┘
```

**Signal level when mixing:** Mega operates at 5V TTL, Teensy at 3.3V. Mega output → Teensy input: BSS138 level shifter required. Teensy output → Mega input: no shifter needed (Mega RX is 5V tolerant).

**Note:** 13 devices × ~83 ms = ~1079 ms at 19200 baud — exceeds the 1-second window. Set `BAUD_OUT` to `115200` on the central Mega.

---

## Output Format

The aggregator outputs a plain sequential VE.Direct text stream. Each block is sent immediately when complete — no separators, no markers between blocks.

```
PID\t0xA053\r\n     ┐
VPV\t18240\r\n      │ block 1 — sent immediately when ready
PPV\t120\r\n        │
...\r\n             │
Checksum\tX\r\n     │
\r\n                ┘
PID\t0xA060\r\n     ┐
...\r\n\r\n         ┘ block 2 — sent as soon as block 1 done
```

Each block starts with `PID\t...` and ends with `\r\n\r\n`. The receiving end identifies devices by their `PID` field.

**No block mixing:** one complete block is sent at a time. If multiple ports finish simultaneously they are queued — first ready port wins.

### Receiver compatibility

| Receiver | Compatible | Notes |
|----------|-----------|-------|
| Any VE.Direct text parser | ✓ | Reads blocks sequentially by PID |
| Linux host with custom parser | ✓ | Full support |
| Cerbo GX / Venus GX | ✗ | Expects one device per port |

---

## SET Command Channel — HEX Sequence

*(`readtext_sendhex` only)*

```
1.  send HEX SET:   :8<reg_lo><reg_hi>00<val_lo><val_hi><cs>\n
2.  wait for HEX ACK                             (1s timeout → ERR timeout)
3.  send HEX GET:   :7<reg_lo><reg_hi>00<cs>\n
4.  wait for HEX GET reply                       (1s timeout → ERR timeout)
5.  check readback == sent value                 (mismatch → ERR verify)
6.  restore text mode: :154\n
7.  send OK/ERR on output TX
    — do NOT wait for text stream to resume
```

**Register used: `0x2015` — Charge Current Limit**

| Property    | Value |
|-------------|-------|
| Register    | `0x2015` |
| Unit        | 0.1A |
| Storage     | volatile — unlimited write cycles |
| Conversion  | `round(watts / Vbat × 10)` → register value |

`0x2015` is preferred over `0xEDF0` — the latter has limited write cycles.

Only the port of the affected device is paused during the HEX sequence (~50–100 ms typical, up to 1s on timeout). All other ports continue reading and sending normally.

For `SET ALL`: HEX SET is sent to all direct ports simultaneously (pseudo-multicast), then replies are verified one by one.

---

## Timing Budget

VE.Direct transmits once per second. A typical block (~200 bytes) occupies ~83 ms at 19200 baud, ~14 ms at 115200 baud.

### Output at 19200 baud

| Devices | Transmission time | Utilisation | Headroom |
|----------|------------------|-------------|----------|
| 1        | ~83 ms           | 8 %         | 917 ms   |
| 3        | ~249 ms          | 25 %        | 751 ms   |
| 6        | ~498 ms          | 50 %        | 502 ms   |
| 7        | ~581 ms          | 58 %        | 419 ms   |
| 9        | ~747 ms          | 75 %        | 253 ms   |
| 12       | ~996 ms          | ~100 % ⚠    | —        |

### Output at 115200 baud

| Devices | Transmission time | Utilisation | Headroom |
|----------|------------------|-------------|----------|
| 9        | ~126 ms          | 13 %        | 874 ms   |
| 13       | ~182 ms          | 18 %        | 818 ms   |
| 21       | ~294 ms          | 29 %        | 706 ms   |
| 53       | ~742 ms          | 74 %        | 258 ms   |

### Optimal topology for maximum device count

| Output baud | Topology | MCUs | Max. devices |
|-------------|----------|------|---------------|
| 19200       | Mega readtext | 1 | 3 |
| 19200       | Teensy readtext | 1 | 7 |
| 19200       | Mega star | 4 | 9 |
| 115200      | Mega cascade | 4 | 12 |
| 115200      | Mixed star (1× Teensy + 2× Mega → 1 central) | 4 | 13 |
| 115200      | Teensy star (3× Teensy → 1 central) | 4 | 21 |

---

## Hardware & Logic Levels

| Platform | Logic level | Level shifter needed |
|----------|-------------|----------------------|
| Arduino Mega 2560 | 5V | No — direct connection |
| Teensy 4.1 | 3.3V | Yes — BSS138 per RX input |

**Recommended:** BSS138-based bidirectional 4-channel module. Despite the "I2C" label these work for any signal up to ~1 MBit/s including UART at 19200 baud. One module covers 4 RX inputs. Two modules cover all 7 Teensy inputs. For `readtext_sendhex` TX pins also need level shifters (3.3V → 5V).

---

## Pin Assignment

### Arduino Mega 2560 — all variants

| Pin       | Signal                  | Description                    |
|-----------|-------------------------|--------------------------------|
| RX1 (19)  | Device / Upstream 1 TX | Serial1 — read text stream     |
| RX2 (17)  | Device / Upstream 2 TX | Serial2 — read text stream     |
| RX3 (15)  | Device / Upstream 3 TX | Serial3 — read text stream     |
| TX0 (1)   | Output                  | Serial0 — aggregated stream    |
| GND       | Ground all inputs       | common ground                  |
| 5V        | Power supply            | external DC/DC or USB          |

**USB direct:** Serial0 TX and the USB port share the same chip. Simply plug in USB — host sees `/dev/ttyUSB0` or `/dev/ttyACM0`. `BAUD_OUT` must match on both sides.

### Arduino Mega 2560 — `readtext_sendhex` additions

| Pin       | Signal                  | Description                         |
|-----------|-------------------------|-------------------------------------|
| TX1 (18)  | Device / Upstream 1 RX | Serial1 — HEX commands / SET fwd    |
| TX2 (16)  | Device / Upstream 2 RX | Serial2 — HEX commands / SET fwd    |
| TX3 (14)  | Device / Upstream 3 RX | Serial3 — HEX commands / SET fwd    |
| RX0 (0)   | Command input           | Serial0 — SET/HEX from host         |

### Teensy 4.1 — `readtext` variant

| Pin        | Signal         | Description                   |
|------------|----------------|-------------------------------|
| RX1–RX7    | Device 1–7 TX | Serial1–7 — read text stream  |
| TX8 / USB  | Output         | Serial8 or SerialUSB          |
| GND        | Ground         | common ground                 |
| 3.3V / VIN | Power supply   | 3.3V pin or VIN (5V tolerant) |

⚠ All RX inputs require BSS138 level shifter (5V → 3.3V).

### Teensy 4.1 — `readtext_sendhex` additions

| Pin     | Signal           | Description                     |
|---------|------------------|---------------------------------|
| TX1–TX7 | Device 1–7 RX   | Serial1–7 — HEX commands        |
| RX8     | Command input    | Serial8 — SET/HEX from host     |

⚠ TX outputs also require BSS138 level shifter (3.3V → 5V).

### VE.Direct Connector (JST-PH 2 mm, 4-pin)

| Pin | Signal      | Usage                                             |
|-----|-------------|---------------------------------------------------|
| 1   | GND         | to MCU GND                                        |
| 2   | TX (output) | to MCU RX (via level shifter on Teensy)           |
| 3   | RX (input)  | to MCU TX — `readtext_sendhex` only              |
| 4   | +5V         | max. 10 mA avg — not suitable for MCU power       |

---

## Power Supply

| Source                        | Voltage | Max. current            | Max. power  |
|-------------------------------|---------|-------------------------|-------------|
| VE.Direct pin 4 (per device) | 5V      | 10 mA (avg), 20 mA/5ms  | 50 mW       |
| Arduino Mega 2560 (draw)      | 5V      | ~80–100 mA              | ~400–500 mW |
| Teensy 4.1 (draw)             | 3.3–5V  | ~100 mA                 | ~300–500 mW |

**VE.Direct pin 4 is not suitable for powering the MCU** — it supplies only 10 mA average, the MCU requires ~100 mA.

Power the MCU via:
- **DC/DC converter** directly from the battery (e.g. 8–60V input module, set to 5V)
- **5V USB power supply** at the installation site
- **USB cable** from the host system — if close enough

---

## Galvanic Isolation

**Non-critical** as long as all VE.Direct devices share the same battery and common ground — devices and MCU are at the same potential.

**Relevant** when devices are on different PV strings with separate earthing, or when the MCU and devices are powered from separate sources. In these cases implement galvanic isolation **downstream of the MCU** — e.g. via an isolated serial converter on the output.

---

## Buffer Sizes

Each port buffers one complete VE.Direct block at a time (512 bytes). Blocks are forwarded immediately when complete.

---

## Technical Data

| Parameter                    | Mega 2560                              | Teensy 4.1                              |
|------------------------------|----------------------------------------|-----------------------------------------|
| Clock speed                  | 16 MHz                                 | 600 MHz                                 |
| Logic level                  | 5V                                     | 3.3V                                    |
| Level shifter needed         | No                                     | Yes — BSS138 per RX (+ TX for readtext_sendhex) |
| Hardware UARTs               | 4                                      | 8                                       |
| Max. device inputs           | 3                                      | 7                                       |
| Input baud rate              | 19200 baud, 8N1                        | 19200 baud, 8N1                         |
| Output baud rate             | 19200 baud (configurable)              | 19200 baud (configurable)               |
| Output connection            | TX0 pin or USB (same chip)             | TX8 pin or SerialUSB (`OUTPUT_USB`)     |
| Max. devices (direct)        | 3                                      | 7                                       |
| Max. devices (star)          | 9                                      | 21 (3× Teensy → 1 central)              |
| Max. devices (cascade)       | 9 / 12 at 115200 baud                  | 28 / more at 115200 baud                |
| Type detection               | none — all ports identical             | none — all ports identical              |
| HEX busy scope               | per port — others unaffected           | per port — others unaffected            |
| SET channel                  | yes (readtext_sendhex variant)                 | yes (readtext_sendhex variant)                  |
| VBAT_FALLBACK                | configurable (default 24V)             | configurable (default 24V)              |
| Power supply                 | external 5V, min. 150 mA              | external 5V, min. 150 mA               |
| CPU load (text only)         | < 1 % at 3 devices                   | < 0.1 % at 7 devices                  |
| Text latency                 | < 1 block period (~83 ms)             | < 1 block period (~83 ms)              |
| HEX command latency          | ~50–100 ms typical, 1s max            | ~50–100 ms typical, 1s max             |

---

## Limitations

- Baud rate must be identical on all input stages (19200 baud)
- During a SET or HEX command only the affected device port pauses — other ports continue normally
- `VBAT_FALLBACK` is used until first Vbat is received — SET commands in the first seconds after startup may be slightly inaccurate
- VE.Direct pin 4 supplies max. 10 mA — MCU must be powered externally
- Cerbo GX / Venus GX cannot be used as direct receiver
- Teensy 4.1: all RX inputs require BSS138 level shifter (5V → 3.3V); `readtext_sendhex` TX outputs also require level shifters (3.3V → 5V)
- PIDs are re-learned on every block — device swaps are detected within ~1s. A PID expires after `PID_TIMEOUT` ms of inactivity (default 10s) and is then routed upstream or returns `ERR timeout`
