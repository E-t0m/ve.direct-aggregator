# VE.Direct Aggregator — Technical Specification

**Firmware · Arduino Mega 2560 / Teensy 4.1 · v1.0 · 2026**

---

## Overview

The VE.Direct Aggregator is firmware for the Arduino Mega 2560 or Teensy 4.1 with two independent functional areas:

**Text aggregation:** Multiple Victron MPPT solar charge controllers are read via VE.Direct and merged into a single serial output.

**Power control:** A bidirectional command channel allows setting the maximum charge power of individual or all chargers. The channel is integrated into all topologies — commands are routed through cascades, confirmations are returned.

Both functions share the same hardware UARTs and do not interfere with each other.

---

## Firmware Variants

Six variants are available depending on the required feature set and target hardware:

| File | Hardware | Inputs | Features |
|------|----------|--------|----------|
| `vedirect_single.ino` | Mega 2560 | 3 | Direct chargers only, no upstream, no SET |
| `vedirect_multiple.ino` | Mega 2560 | 3 | Auto type detection DIRECT/UPSTREAM, no SET |
| `vedirect_multiple_powerset.ino` | Mega 2560 | 3 | Full: type detection + SET + HEX power control |
| `vedirect_single_teensy41.ino` | Teensy 4.1 | 7 | Direct chargers only, no upstream, no SET |
| `vedirect_multiple_teensy41.ino` | Teensy 4.1 | 7 | Auto type detection DIRECT/UPSTREAM, no SET |
| `vedirect_multiple_powerset_teensy41.ino` | Teensy 4.1 | 7 | Full: type detection + SET + HEX power control |

All variants share the same output format (`---\tN\r\n` marker + blocks) and are mutually compatible — any variant's output can feed another variant's upstream input. Mega and Teensy variants can be freely mixed in star and cascade topologies.

**Using `multiple_powerset` everywhere:** Since `multiple_powerset` is a strict superset — it auto-detects port type and the SET channel does nothing when unused — it can be flashed on all MCUs in a topology without functional difference. The upstream MCUs simply never receive SET commands and their TX pins remain idle. This simplifies firmware management at the cost of slightly higher code complexity on each MCU.

**Why mixing works:** The aggregator protocol is purely serial and hardware-agnostic. Each MCU only sees bytes on its input pins — it does not know or care whether the upstream device is a Mega, a Teensy, or a direct charger. As long as baud rate (19200) and signal levels are matched, any combination is valid.

**Signal level when mixing:** The Mega operates at 5V TTL, the Teensy at 3.3V. When connecting a Mega output to a Teensy input, a BSS138 level shifter is required on the Teensy RX pin. When connecting a Teensy output to a Mega input, no level shifter is needed — the Mega RX pin is 5V tolerant and correctly reads 3.3V high signals.

**Example — mixed star topology:**

```
MPPT 1─7  ──► [Teensy single] ──TTL──┐
MPPT 8─10 ──► [Mega single]   ──TTL──┼──► [Mega multiple] ──► output
MPPT 11─13──► [Mega single]   ──TTL──┘
```

The central Mega runs `multiple` and sees three upstream streams — it does not distinguish between the Teensy and Mega sources, all deliver the same `---\tN\r\n` format. The upstream MCUs run `single` — they only have direct chargers and never need upstream detection. Result: 13 chargers on a single output.

**Note:** 13 chargers × ~83 ms = ~1079 ms at 19200 baud output — this exceeds the 1-second transmit interval. Set `BAUD_OUT` to `115200` on the central Mega to bring transmission time down to ~182 ms.

---

## Features

### Multi-channel input

All hardware UARTs are polled simultaneously. Each channel buffers its data stream independently — a slow or temporarily absent charger does not block the others.

### Automatic port type detection

*(`multiple` and `multiple_powerset` only)*

At startup the firmware reads the first incoming characters on each port and decides once whether a direct MPPT charger or an upstream aggregator is connected. No manual configuration is required.

