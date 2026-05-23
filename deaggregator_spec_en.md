# VE.Direct De-Aggregator -- Spec v2.0

**`vedirect_deaggregator.py`** reads the aggregated VE.Direct stream from a
Mega 2560 / Teensy 4.1 and exposes each MPPT as its own virtual serial port.

---

## Purpose

The aggregator combines N MPPT streams onto one serial port.
The de-aggregator splits that stream back apart -- one `/dev/pts/N` per device --
so that Venus OS / Cerbo GX sees each MPPT individually.

```
MPPT 1 -----+
MPPT 2 ----- Mega 2560 -- USB or RS-485 -- De-Aggregator -- /dev/pts/3 -> Venus OS
MPPT 3 -----+                                            +-- /dev/pts/4 -> Venus OS
                                                         +-- /dev/pts/5 -> Venus OS
```

---

## Requirements

```bash
pip install pyserial --break-system-packages
apt install socat        # optional, for manual testing
```

---

## Usage

```bash
# basic
python3 vedirect_deaggregator.py

# with options
python3 vedirect_deaggregator.py \
    --port /dev/ttyACM3 \
    --baud 19200 \
    --label HQ2529K6QK4=Roof \
    --label HQ2529AVWNQ=Garage
```

Output on startup:
```
10:00:01.123  ve_deaggregator v2.0 -- opening /dev/ttyACM3 at 19200 baud
10:00:01.456  connected -- waiting for blocks...

--- virtual port map -------------------------------------------
  /dev/pts/3  <->  HQ2529K6QK4  (Roof)
  /dev/pts/4  <->  HQ2529AVWNQ  (Garage)
----------------------------------------------------------------
```

---

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `--port` | `/dev/ttyACM3` | Aggregator serial port |
| `--baud` | `19200` | Baud rate |
| `--max` | `16` | Maximum number of virtual ports |
| `--label SER=NAME` | -- | Custom label per device |

---

## Protocol

The de-aggregator reads the stream and filters:
- HEX frames (`:...`) -- discarded
- `ALIVE` -- discarded
- `OK`, `ERR`, `HEX_REPLY`, `READTEXT`, `SENDHEX` -- discarded
- Complete text blocks (`PID`...`Checksum`) -- routed

Routing is by `SER#`. Each new device gets a new virtual port automatically.

---

## Venus OS Integration

### Raspberry Pi running Venus OS

```bash
cat > /etc/systemd/system/ve-deagg.service << 'SVC'
[Unit]
Description=VE.Direct De-Aggregator
After=network.target

[Service]
ExecStart=/usr/bin/python3 /data/vedirect_deaggregator.py \
    --port /dev/ttyACM3 --baud 19200
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SVC

systemctl enable ve-deagg
systemctl start ve-deagg
```

Then in Venus OS:
- **Settings -> I/O -> VE.Direct ports**
- Add each `/dev/pts/N` as a separate VE.Direct port
- Venus OS recognises each MPPT as an independent device

### Alternative: symlinks for persistent paths

```bash
ln -sf /dev/pts/3 /dev/ttyVE_K6QK4
ln -sf /dev/pts/4 /dev/ttyVE_AVWNQ
```

---

## Compatibility

| Firmware | Compatible | Notes |
|----------|-----------|-------|
| `readtext` (Mega/Teensy) | yes | Text blocks only, no commands |
| `readtext_sendhex` (Mega/Teensy) | yes | HEX/SET replies are filtered out |

The de-aggregator uses the same block parsing logic as `ve_aggregator.py`.

---

## Limitations

- **Reverse SET/HEX routing:** Not implemented. The de-aggregator does not
  currently forward commands from Venus OS to the aggregator. Technically possible
  -- each virtual port master fd could be monitored for incoming writes and
  translated to `SET <SER#>` / `HEX <SER#>` commands on the upstream port.

- **Virtual ports are not persistent:** `/dev/pts/N` changes on each restart.
  Use symlinks or udev rules for stable paths.

- **Cerbo GX:** Has its own VE.Direct ports; the de-aggregator runs on an
  upstream RPi or directly on the Cerbo GX if Python3 is available there.

---

## System diagram with Cerbo GX

```
Solar panel --> MPPT 1 --> Mega 2560 / Teensy 4.1
Solar panel --> MPPT 2 -->   (readtext_sendhex)
Solar panel --> MPPT 3 -->      |
                                | USB (/dev/ttyACM3)
                                | or serial -- extendable via RS-485
                                |   (/dev/ttyS0, /dev/ttyUSB0, ...)
                         RPi / Cerbo GX
                                |
                    vedirect_deaggregator.py
                                |
                    +-----------+-----------+
                 /dev/pts/3  /dev/pts/4  /dev/pts/5
                    |           |           |
                  Venus OS -- MPPT 1, 2, 3 visible individually
                  VRM Portal -- all data online
```

The Cerbo GX sees each MPPT as an independent device and monitors power,
errors and statistics per unit.

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
