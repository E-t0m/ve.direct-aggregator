# VE.Direct Aggregator — Technical Specification

**Firmware · Arduino Mega 2560**

---

## Overview

The VE.Direct Aggregator is firmware for the Arduino Mega 2560 with two independent functional areas:

**Text aggregation:** Multiple Victron MPPT solar charge controllers are read via VE.Direct and merged into a single serial output.

**Power control:** A bidirectional command channel allows setting the maximum charge power of individual or all chargers. The channel is integrated into all topologies — commands are routed through cascades, confirmations are returned.

Both functions share the same hardware UARTs and do not interfere with each other.

---

## Features

### Multi-channel input

Three hardware UARTs on the Mega (Serial1, Serial2, Serial3) are polled simultaneously. Each channel buffers its data stream independently — a slow or temporarily absent charger does not block the others.

### Automatic port type detection

At startup the firmware reads the first incoming characters on each port and decides once whether a direct MPPT charger or an upstream aggregator is connected. No manual configuration is required.

```
Port receives "---\t..."  →  UPSTREAM  (upstream aggregator)
Port receives "PID\t..."  →  DIRECT    (direct MPPT charger)
```

Once detected the type is fixed for the entire runtime. Topology changes require a restart.

### Serial output

All buffered blocks are sent sequentially via Serial0 TX (pin 1). Since only one block is sent at a time there are no collisions.

### Cascadability

Due to automatic type detection multiple aggregators can be connected in star or cascade topology without any code changes. Empty Megas with no chargers attached act as transparent relays — they forward all upstream traffic and route SET commands without modification. The only practical limit is latency relative to the 1-second transmit interval of the chargers.

### Power control via SET command

The Mega receives SET commands on Serial0 RX, translates them into VE.Direct HEX commands and sends them to the correct charger. After the charger confirms the setting it is verified by HEX GET, the charger is actively switched back to text mode, and a confirmation is returned.

**Command format:**
```
SET <pid> <watts>\n     limit a single charger by PID
SET ALL <watts>\n       limit all chargers simultaneously
```

**Reply format:**
```
OK <pid> <watts>\n      setting verified by re-read
ERR <pid> timeout\n     no HEX ACK within 1s
ERR <pid> verify\n      readback value does not match
```

### Automatic watts-to-amps conversion

The Mega learns the current battery voltage (`Vbat`) from the live text stream and automatically converts the watt value of a SET command to milliamps:

```
mA = (watts × 1000) / Vbat
```

Until the first Vbat value is received (~1–2 seconds after startup) a fallback of 24V is used.

### PID-based routing in cascades

Each Mega learns the PIDs of its directly connected chargers from the text stream. Unknown PIDs are forwarded on the TX pins of upstream ports. Replies from upstream are passed back on Serial0 TX.

---

## Supported Topologies

### Direct — up to 3 chargers on 1 Mega

```
MPPT 1 ─TX──► [Mega] RX1 / TX1 ──► MPPT 1 RX
MPPT 2 ─TX──► [Mega] RX2 / TX2 ──► MPPT 2 RX   ◄──► TX0/RX0 ◄──► output
MPPT 3 ─TX──► [Mega] RX3 / TX3 ──► MPPT 3 RX
```

### Star — up to 9 chargers, 3 Megas to 1 central Mega

```
MPPT 1─3 ◄──► [Mega 1] ◄──TTL──┐
MPPT 4─6 ◄──► [Mega 2] ◄──TTL──┼──► [Mega 4] ◄──► output
MPPT 7─9 ◄──► [Mega 3] ◄──TTL──┘
```

Each TTL connection is bidirectional — the central Mega's TX goes to the upstream Mega's RX0, the upstream Mega's TX0 goes back to the central Mega's RX. SET commands flow forward, OK/ERR replies flow back.

### Cascade — up to 12 chargers at 115200 baud output

```
MPPT 1─3 ◄──► [Mega 1] ◄──► [Mega 2] ◄──► [Mega 3] ◄──► [Mega 4] ◄──► output
                 3 blocks     6 blocks      9 blocks     12 blocks
```

SET commands are forwarded stage by stage until the charger with the matching PID is found. OK/ERR replies are routed back.

### Relay cascade — empty Megas as repeaters

