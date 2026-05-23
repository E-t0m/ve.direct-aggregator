# VE.Direct De-Aggregator — Spec v2.0

**`vedirect_deaggregator.py`** — liest den aggregierten VE.Direct Stream
vom Mega 2560 / Teensy 4.1 und stellt jeden MPPT als eigenen virtuellen
seriellen Port bereit.

---

## Zweck

Der Aggregator fasst N MPPT-Streams auf einem seriellen Port zusammen.
Der De-Aggregator trennt diesen Stream wieder auf — pro Gerät ein
`/dev/pts/N` — so dass Venus OS / Cerbo GX jeden MPPT einzeln sieht.

```
MPPT 1 ─────┐
MPPT 2 ──── Mega 2560 ── USB oder RS-485 ── De-Aggregator ── /dev/pts/3 → Venus OS
MPPT 3 ─────┘                                           └── /dev/pts/4 → Venus OS
                                                         └── /dev/pts/5 → Venus OS
```

---

## Voraussetzungen

```bash
pip install pyserial --break-system-packages
apt install socat        # optional, für manuelle Tests
```

---

## Verwendung

```bash
# Grundaufruf
python3 vedirect_deaggregator.py

# Mit optionen
python3 vedirect_deaggregator.py \
	--port /dev/ttyACM3 \
	--baud 19200 \
	--label HQ2529K6QK4=Dach \
	--label HQ2529AVWNQ=Garage
```

Ausgabe beim Start:
```
10:00:01.123  ve_deaggregator v2.0 — opening /dev/ttyACM3 at 19200 baud
10:00:01.456  connected — waiting for blocks...

─── virtual port map ────────────────────────────────────
  /dev/pts/3  ←→  HQ2529K6QK4  (HQ2529K6QK4)
  /dev/pts/4  ←→  HQ2529AVWNQ  (HQ2529AVWNQ)
────────────────────────────────────────────────────────
```

---

## Optionen

| Option | Standard | Beschreibung |
|--------|----------|--------------|
| `--port` | `/dev/ttyACM3` | Serieller Port des Aggregators |
| `--baud` | `19200` | Baudrate |
| `--max` | `16` | Maximale Anzahl virtueller Ports |
| `--label SER=NAME` | — | Benutzerdefinierter Name pro Gerät |

---

## Protokoll

Der De-Aggregator liest den Stream und filtert:
- HEX-Frames (`:...`) → verworfen
- `ALIVE` → verworfen
- `OK`, `ERR`, `HEX_REPLY`, `READTEXT`, `SENDHEX` → verworfen
- Vollständige Text-Blöcke (`PID`…`Checksum`) → geroutet

Routing nach `SER#`. Jedes neue Gerät bekommt
automatisch einen neuen virtuellen Port.

---

## Venus OS Integration

### Raspberry Pi mit Venus OS

```bash
# De-Aggregator als systemd-Service einrichten:
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

Danach in Venus OS:
- **Settings → I/O → VE.Direct ports**
- Jeden `/dev/pts/N` als separaten VE.Direct Port hinzufügen
- Venus OS erkennt jeden MPPT als eigenständiges Gerät

### Alternativer Ansatz: symlinks

```bash
# Feste Gerätepfade via symlinks
ln -sf /dev/pts/3 /dev/ttyVE_K6QK4
ln -sf /dev/pts/4 /dev/ttyVE_AVWNQ
```

---

## Interoperabilität mit readtext / readtext_sendhex

| Firmware | Kompatibel | Hinweis |
|----------|-----------|---------|
| `readtext` (Mega/Teensy) | ✓ | Nur Text-Blöcke, keine Befehle |
| `readtext_sendhex` (Mega/Teensy) | ✓ | HEX/SET-Replies werden gefiltert |

Der De-Aggregator nutzt dieselbe `_parse_block` Logik wie `ve_aggregator.py`.

---

## Einschränkungen

- **Rückwärtiges SET/HEX Routing:** Nicht implementiert. Der De-Aggregator
  leitet Befehle von Venus OS nicht an den Aggregator weiter. Technisch moeglich
  -- jeder virtuelle Port-Master-FD koennte auf eingehende Schreibzugriffe
  ueberwacht und als `SET <SER#>` / `HEX <SER#>` Befehl weitergeleitet werden.

- **Virtuelle Ports nicht persistent:** `/dev/pts/N` ändert sich bei
  jedem Start. Symlinks oder udev-Regeln für persistente Pfade verwenden.

- **Cerbo GX:** Hat eigene VE.Direct-Ports; der De-Aggregator läuft auf
  einem vorgelagerten RPi oder direkt auf dem Cerbo GX wenn dort Python3
  verfügbar ist.

---

## Zusammenspiel mit Cerbo GX

```
Solarpanel ──► MPPT 1 ──► Mega 2560
Solarpanel ──► MPPT 2 ──►   (readtext_sendhex)
Solarpanel ──► MPPT 3 ──►      │
                                │ USB (/dev/ttyACM3)
                                │ oder seriell -- erweiterbar via RS-485
                                │   (/dev/ttyS0, /dev/ttyUSB0, ...)
                         RPi / Cerbo GX
                                │
                    vedirect_deaggregator.py
                                │
                    ┌───────────┼───────────┐
                 /dev/pts/3  /dev/pts/4  /dev/pts/5
                    │           │           │
                  Venus OS — MPPT 1, 2, 3 einzeln sichtbar
                  VRM Portal — alle Daten online
```

Der Cerbo GX sieht jeden MPPT als eigenständiges Gerät und kann
Leistungsdaten, Fehler und Statistiken einzeln überwachen.

---

## DS18B20 Temperatursensor (optional)

Ein oder mehrere DS18B20 1-Wire-Sensoren koennen an einem einzigen Digital-Pin
angeschlossen werden (`TEMP_PIN`, Standard D2). Jeder Sensor wird als
Pseudo-VE.Direct-Block ausgegeben:

```
PID     0x9999
SER#    TEMP-P2-S0
FW      100
TEMP    23.50
Checksum  <byte>
```

Der De-Aggregator erstellt automatisch einen virtuellen Port pro Sensor.
Venus OS sieht jeden Sensor als eigenstaendiges Geraet.

**Verdrahtung (3-adrig, beliebig viele Sensoren an einem Pin):**
- VCC -> 5V
- GND -> GND
- DATA -> TEMP_PIN, mit 4.7k Pull-up zwischen 5V und DATA

**Konfiguration in der Firmware:**
```c
#define TEMP_ENABLE   1      // 0 = deaktiviert
#define TEMP_PIN      2      // Digital-Pin fuer 1-Wire DATA
#define TEMP_INTERVAL 5000   // Ausleseintervall in ms
```

Kein Sensor angeschlossen -> `temp_count = 0` -> keine Blocks, kein Overhead.

**Benoetiste Libraries:** OneWire + DallasTemperature (Arduino Library Manager).
