# VE.Direct Aggregator — Technische Spezifikation

**Firmware · Arduino Mega 2560 / Teensy 4.1 · v2.0 · 2026**

---

## Überblick

Der VE.Direct Aggregator liest mehrere Victron VE.Direct-Geräte gleichzeitig aus und führt ihre Datenströme zu einem einzigen seriellen Ausgang zusammen. Der Ausgang ist ein normaler sequenzieller VE.Direct-Textstrom — Standardformat, mehrere Geräte auf einer Verbindung.

**Unterstützte Geräte:** alle Geräte mit VE.Direct-Textprotokoll — MPPT-Solarladeregler, BMV-Batteriemonitor, SmartShunt, Phoenix-Wechselrichter, Blue Smart Charger, Orion DC/DC-Wandler.

**Leistungssteuerung** (nur `readtext_sendhex`): ein bidirektionaler Befehlskanal ermöglicht das Begrenzen der Ladeleistung von MPPT-Reglern. Befehle werden durch Kaskaden geroutet, Antworten werden zurückgeleitet.

**Vorgesehener Empfänger:** ein Linux-System (z.B. Raspberry Pi) das Geräte anhand von `SER#` identifiziert. **Nicht direkt kompatibel** mit Cerbo GX / Venus GX (diese erwarten ein Gerät pro VE.Direct-Port) -- aber mit `vedirect_deaggregator.py` nutzbar.

---

## Firmware-Varianten

| Datei | Hardware | Eingänge | Ausgang | Funktionen |
|-------|----------|----------|---------|------------|
| `vedirect_readtext.ino` | Mega 2560 | 3 | TX0 / USB | Text-Aggregation |
| `vedirect_readtext_sendhex.ino` | Mega 2560 | 3 | TX0 / USB | Text + SET + HEX |
| `vedirect_readtext_teensy41.ino` | Teensy 4.1 | 7 | TX8 oder USB | Text-Aggregation |
| `vedirect_readtext_sendhex_teensy41.ino` | Teensy 4.1 | 7 | TX8 oder USB | Text + SET + HEX |

Alle Ports werden identisch behandelt — direkte Geräte und vorgelagerte Aggregatoren liefern beide gültige VE.Direct-Blöcke.

**Mega TX0 / USB:** Serial0 TX (Pin 1) und USB teilen denselben Chip — beide Verbindungen funktionieren mit identischer Firmware. USB-Kabel → Host sieht `/dev/ttyUSB0` oder `/dev/ttyACM0`.

**Teensy Ausgang:** gesteuert durch `#define OUTPUT_USB` im Firmware-Header:
```c
#define OUTPUT_USB 0   // TX8-Pin
#define OUTPUT_USB 1   // SerialUSB nativ → /dev/ttyACM0
```

---

## Baudraten-Konstanten

| Konstante | Wert | Verwendung |
|-----------|------|------------|
| `BAUD_VEDIRECT` | 19200 | Direkte VE.Direct-Geräte — fest durch Victron |
| `BAUD_UPSTREAM` | 115200 | MCU-zu-MCU Verbindungen in Kaskaden |
| `BAUD_OUT` | 19200 | Ausgang — auf `BAUD_UPSTREAM` für Kaskadenausgang |

Einzelne Eingänge per `port_baud[]` konfigurierbar:
```c
uint32_t port_baud[3] = {BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_UPSTREAM};
```

---

## Ausgabeformat

Normaler sequenzieller VE.Direct-Textstrom. Jeder Block wird sofort gesendet wenn die `Checksum\t`-Zeile empfangen wird — ein Block nach dem anderen, keine Vermischung.

```
PID\t0xA060\r\n
FW\t174\r\n
SER#\tHQ2529K6QK4\r\n
V\t52010\r\n
...
Checksum\t<Byte>\r\n
PID\t0xA060\r\n       ← nächster Block beginnt sofort
...
```

