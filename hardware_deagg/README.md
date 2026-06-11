# pico_vedirect — VE.Direct RS485 De-Aggregator Cluster Node

A Raspberry Pi Pico firmware that sits on an RS485 bus carrying an
aggregated VE.Direct stream (from the
[ve.direct-aggregator](https://github.com/E-t0m/ve.direct-aggregator)
project), splits incoming blocks by SER#, and presents each device as an
independent CDC-ACM serial port to the host (Cerbo GX or any Venus OS device).

Multiple Picos on the same RS485 bus form a self-organizing cluster:
they coordinate SER# ownership via short frames sent in the inter-block
idle gaps, so the cluster scales beyond the 7-port limit of a single Pico
without any central controller.

## Hardware

### Per Pico node

| Part | Notes |
|---|---|
| Raspberry Pi Pico H | pre-soldered headers, ~6 € |
| RS485 module with auto direction (MAX13487 / XY-017) | ~3 €, TTL in, A/B out |
| 120 Ω resistor | termination across A/B at each end of the bus |
| Micro-USB cable | to Cerbo GX USB port |
| 3× Dupont female-female cables | RO→GP1, 3V3→VCC, GND→GND |

### Wiring

```
RS485 module          Pico
VCC  ------------- 3V3  (pin 36)
GND  ------------- GND  (pin 38)
RO   ------------- GP1  (pin 2)   UART0 RX
DI   not connected                read-only node
DE/RE jumpered to GND or auto     auto-direction module handles this
```

Node ID jumpers (active-low, internal pull-up):

```
No jumpers  -> node 0  (master, wins all tie-breaks)
GP2 to GND  -> node 1
GP3 to GND  -> node 2
GP2+GP3     -> node 3
GP4 to GND  -> node 4
... etc.
```

Alternatively, set NODE_ID at compile time (see Build section).

### Cluster wiring

All Picos share the same two-wire RS485 bus:

```
Aggregator A/B ─────┬────── Pico #0 A/B
                    ├────── Pico #1 A/B
                    └────── Pico #2 A/B

120 Ω across A/B at the aggregator end
120 Ω across A/B at the last Pico on the bus
```

Each Pico connects independently to the host via its own USB cable and
(optionally) a USB hub:

```
Pico #0 USB ──┐
Pico #1 USB ──┼── USB hub ── Cerbo GX
Pico #2 USB ──┘
```

## Build

### Prerequisites

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) (tested with v1.5+)
- CMake 3.13+
- arm-none-eabi-gcc

```bash
export PICO_SDK_PATH=/path/to/pico-sdk

mkdir build && cd build

# Node 0 (default):
cmake ..

# Node 1 (compile-time node ID):
cmake .. -DNODE_ID=1

make -j4
```

This produces `pico_vedirect.uf2`.

### Flash

Hold BOOTSEL on the Pico while connecting USB, then copy the UF2:

```bash
cp build/pico_vedirect.uf2 /media/$USER/RPI-RP2/
```

The Pico reboots automatically and appears as 7 CDC serial ports.

## How it works

### Stream parsing

The aggregator sends a sequential VE.Direct text stream at 19200 baud.
Each block ends with `\r\n\r\n`. The router assembles bytes into blocks,
extracts the `SER#` field and dispatches each block to its CDC slot.

### Cluster coordination

Between blocks the RS485 bus is idle for a few milliseconds. Each Pico
uses this window to broadcast an 8-byte status frame:

```
0xFF  type  node_id  slots_used  slots_free  pid_high  pid_low  crc8
```

When a Pico sees a PID it has not handled before:
1. It checks the cluster-wide SER# ownership table (built from received frames)
2. If the PID is unclaimed and the node has a free slot, it schedules a
   CLAIM frame after a delay of `node_id × 10 ms`
3. Lower node IDs always send first, so ties are resolved deterministically
4. All other nodes record the claim and ignore subsequent blocks for that PID
5. If a node goes silent for 3 s its PIDs are released and redistributed

### Read-only operation

Host-to-device data (HEX commands from Venus OS) is silently discarded.
The Venus OS VE.Direct driver falls back to text-mode automatically when
no HEX responses arrive and still receives all measurement data.

## Diagnostics

Each Pico's CDC port names appear in `/dev/serial/by-id/` as:

```
usb-Victron_De-Aggregator_VEDirect_Cluster_Node_0_<serial>-if00
usb-Victron_De-Aggregator_VEDirect_Cluster_Node_0_<serial>-if02
...
```

The `iInterface` strings (`VEDIRECT-0-0` .. `VEDIRECT-0-6`) are visible in:

```bash
udevadm info -a -n /dev/ttyACM0 | grep interface
```

## Scaling

| Picos | Max PIDs | Bus topology |
|---|---|---|
| 1 | 7 | direct USB |
| 2 | 14 | hub or two USB ports |
| 3 | 21 | covers full aggregator output |
| N | N × 7 | limited by USB hub ports and RS485 load |

RS485 allows up to 32 standard-load receivers on one bus segment; at
19200 baud with a few Picos the electrical load is negligible.
