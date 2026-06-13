# VE.Direct Aggregator — Readtext Stream Specification

**Output stream format of `readtext` and `readtext_sendhex` firmware · v2.0 · 2026**

---

## Overview

The readtext stream is the aggregated output of the VE.Direct Aggregator firmware. It is a plain sequential VE.Direct text stream — standard VE.Direct format, multiple devices multiplexed onto a single serial connection.

The receiving end identifies individual devices by their `SER#` field. `PID` alone is insufficient as multiple devices of the same type share the same PID value.

---

## Serial Parameters

| Parameter | Value |
|-----------|-------|
| Baud rate | `BAUD_OUT` (default `BAUD_VEDIRECT` = 19200) |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |

---

## Stream Format

The stream consists of back-to-back VE.Direct text blocks. Each block is enqueued immediately when the `Checksum\t` line is received — the receive buffer is freed instantly so the next block can be read while the current one is being sent. Blocks are sent one at a time, no mixing between devices.

### Block structure

Each block:
- starts with `PID\t<hex_id>\r\n`
- contains one field per line in `NAME\tVALUE\r\n` format
- ends with `Checksum\t<byte>\r\n`

**Block end detection:** the `Checksum\t` line signals the end of a VE.Direct block. There is no separator between blocks and no double newline — the next block starts immediately with `PID\t` after the checksum line.

```
PID\t0xA060\r\n
FW\t174\r\n
SER#\tHQ2529K6QK4\r\n
V\t52010\r\n
I\t-60\r\n
VPV\t10\r\n
PPV\t0\r\n
CS\t0\r\n
MPPT\t0\r\n
OR\t0x00000001\r\n
ERR\t0\r\n
LOAD\tON\r\n
IL\t0\r\n
H19\t0\r\n
...
HSDS\t0\r\n
Checksum\t<byte>\r\n
PID\t0xA060\r\n       ← next block starts immediately
...
```

### WHO / firmware identification

On receipt of `WHO\n` the firmware responds with:

```
READTEXT Mega2560 N=3\n
READTEXT Teensy41 N=7\n
SENDHEX Mega2560 N=3\n   (readtext_sendhex variant)
```

This allows the host to detect which firmware variant and port count is active.

---

### ALIVE signal

If no block has been sent for `ALIVE_TIMEOUT` ms (default 10s), the firmware sends:

```
ALIVE\r\n
```

This is not a VE.Direct field. It signals that the MCU is running but no device data is available. Standard VE.Direct parsers ignore it. The `ve_aggregator` Python module recognises it and updates the connection timestamp.

---

## Device Identification

Devices are identified by `SER#`. The `ve_aggregator` module uses `SER#` as the internal device key.

| PID | Device family |
|-----|--------------|
| `0xA040`–`0xA05F` | BlueSolar MPPT |
| `0xA060`–`0xA07F` | SmartSolar MPPT |
| `0x0203`–`0x0205` | BMV-600 series |
| `0x0300`–`0x0302` | BMV-700 series |
| `0xA381` | SmartShunt |
| `0xA290`–`0xA2B0` | Phoenix Inverter |

---

## Field Reference

### MPPT Solar Charger

| Field | Unit (raw) | Converted | Description |
|-------|-----------|-----------|-------------|
| `PID` | string | — | Product ID |
| `FW` | string | — | Firmware version |
| `SER#` | string | — | Serial number |
| `V` | mV | V (÷1000) | Battery voltage |
| `I` | mA | A (÷1000) | Battery current |
| `VPV` | mV | V (÷1000) | PV input voltage |
| `PPV` | W | W | PV input power |
| `CS` | int | string | Charge state |
| `MPPT` | int | string | MPPT state |
| `ERR` | int | string | Error code |
| `LOAD` | string | — | Load output (ON/OFF) |
| `IL` | mA | A (÷1000) | Load current |
| `OR` | hex string | — | Off reason (bitmask) |
| `H19`–`H23` | Wh | Wh | Historical energy |
| `HSDS` | int | — | Day sequence number |

**CS values:** 0=Off, 2=Fault, 3=Bulk, 4=Absorption, 5=Float, 7=Equalise, 245=Starting, 247=Auto Equalise, 252=External control

**MPPT values:** 0=Off, 1=Voltage/current limited, 2=Active

### BMV Battery Monitor / SmartShunt

| Field | Unit (raw) | Converted | Description |
|-------|-----------|-----------|-------------|
| `V` | mV | V (÷1000) | Main battery voltage |
| `VS` | mV | V (÷1000) | Aux/starter battery voltage |
| `I` | mA | A (÷1000) | Battery current |
| `T` | °C | °C | Battery temperature |
| `P` | W | W | Instantaneous power |
| `CE` | mAh | Ah (÷1000) | Consumed Ah |
| `SOC` | ‰ | ‰ | State of charge |
| `TTG` | min | min | Time to go |
| `Alarm` | string | — | Alarm state (ON/OFF) |
| `Relay` | string | — | Relay state (ON/OFF) |
| `AR` | int | — | Alarm reason (bitmask) |
| `OR` | int | — | Off reason (bitmask) |

---

## Checksum

Each VE.Direct block includes a `Checksum` field. The checksum byte is chosen such that the sum of all bytes in the block (including the checksum byte itself) equals 0 modulo 256.

```python
assert sum(block_bytes) % 256 == 0
```

The firmware forwards all blocks without checksum validation — blocks are relayed as received. The `ve_aggregator` Python module validates checksums and silently discards invalid blocks.

**Note:** the checksum is computed over all raw bytes from `PID\t` through and including `Checksum\t<byte>\r\n`.

---

## Timing

VE.Direct devices transmit once per second. The aggregator sends each block immediately when the `Checksum\t` line is received.

At `BAUD_VEDIRECT` (19200 baud) a typical block (~200 bytes) takes ~83 ms to transmit. At `BAUD_UPSTREAM` (115200 baud) ~14 ms.

| Devices | Time at 19200 | Utilisation | Headroom |
|---------|--------------|-------------|----------|
| 1 | ~83 ms | 8 % | 917 ms |
| 3 | ~249 ms | 25 % | 751 ms |
| 7 | ~581 ms | 58 % | 419 ms |
| 9 | ~747 ms | 75 % | 253 ms |

---

## Receiver Compatibility

| Receiver | Compatible | Notes |
|----------|-----------|-------|
| `ve_aggregator` Python module | ✓ | Full support |
| Any VE.Direct text parser | ✓ | Reads blocks sequentially, ignores ALIVE |
| Cerbo GX / Venus GX | via Deagg. | `vedirect_deaggregator.py` required |

---

## Limitations

- Devices are identified by `SER#` — receivers must not rely on `PID` alone
- The stream is read-only — no commands or responses on the readtext stream
- For bidirectional communication (SET/HEX) use `readtext_sendhex` firmware
- Cerbo GX / Venus GX cannot be used as direct receiver — use `vedirect_deaggregator.py`

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