**Blockende:** `Checksum\t`-Zeile — kein doppeltes `\n`, kein Trennzeichen zwischen Blöcken.

**ALIVE-Signal:** `ALIVE\r\n` wird gesendet wenn `ALIVE_TIMEOUT` ms (Standard 10s) kein Block gesendet wurde. Signalisiert dass der MCU läuft aber keine Gerätedaten vorliegen.

**Geräteidentifikation:** Empfänger müssen `SER#` kombiniert verwenden. Mehrere Geräte gleichen Typs teilen dieselbe `PID`.

**Zirkuläre TX-Queue:** fertige Blöcke werden sofort in eine Queue (12 Slots à 300 Bytes) eingereiht und der Eingangspuffer sofort freigegeben. Der Sender arbeitet die Queue nichtblockierend ab — kein Block geht durch Sendedruck verloren, auch bei gleichzeitigen Blöcken mehrerer Ports oder Bursts von Upstream-Aggregatoren.

### Empfängerkompatibilität

| Empfänger | Kompatibel | Hinweis |
|-----------|-----------|---------|
| `ve_aggregator` Python-Modul | ✓ | Vollständige Unterstützung |
| Beliebiger VE.Direct-Parser | ✓ | Standardformat, ignoriert ALIVE |
| Cerbo GX / Venus GX | via Deagg. | `vedirect_deaggregator.py` erforderlich |

---

## Funktionen

### Mehrkanal-Eingang

Alle Hardware-UARTs werden gleichzeitig abgefragt. Jeder Port puffert einen vollständigen Block unabhängig (`BUF_SIZE` = 300 Bytes). Ein langsames oder fehlendes Gerät blockiert die anderen nicht.

### Blockend-Erkennung und Queue

Bei jedem empfangenen `\n` wird die aktuelle Zeile mit `"Checksum\t"` verglichen. Bei Treffer wird der Block sofort in die TX-Queue kopiert und der Eingangspuffer zurückgesetzt — der Port kann unmittelbar den nächsten Block einlesen.

### Kaskadierfähigkeit

MCUs können ohne Codeänderungen in Stern- oder Kaskadenform verbunden werden. MCUs ohne angeschlossene Geräte leiten alles transparent durch.

### Leistungssteuerung (readtext_sendhex)

SET-Befehle auf dem Ausgangs-RX werden in VE.Direct HEX übersetzt und an den passenden MPPT-Regler gesendet. Nur der betroffene Port pausiert (~50–100 ms). Alle anderen Ports laufen weiter.

### Routing (readtext_sendhex)

Jeder MCU lernt SER# und PID jedes direkt angeschlossenen Geräts aus den durchlaufenden Blocks. Befehle werden primär nach SER# geroutet, PID ist Fallback -- dadurch werden Geräte mit gleicher PID korrekt unterschieden. Für Upstream-/Kaskaden-Geräte wird zusätzlich eine Route-Tabelle gepflegt (`MAX_ROUTES` = 12 pro Port) mit PIDs die auf dem jeweiligen Port ankommen. Unbekannte Identifier werden auf allen Ports weitergeleitet. Einträge verfallen nach `PID_TIMEOUT` ms (Standard 10s), SER# wird gleichzeitig gelöscht.

---

## Gerätekompatiblität

| Gerät | Text-Stream | HEX-Durchleitung | SET-Befehl |
|-------|-------------|------------------|------------|
| MPPT Solarladeregler | ✓ | ✓ vollständig | ✓ |
| BMV-Batteriemonitor | ✓ | ✓ lesen/schreiben | — |
| SmartShunt | ✓ | ✓ lesen | — |
| Phoenix-Wechselrichter | ✓ | ✓ Ein/Aus, Modus | — |
| Blue Smart Charger | ✓ | ✓ Modus, Strom | — |
| Orion DC/DC-Wandler | ✓ | ✓ Ein/Aus | — |

---

## Topologien

### Direkt