```
Port receives "---\t..."  →  UPSTREAM  (upstream aggregator)
Port receives "PID\t..."  →  DIRECT    (direct MPPT charger)
```

Once detected the type is fixed for the entire runtime. Topology changes require a restart.

### Serial output

All buffered blocks are sent sequentially via the output serial port. Since only one block is sent at a time there are no collisions.

### Cascadability

*(`multiple` and `multiple_powerset` only)*

Due to automatic type detection multiple aggregators can be connected in star or cascade topology without any code changes. Empty Megas with no chargers attached act as transparent relays — they forward all upstream traffic and route SET commands without modification. The only practical limit is latency relative to the 1-second transmit interval of the chargers.

### Power control via SET command

*(`multiple_powerset` only)*

The Mega receives SET commands on Serial0 RX, translates them into VE.Direct HEX commands and sends them to the correct charger. After the charger confirms the setting it is verified by HEX GET, the charger is actively switched back to text mode, and a confirmation is returned.

**Command format:**
```
SET <pid> <watts>\n     limit a single charger by PID
SET ALL <watts>\n       limit all chargers simultaneously
```

**Reply format:**
```
OK <pid> <watts>W <amps>A\n     setting verified
ERR <pid> timeout\n             no HEX ACK within 1s
ERR <pid> verify set=XA rb=YA\n readback mismatch
```

### Arbitrary HEX passthrough

*(`multiple_powerset` only)*

The host can send any VE.Direct HEX string directly to a charger by PID. The firmware passes it verbatim without parsing or validation. The reply is returned prefixed with the PID.

**Command format:**
```
HEX <pid> <hex_string>\n    send to single charger
HEX ALL <hex_string>\n      broadcast to all chargers
```

**Reply format:**
```
HEX_REPLY <pid> :<hex_response>\n
ERR <pid> timeout\n
```

Examples:
```
HEX 0xA053 :154\n            restore text mode
HEX ALL :154\n               restore text mode on all
HEX 0xA053 :70015200A3\n     arbitrary GET
```

The host is responsible for correct HEX formatting including checksum. For `HEX` commands the host must restore text mode manually if needed — the firmware does not do this automatically (unlike `SET`).

### Automatic watts-to-amps conversion

*(`multiple_powerset` only)*

The Mega learns the current battery voltage (`Vbat`) from the live text stream and automatically converts the watt value of a SET command to amps with 0.1A resolution:

```
A    = watts / Vbat                  (float, e.g. 19.5 A)
reg  = round(A × 10)                 (register value in 0.1A units)
```

Until the first Vbat value is received (~1–2 seconds after startup) a fallback of 24V is used.

### PID-based routing in cascades

*(`multiple_powerset` only)*

Each Mega learns the PIDs of its directly connected chargers from the text stream. Unknown PIDs are forwarded on the TX pins of upstream ports. Replies from upstream are passed back on Serial0 TX.

---

## Supported Topologies

### Direct — up to 3 chargers on 1 Mega, or up to 7 on 1 Teensy 4.1

```
MPPT 1 ─TX──► [Mega/Teensy] RX1   ──► TX_out ──► output
MPPT 2 ─TX──► [Mega/Teensy] RX2
MPPT 3 ─TX──► [Mega/Teensy] RX3
...
```

### Star — up to 9 chargers, 3 Megas to 1 central Mega

```
MPPT 1─3 ──► [Mega 1] ──TTL──┐
MPPT 4─6 ──► [Mega 2] ──TTL──┼──► [Mega 4] ──► output
MPPT 7─9 ──► [Mega 3] ──TTL──┘
```

With `multiple_powerset` each TTL connection is bidirectional — SET commands flow forward, OK/ERR replies flow back.

### Cascade — up to 12 chargers at 115200 baud output

```
MPPT 1─3 ──► [Mega 1] ──► [Mega 2] ──► [Mega 3] ──► [Mega 4] ──► output
               3 blocks     6 blocks     9 blocks     12 blocks
```

