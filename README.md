# VE.Direct Aggregator — Technical Specification

**Firmware · Arduino Mega 2560 · v1.0 · 2026**

---

## Overview

The VE.Direct Aggregator is firmware for the Arduino Mega 2560 that reads multiple Victron MPPT solar charge controllers via their serial VE.Direct interface and merges the data streams into a single serial output. Instead of multiple separate serial connections to the receiving system, only one is required.

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

Due to automatic type detection multiple aggregators can be connected in star or cascade topology without any code changes.

---

## Supported Topologies

### Direct — up to 3 chargers on 1 Mega

```
MPPT 1 ─TX──► [Mega] RX1
MPPT 2 ─TX──► [Mega] RX2  ──► TX0 ──► output
MPPT 3 ─TX──► [Mega] RX3
```

### Star — up to 9 chargers, 3 Megas to 1 central Mega

```
MPPT 1─3 ──► [Mega 1] ──TTL──┐
MPPT 4─6 ──► [Mega 2] ──TTL──┼──► [Mega 4] ──► output
MPPT 7─9 ──► [Mega 3] ──TTL──┘
```

Mega 1–3 sit close to their chargers. Mega 4 combines the three short TTL connections into a single serial output.

### Cascade — up to 12 chargers, 4 stages in series

```
MPPT 1─3  ──► [Mega 1] ──► [Mega 2] ──► [Mega 3] ──► [Mega 4] ──► output
                3 blocks     6 blocks     9 blocks     12 blocks
```

With a higher output baud rate (e.g. 115200 Baud) a 4-stage cascade is practical. Each stage adds 3 chargers. The parallel read time per stage is ~249 ms, total transmission at 115200 Baud is ~168 ms — well within the 1-second window.

---

## Output Format

Before each packet a marker is sent indicating the number of VE.Direct blocks contained:

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

## Timing Budget at 19200 Baud

VE.Direct transmits once per second. A typical block (~200 bytes) occupies ~83 ms of transmission time.

| Chargers | Transmission time | Utilisation | Headroom |
|----------|------------------|-------------|----------|
| 1        | ~83 ms           | 8 %         | 917 ms   |
| 3        | ~249 ms          | 25 %        | 751 ms   |
| 6        | ~498 ms          | 50 %        | 502 ms   |
| 9        | ~747 ms          | 75 %        | 253 ms   |
| 12       | ~996 ms          | ~100 % ⚠    | —        |

At 12 chargers and 19200 Baud output the 1-second window is fully consumed. The practical limit is 9 chargers, leaving a comfortable 25 % headroom.

With a higher output baud rate (115200 Baud) the output transmission of 12 blocks takes only ~168 ms, reducing total time to ~417 ms and leaving 58 % headroom.

---

## Pin Assignment Arduino Mega 2560

| Pin       | Signal                    | Description                   |
|-----------|---------------------------|-------------------------------|
| RX1 (19)  | Charger / Upstream 1 TX   | Serial1 — hardware UART       |
| RX2 (17)  | Charger / Upstream 2 TX   | Serial2 — hardware UART       |
| RX3 (15)  | Charger / Upstream 3 TX   | Serial3 — hardware UART       |
| TX0 (1)   | Output                    | Serial0 — aggregated stream   |
| GND       | Ground all inputs         | common ground                 |
| 5V        | Power supply              | from VE.Direct pin 4          |

VE.Direct uses 5V TTL — directly compatible with the Arduino Mega. No level shifter required. The 5V supply (max. 100 mA) from one of the chargers is sufficient for the Mega.

### VE.Direct Connector Pinout (JST-PH 2 mm, 4-pin)

| Pin | Signal       | Usage              |
|-----|--------------|--------------------|
| 1   | GND          | to Mega GND        |
| 2   | TX (output)  | to Mega RX1/2/3    |
| 3   | RX (input)   | not connected      |
| 4   | +5V          | Mega power supply  |

---

## Buffer Sizes

| Input type | Buffer size | Capacity                  |
|------------|-------------|---------------------------|
| DIRECT     | 512 bytes   | 1 VE.Direct block         |
| UPSTREAM   | 2048 bytes  | up to ~9 blocks (cascade) |

---

## Technical Data

| Parameter               | Value                                   |
|-------------------------|-----------------------------------------|
| Target platform         | Arduino Mega 2560 (ATmega2560, 16 MHz)  |
| Input baud rate         | 19200 Baud, 8N1                         |
| Output baud rate        | 19200 Baud, 8N1 (configurable)          |
| Max. inputs             | 3 × hardware UART                       |
| Max. chargers (direct)  | 3                                       |
| Max. chargers (star)    | 9                                       |
| Max. chargers (cascade) | 9 at 19200 / 12 at 115200 Baud output   |
| Type detection          | automatic, once at startup              |
| Power supply            | 5V from VE.Direct pin 4                 |
| CPU load                | < 1 % at 3 chargers                     |
| Latency                 | < 1 block period (~83 ms)               |

---

## Required Hardware

| Component               | Notes                                  |
|-------------------------|----------------------------------------|
| Arduino Mega 2560 clone | e.g. Elegoo, ~10–20 €                  |
| JST-PH 2 mm cable       | 4-pin, for VE.Direct connections       |

---

## Limitations

- Topology changes require a restart of all affected Megas
- In cascade topology the last Mega must use Serial0 exclusively as output — no USB debug during operation
- Baud rate must be identical on all input stages (19200 Baud)
- VE.Direct text mode only — HEX mode is not supported