```
Gerät 1 ──► [Mega] RX1 ──► Ausgang      Gerät 1 ──► [Teensy] RX1
Gerät 2 ──► [Mega] RX2                  ...
Gerät 3 ──► [Mega] RX3                  Gerät 7 ──► [Teensy] RX7 ──► Ausgang
```

### Stern

```
Gerät 1─3 ──► [Mega] ──┐
Gerät 4─6 ──► [Mega] ──┼──► [Mega] ──► Ausgang
Gerät 7─9 ──► [Mega] ──┘
```

Bei `readtext_sendhex` sind die Verbindungen bidirektional — SET-Befehle vorwärts, Antworten zurück.

### Kaskade (115200 Baud Ausgang)

```
Gerät 1─3 ──► [Mega] ──► [Mega] ──► [Mega] ──► [Mega] ──► Ausgang
               3            6            9           12 Blöcke
```

### Gemischte Topologie

Mega und Teensy können frei kombiniert werden. Signalpegel: Mega-Ausgang (5V) → Teensy-Eingang braucht BSS138. Teensy-Ausgang (3.3V) → Mega-Eingang braucht keinen Pegelwandler.

```
Gerät 1─7  ──► [Teensy] ──┐
Gerät 8─10 ──► [Mega]   ──┼──► [Mega] ──► Ausgang (115200 Baud)
Gerät 11─13──► [Mega]   ──┘
```

---

## Timing-Budget

### Ausgang 19200 Baud

| Geräte | Zeit | Auslastung | Reserve |
|--------|------|------------|---------|
| 1 | ~83 ms | 8 % | 917 ms |
| 3 | ~249 ms | 25 % | 751 ms |
| 7 | ~581 ms | 58 % | 419 ms |
| 9 | ~747 ms | 75 % | 253 ms |
| 12 | ~996 ms | ~100 % ⚠ | — |

### Ausgang 115200 Baud

| Geräte | Zeit | Auslastung | Reserve |
|--------|------|------------|---------|
| 9 | ~126 ms | 13 % | 874 ms |
| 13 | ~182 ms | 18 % | 818 ms |
| 21 | ~294 ms | 29 % | 706 ms |
| 49 | ~686 ms | 69 % | 314 ms |

---

## Pinbelegung

### VE.Direct Stecker (JST-PH 2 mm, 4-polig)

| Pin | Signal | Anschluss |
|-----|--------|-----------|
| 1 | GND | MCU GND |
| 2 | RX (Geräteeingang) | MCU TX — nur readtext_sendhex |
| 3 | TX (Geräteausgang) | MCU RX (Teensy: über BSS138) |
| 4 | +5V | max 10 mA — nicht zur MCU-Versorgung |

### Arduino Mega 2560 — alle Varianten

| Pin | Signal |
|-----|--------|
| RX1 (19) | Gerät / Upstream 1 TX → Serial1 |
| RX2 (17) | Gerät / Upstream 2 TX → Serial2 |
| RX3 (15) | Gerät / Upstream 3 TX → Serial3 |
| TX0 (1) | Ausgang (auch per USB) |

### Arduino Mega 2560 — readtext_sendhex Ergänzungen

| Pin | Signal |
|-----|--------|
| TX1 (18) | Gerät 1 RX — HEX-Befehle |
| TX2 (16) | Gerät 2 RX — HEX-Befehle |
| TX3 (14) | Gerät 3 RX — HEX-Befehle |
| RX0 (0) | SET/HEX Befehlseingang |

### Teensy 4.1 — readtext

| Pin | Signal |
|-----|--------|
| RX1–RX7 | Gerät 1–7 TX ⚠ BSS138 erforderlich |
| TX8 / USB | Ausgang |

### Teensy 4.1 — readtext_sendhex Ergänzungen

| Pin | Signal |
|-----|--------|
| TX1–TX7 | Gerät 1–7 RX ⚠ BSS138 erforderlich |
| RX8 | SET/HEX Befehlseingang |

---

## Stromversorgung