### Relay cascade — empty Megas as repeaters

```
MPPT 1─3 ──► [Mega] ──► [Mega empty] ──► [Mega empty] ──► output
```

Megas with no chargers attached pass all traffic through transparently. Useful as TTL line repeaters when cable runs exceed ~5m. No code changes required.

---

## Output Format

A marker is sent before each packet indicating the number of VE.Direct blocks contained:

```
---\tN\r\n          ← marker: N blocks follow
PID\t0xA053\r\n     ┐
VPV\t18240\r\n      │ block 1 (charger 1)
PPV\t120\r\n        │
...\r\n             │
Checksum\tX\r\n     │
\r\n                ┘
PID\t0xA060\r\n     ┐
...                 │ block 2 (charger 2)
\r\n                ┘
```

Each VE.Direct block ends with `\r\n\r\n` — two consecutive `\n` signal the end of a block.

The `---\tN` marker is not a VE.Direct field. Standard VE.Direct parsers ignore it in the `else: pass` branch without errors.

---

## SET Command Channel — HEX Sequence

*(`multiple_powerset` only)*

When a SET command arrives the text aggregator pauses for the affected charger. The sequence on the Mega:

```
1.  send HEX SET:   :8<reg_lo><reg_hi>00<val_lo><val_hi><cs>\n
2.  wait for HEX ACK                             (1s timeout → ERR timeout)
3.  send HEX GET:   :7<reg_lo><reg_hi>00<cs>\n
4.  wait for HEX GET reply                       (1s timeout → ERR timeout)
5.  check readback == sent value                 (mismatch → ERR verify)
6.  restore text mode: :154\n
7.  send OK/ERR on Serial0 TX
    — do NOT wait for text stream to resume
```

**Register used: `0x2015` — Charge Current Limit**

| Property    | Value |
|-------------|-------|
| Register    | `0x2015` |
| Unit        | 0.1A |
| Storage     | volatile — unlimited write cycles |
| Conversion  | `round(watts / Vbat × 10)` → register value |

`0x2015` is preferred over the non-volatile register `0xEDF0` — the latter has limited write cycles and is not suitable for frequent updates.

The gap in the text stream of the affected charger is the signal to the receiver that a SET command was executed. No separate notification is sent.

For `SET ALL` all HEX SET commands are sent simultaneously (pseudo-multicast), replies and verification are then handled one by one.

---

## Timing Budget at 19200 Baud

VE.Direct transmits once per second. A typical block (~200 bytes) occupies ~83 ms of transmission time at 19200 baud, ~14 ms at 115200 baud.

### Output at 19200 baud

| Chargers | Transmission time | Utilisation | Headroom |
|----------|------------------|-------------|----------|
| 1        | ~83 ms           | 8 %         | 917 ms   |
| 3        | ~249 ms          | 25 %        | 751 ms   |
| 6        | ~498 ms          | 50 %        | 502 ms   |
| 7        | ~581 ms          | 58 %        | 419 ms   |
| 9        | ~747 ms          | 75 %        | 253 ms   |
| 12       | ~996 ms          | ~100 % ⚠    | —        |

### Output at 115200 baud

| Chargers | Transmission time | Utilisation | Headroom |
|----------|------------------|-------------|----------|
| 9        | ~126 ms          | 13 %        | 874 ms   |
| 13       | ~182 ms          | 18 %        | 818 ms   |
| 21       | ~294 ms          | 29 %        | 706 ms   |
| 53       | ~742 ms          | 74 %        | 258 ms   |
| 71       | ~994 ms          | ~100 % ⚠    | —        |

The theoretical maximum at 115200 baud output is ~71 chargers before the 1-second window is exhausted. The practical limit is lower — it is determined by how many charger inputs can be read in parallel within one cycle.

### Optimal topology for maximum charger count