```
MPPT 1─3 ◄──► [Mega] ◄──► [Mega empty] ◄──► [Mega empty] ◄──► output
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

The gap in the text stream of the affected charger is the signal to the receiver that a SET command was executed. No separate notification is sent.

For `SET ALL` all HEX SET commands are sent simultaneously (pseudo-multicast), replies and verification are then handled one by one.

---

## Timing Budget at 19200 Baud

VE.Direct transmits once per second. A typical block (~200 bytes) occupies ~83 ms of transmission time.

| Chargers | Transmission time | Utilisation | Headroom |
|----------|------------------|-------------|----------|
| 1        | ~83 ms           | 8 %         | 917 ms   |
| 3        | ~249 ms          | 25 %        | 751 ms   |
| 6        | ~498 ms          | 50 %        | 502 ms   |
| 9        | ~747 ms          | 75 %        | 253 ms   |
| 12       | ~996 ms          | ~100 % ⚠    | —        |

At 12 chargers and 19200 baud the 1-second window is fully consumed. The practical limit is 9 chargers, leaving a comfortable 25 % headroom. With 115200 baud output 12 chargers in a 4-stage cascade are feasible — total time ~417 ms, 58 % headroom.

---

## Pin Assignment Arduino Mega 2560

| Pin       | Signal                          | Description                         |
|-----------|---------------------------------|-------------------------------------|
| RX1 (19)  | Charger / Upstream 1 TX         | Serial1 — read text stream          |
| TX1 (18)  | Charger / Upstream 1 RX         | Serial1 — HEX commands / SET fwd    |
| RX2 (17)  | Charger / Upstream 2 TX         | Serial2 — read text stream          |
| TX2 (16)  | Charger / Upstream 2 RX         | Serial2 — HEX commands / SET fwd    |
| RX3 (15)  | Charger / Upstream 3 TX         | Serial3 — read text stream          |
| TX3 (14)  | Charger / Upstream 3 RX         | Serial3 — HEX commands / SET fwd    |
| TX0 (1)   | Output + OK/ERR replies         | Serial0 — aggregated stream         |
| RX0 (0)   | SET command input               | Serial0 — from downstream / host    |
| GND       | Ground all inputs               | common ground                       |
| 5V        | Power supply                    | external DC/DC or USB               |

VE.Direct uses 5V TTL — directly compatible with the Arduino Mega. No level shifter required.

### VE.Direct Connector Pinout (JST-PH 2 mm, 4-pin)

| Pin | Signal       | Usage                               |
|-----|--------------|-------------------------------------|
| 1   | GND          | to Mega GND                         |
| 2   | TX (output)  | to Mega RX1/2/3                     |
| 3   | RX (input)   | to Mega TX1/2/3 — for SET commands  |
| 4   | +5V          | max. 10 mA — not suitable for Mega  |

---

## Power Supply

| Source                        | Voltage | Max. current            | Max. power  |
|-------------------------------|---------|-------------------------|-------------|
| VE.Direct pin 4 (per charger) | 5V      | 10 mA (avg), 20 mA/5ms  | 50 mW       |
| Arduino Mega 2560 (draw)      | 5V      | ~80–100 mA              | ~400–500 mW |

**VE.Direct pin 4 is not suitable for powering the Mega** — the Mega requires 8–10× the available current.

Power the Mega via one of:

- **DC/DC converter** (e.g. MP1584, LM2596) directly from the battery, set to 5V — reliable, inexpensive (~1–2 €)
- **5V USB power supply** at the installation site
- **USB cable** from the host system — only if close enough to the Mega

---

## Galvanic Isolation

**Non-critical** as long as all MPPT chargers share the same battery and common ground potential. In this case chargers and Mega are at the same potential — no equalization currents, no risk.

**Relevant** when:
- Chargers are on different PV strings with separate earthing
- The Mega and chargers are powered from separate sources

In these cases galvanic isolation should be implemented **downstream of the Mega** — e.g. via an isolated serial converter on the output. This protects the entire downstream system regardless of the number of chargers.

---

## Buffer Sizes

| Input type | Buffer size | Capacity                   |
|------------|-------------|----------------------------|
| DIRECT     | 512 bytes   | 1 VE.Direct block          |
| UPSTREAM   | 2048 bytes  | up to ~9 blocks (cascade)  |

---

## Technical Data

| Parameter                  | Value                                    |
|----------------------------|------------------------------------------|
| Target platform            | Arduino Mega 2560 (ATmega2560, 16 MHz)   |
| Input baud rate            | 19200 baud, 8N1                          |
| Output baud rate           | 19200 baud, 8N1 (configurable)           |
| Max. inputs                | 3 × hardware UART bidirectional          |
| Max. chargers (direct)     | 3                                        |
| Max. chargers (star)       | 9                                        |
| Max. chargers (cascade)    | 9 at 19200 / 12 at 115200 baud output    |
| Type detection             | automatic, once at startup               |
| PID learning               | automatic from text stream               |
| Vbat learning              | automatic, updated every second          |
| SET timeout                | 1s per HEX reply                         |
| Power supply               | external 5V, min. 150 mA                 |
| CPU load (text only)       | < 1 % at 3 chargers                      |
| Text latency               | < 1 block period (~83 ms)                |
| SET confirmation latency   | ~100–200 ms + HEX reply time             |

---

## Limitations

- Topology changes require a restart of all affected Megas
- In cascade topology the last Mega must use Serial0 exclusively as output — no USB debug during operation
- Baud rate must be identical on all input stages (19200 baud)
- During a SET command the text stream of the affected charger pauses for ~1–3 seconds
- Vbat fallback 24V until first text block received — SET commands in the first seconds after startup may be slightly inaccurate
- VE.Direct pin 4 supplies max. 10 mA — Mega must be powered externally