VE.Direct Pin 4 liefert max. 10 mA — nicht zur MCU-Versorgung (~100 mA benötigt).

Versorgung über:
- DC/DC-Wandler direkt an der Batterie (8–60V Eingang, 5V Ausgang)
- 5V USB-Netzteil
- USB-Kabel vom Host

Externer Strom und USB können gleichzeitig angeschlossen sein — der Mega schaltet automatisch auf die höhere Spannung. Der USB-Datenpfad bleibt aktiv.

---

## Technische Daten

| Parameter | Mega 2560 | Teensy 4.1 |
|-----------|-----------|------------|
| Takt | 16 MHz | 600 MHz |
| Logikpegel | 5V | 3.3V |
| Pegelwandler | Nein | BSS138 pro RX (+ TX bei sendhex) |
| Hardware-UARTs | 4 | 8 |
| Max. Geräteeingänge | 3 | 7 |
| Blockend-Erkennung | `Checksum\t`-Zeile | `Checksum\t`-Zeile |
| TX-Queue | 12 Slots à 300 Bytes | 12 Slots à 300 Bytes |
| HW UART RX-Buffer | 256 Bytes (~107ms) | 1024 Bytes (~427ms) |
| Geräte-Schlüssel | `SER#` | `SER#` |
| ALIVE-Signal | nach `ALIVE_TIMEOUT` (10s) | nach `ALIVE_TIMEOUT` (10s) |
| HEX-Busy-Scope | pro Port | pro Port |
| RX-Puffer pro Port | 300 Bytes | 300 Bytes |
| `MAX_ROUTES` | 12 pro Port | 12 pro Port |
| `PID_TIMEOUT` | 10s (konfigurierbar) | 10s (konfigurierbar) |
| `VBAT_FALLBACK` | 24V (konfigurierbar) | 24V (konfigurierbar) |

---

## Python-Tools

| Datei | Zweck |
|-------|-------|
| `ve_aggregator.py` | Client-Modul — Blöcke lesen, SET/HEX senden |
| `mppt_read_example.py` | Daten anzeigen mit Intervall pro Gerät |
| `block_monitor.py` | Block-Timing, Gap-Messung pro Gerät |
| `sendhex_test.py` | SET/HEX Befehle testen |
| `powerset_example.py` | Leistungsregelung mit Spannungsrampe |
| `vedirect_simulator.py` | Upstream-Aggregator simulieren (N MPPTs auf einem Port) |

---

## Einschränkungen