| Output baud | Topology | MCUs | Max. chargers | Notes |
|-------------|----------|------|---------------|-------|
| 19200       | Mega direct | 1 | 3 | simplest |
| 19200       | Mega star | 4 | 9 | 3× Mega + 1 central |
| 19200       | Teensy direct | 1 | 7 | single MCU |
| 115200      | Mega cascade | 4 | 12 | 4-stage cascade |
| 115200      | Mixed star | 4 | 13 | 1× Teensy + 2× Mega → 1 central |
| 115200      | Teensy star | 4 | 21 | 3× Teensy → 1 central Teensy |
| 115200      | Mixed deep star | 7 | 28+ | multiple levels, mixed hardware |

The single serial output line is never the bottleneck — baud rate and MCU input count are the real limits.

---

## Hardware & Logic Levels

VE.Direct uses 5V TTL logic. The two platforms differ in their logic level:

| Platform | Logic level | Level shifter needed |
|----------|-------------|----------------------|
| Arduino Mega 2560 | 5V | No — direct connection |
| Teensy 4.1 | 3.3V | **Yes — 1 channel per input** |

**Recommended level shifter for Teensy 4.1:** BSS138-based bidirectional 4-channel module (e.g. labeled "I2C Logic Level Converter 5V to 3.3V"). Despite the I2C label, BSS138 modules work for any signal up to ~1 MBit/s including UART at 19200 baud.

One 4-channel module covers 4 RX inputs. Two modules cover all 7 inputs of the Teensy 4.1 with one channel to spare.

For `multiple_powerset` bidirectional operation (RX + TX per charger), each charger requires 2 channels — one 4-channel module covers 2 chargers.

---

## Pin Assignment

### Arduino Mega 2560 — all variants

| Pin       | Signal                  | Description                    |
|-----------|-------------------------|--------------------------------|
| RX1 (19)  | Charger / Upstream 1 TX | Serial1 — read text stream     |
| RX2 (17)  | Charger / Upstream 2 TX | Serial2 — read text stream     |
| RX3 (15)  | Charger / Upstream 3 TX | Serial3 — read text stream     |
| TX0 (1)   | Output                  | Serial0 — aggregated stream    |
| GND       | Ground all inputs       | common ground                  |
| 5V        | Power supply            | external DC/DC or USB          |

### Arduino Mega 2560 — `multiple_powerset` additions

| Pin       | Signal                  | Description                         |
|-----------|-------------------------|-------------------------------------|
| TX1 (18)  | Charger / Upstream 1 RX | Serial1 — HEX commands / SET fwd    |
| TX2 (16)  | Charger / Upstream 2 RX | Serial2 — HEX commands / SET fwd    |
| TX3 (14)  | Charger / Upstream 3 RX | Serial3 — HEX commands / SET fwd    |
| RX0 (0)   | SET command input       | Serial0 — from downstream / host    |

### Teensy 4.1 — `single` and `multiple` variants

| Pin        | Signal         | Description                              |
|------------|----------------|------------------------------------------|
| RX1–RX7    | Charger 1–7 TX | Serial1–7 — read text stream             |
| TX8        | Output         | Serial8 — aggregated stream              |
| GND        | Ground         | common ground                            |
| 3.3V / VIN | Power supply   | 3.3V pin or VIN (5V tolerant)            |

⚠ All RX inputs require a BSS138 level shifter (5V → 3.3V).

### Teensy 4.1 — `multiple_powerset` additions

| Pin        | Signal         | Description                              |
|------------|----------------|------------------------------------------|
| TX1–TX7    | Charger 1–7 RX | Serial1–7 — HEX commands / SET fwd      |
| RX8        | SET command input | Serial8 — from downstream / host      |

⚠ All TX outputs also require BSS138 level shifter (3.3V → 5V).

### VE.Direct Connector Pinout (JST-PH 2 mm, 4-pin)