- Mehrere Geräte gleichen Typs teilen dieselbe PID — Empfänger müssen `SER#` kombinieren
- `VBAT_FALLBACK` wird verwendet bis der erste Vbat-Wert empfangen wird
- VE.Direct Pin 4 max. 10 mA — MCU muss extern versorgt werden
- Cerbo GX / Venus GX nicht direkt nutzbar -- mit `vedirect_deaggregator.py` aber voll integrierbar
- Teensy 4.1: BSS138 an allen RX-Eingängen erforderlich (+ TX bei readtext_sendhex)
- Geräte (SER# + PID) verfallen nach `PID_TIMEOUT` -- Gerätewechsel werden innerhalb ~1s erkannt

---

## De-Aggregation und Venus OS / Cerbo GX

Der aggregierte Stream kann mit `vedirect_deaggregator.py` wieder in einzelne
virtuelle serielle Ports aufgeteilt werden. Jeder MPPT erscheint als eigener
`/dev/pts/N` Port, den Venus OS und Cerbo GX als eigenständiges VE.Direct-Gerät
einbinden können.

Siehe `deaggregator_spec.md` für die Einrichtung.


### WHO / Firmware-Identifikation

Bei Empfang von `WHO\n` antwortet die Firmware mit:

```
READTEXT Mega2560 N=3\n
READTEXT Teensy41 N=7\n
SENDHEX Mega2560 N=3\n    (readtext_sendhex Variante)
SENDHEX Teensy41 N=7\n
```

`ve_aggregator.py` sendet `WHO\n` automatisch 1s nach Verbindungsaufbau
und gibt `firmware: <Antwort>` aus. So erkennt der Host welche
Firmware-Variante und Portanzahl aktiv ist.

### RESET-Kommando

Alle Firmware-Varianten reagieren auf `RESET\n`:

1. `RESET\n` an alle nachgelagerten Ports weiterleiten (`forward_all`)
2. 50ms warten bis die Übertragung abgeschlossen ist
3. Watchdog-Reset auslösen -- MCU startet innerhalb von 15ms neu

Das Kommando propagiert durch die gesamte Kaskade. Senden von beliebigem Host:

```bash
echo "RESET" > /dev/ttyACM3
# oder über RS-485:
echo "RESET" > /dev/ttyUSB0
```

---

## DS18B20 Temperatursensor (optional)

Ein oder mehrere DS18B20 1-Wire-Sensoren können an einem einzigen Digital-Pin
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
Venus OS sieht jeden Sensor als eigenständiges Gerät.

**Verdrahtung (3-adrig, beliebig viele Sensoren an einem Pin):**
- VCC -> 5V
- GND -> GND
- DATA -> TEMP_PIN, mit 4.7k Pull-up zwischen 5V und DATA

**Konfiguration in der Firmware:**
```c
#define TEMP_ENABLE   1      // 0 = deaktiviert
#define TEMP_PIN      2      // Digital-Pin für 1-Wire DATA
#define TEMP_INTERVAL 5000   // Ausleseintervall in ms
```

Kein Sensor angeschlossen -> `temp_count = 0` -> keine Blocks, kein Overhead.

**Benötigte Libraries:** OneWire + DallasTemperature (Arduino Library Manager).

### Remote-Firmware-Update über RS-485

Der Mega kann ohne Ausbau aus der Installation aktualisiert werden:

1. `avrdude` auf dem RPi starten -- über die bestehende Verbindung (USB oder RS-485):

```bash
avrdude -p atmega2560 -c arduino -P /dev/ttyUSB0 -b 115200 \
        -U flash:w:firmware.hex:i
```

2. Während `avrdude` läuft den Reset-Knopf am Mega drücken

`avrdude` versucht ~10 Sekunden lang zu verbinden -- der Reset-Knopf kann
jederzeit in diesem Fenster gedrückt werden. Der Bootloader startet und
`avrdude` flasht die neue Firmware automatisch.

Mit dem `RESET`-Kommando (readtext_sendhex Firmware) kann Schritt 3
automatisiert werden -- der RPi sendet `RESET\n` und löst den Bootloader
ohne physischen Zugang aus.

---

## Hardware De-Aggregator (alternativer Ansatz)

Für Installationen ohne Linux-Host implementiert das
[pico_vedirect](https://github.com/E-t0m/ve.direct-aggregator/tree/main/hardware_deagg) Projekt
die De-Aggregation direkt in Hardware: ein oder mehrere Raspberry Pi
Pico Boards hängen am RS-485-Bus mit dem aggregierten Stream, trennen
eingehende Blöcke nach `SER#` und stellen jedes Gerät als eigenständigen
CDC-ACM seriellen Port direkt am Cerbo GX per USB bereit.

Mehrere Picos organisieren sich selbst am selben Bus (Cluster-Koordination
über Frames in den Inter-Block-Pausen) und skalieren über das 7-Port-Limit
eines einzelnen Pico hinaus. Der Ansatz ist read-only -- HEX-Befehle von
Venus OS werden verworfen, der VE.Direct-Treiber fällt automatisch in den
Text-Modus zurück.

Diesen Ansatz nutzen wenn kein RPi/Linux-Host zwischen Aggregator und
Cerbo GX verfügbar ist. `vedirect_deaggregator.py` (dieses Repo) nutzen
wenn bereits ein Linux-Host in der Kette vorhanden ist.