| Pin | Signal       | Usage                                              |
|-----|--------------|-----------------------------------------------------|
| 1   | GND          | to MCU GND                                          |
| 2   | TX (output)  | to MCU RX (via level shifter on Teensy)             |
| 3   | RX (input)   | to MCU TX — `multiple_powerset` only                |
| 4   | +5V          | max. 10 mA avg — not suitable for MCU power supply  |

---

## Power Supply

| Source                        | Voltage | Max. current            | Max. power  |
|-------------------------------|---------|-------------------------|-------------|
| VE.Direct pin 4 (per charger) | 5V      | 10 mA (avg), 20 mA/5ms  | 50 mW       |
| Arduino Mega 2560 (draw)      | 5V      | ~80–100 mA              | ~400–500 mW |
| Teensy 4.1 (draw)             | 3.3–5V  | ~100 mA                 | ~300–500 mW |

**VE.Direct pin 4 is not suitable for powering the MCU** — it supplies only 10 mA average.

Power the MCU via one of:

- **DC/DC converter** (e.g. MP1584, LM2596, or 8–60V input module) directly from the battery, set to 5V
- **5V USB power supply** at the installation site
- **USB cable** from the host system — only if close enough

---

## Galvanic Isolation

**Non-critical** as long as all MPPT chargers share the same battery and common ground potential. In this case chargers and MCU are at the same potential — no equalization currents, no risk.

**Relevant** when:
- Chargers are on different PV strings with separate earthing
- The MCU and chargers are powered from separate sources

In these cases galvanic isolation should be implemented **downstream of the MCU** — e.g. via an isolated serial converter on the output. This protects the entire downstream system regardless of the number of chargers.

---

## Buffer Sizes

| Input type | Buffer size | Capacity                   |
|------------|-------------|----------------------------|
| DIRECT     | 512 bytes   | 1 VE.Direct block          |
| UPSTREAM   | 2048 bytes  | up to ~9 blocks (cascade)  |

---

## Technical Data

| Parameter                    | Mega 2560                              | Teensy 4.1                              |
|------------------------------|----------------------------------------|-----------------------------------------|
| Clock speed                  | 16 MHz                                 | 600 MHz                                 |
| Logic level                  | 5V                                     | 3.3V                                    |
| Level shifter needed         | No                                     | Yes — BSS138 per RX (+ TX for powerset) |
| Hardware UARTs               | 4                                      | 8                                       |
| Max. charger inputs          | 3                                      | 7                                       |
| Input baud rate              | 19200 baud, 8N1                        | 19200 baud, 8N1                         |
| Output baud rate             | 19200 baud (configurable)              | 19200 baud (configurable)               |
| Max. chargers (direct)       | 3                                      | 7                                       |
| Max. chargers (star)         | 9                                      | 21 (3× Teensy → 1 central)              |
| Max. chargers (cascade)      | 9 / 12 at 115200 baud                  | 28 / more at 115200 baud                |
| Type detection               | automatic (multiple / powerset)        | automatic (multiple / powerset)         |
| SET channel                  | yes (powerset)                         | yes (powerset)                          |
| Power supply                 | external 5V, min. 150 mA              | external 5V, min. 150 mA               |
| CPU load (text only)         | < 1 % at 3 chargers                   | < 0.1 % at 7 chargers                  |
| Text latency                 | < 1 block period (~83 ms)             | < 1 block period (~83 ms)              |

---

## Limitations

- Topology changes require a restart of all affected MCUs
- In cascade topology the last Mega must use Serial0 exclusively as output — no USB debug during operation
- Baud rate must be identical on all input stages (19200 baud)
- During a SET command (`multiple_powerset`) the text stream of the affected charger pauses for ~1–3 seconds
- Vbat fallback 24V until first text block received — SET commands in the first seconds after startup may be slightly inaccurate
- VE.Direct pin 4 supplies max. 10 mA — MCU must be powered externally
- Teensy 4.1: all RX inputs require BSS138 level shifter (5V → 3.3V); `multiple_powerset` also requires level shifters on TX outputs (3.3V → 5V)
