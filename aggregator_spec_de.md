# VE.Direct Aggregator — Technische Spezifikation

**Firmware · Arduino Mega 2560 / Teensy 4.1 · v1.0 · 2026**

---

## Überblick

Der VE.Direct Aggregator ist Firmware für den Arduino Mega 2560 oder Teensy 4.1 mit zwei unabhängigen Funktionsbereichen:

**Text-Aggregation:** Mehrere Victron VE.Direct-Geräte werden eingelesen und zu einem einzigen seriellen Ausgang zusammengeführt — ein normaler sequenzieller VE.Direct-Textstrom. Alle Geräte mit VE.Direct-Textprotokoll werden ohne Codeänderung unterstützt: MPPT-Solarladeregler, BMV-Batteriemonitor, SmartShunt, Phoenix-Wechselrichter, Blue Smart Charger und Orion DC/DC-Wandler.

**Leistungssteuerung:** Ein bidirektionaler Befehlskanal ermöglicht das Setzen der maximalen Ladeleistung einzelner oder aller MPPT-Regler (nur `readtext_sendhex`). Befehle werden durch Kaskaden geroutet, Bestätigungen werden zurückgeleitet. Nur der Port des betroffenen Geräts pausiert während eines HEX-Befehls (~50–100 ms typisch) — alle anderen Ports laufen ungestört weiter.

**Vorgesehener Empfänger:** ein Linux-System (z.B. Raspberry Pi) mit einem Parser der Geräte anhand ihres `PID`-Feldes identifiziert. Der Ausgang ist **nicht kompatibel** mit Cerbo GX / Venus GX als direktem Empfänger — diese erwarten genau ein Gerät pro VE.Direct-Port und ignorieren weitere PIDs auf demselben Port.

---

## Firmware-Varianten

Vier Varianten je nach benötigtem Funktionsumfang und Zielhardware:

| Datei | Hardware | Eingänge | Ausgang | Funktionen |
|-------|----------|----------|---------|------------|
| `vedirect_readtext.ino` | Mega 2560 | 3 | TX0 / USB | Text-Aggregation |
| `vedirect_readtext_sendhex.ino` | Mega 2560 | 3 | TX0 / USB | Text-Aggregation + SET + HEX |
| `vedirect_readtext_teensy41.ino` | Teensy 4.1 | 7 | TX8 oder USB | Text-Aggregation |
| `vedirect_readtext_sendhex_teensy41.ino` | Teensy 4.1 | 7 | TX8 oder USB | Text-Aggregation + SET + HEX |

Alle Ports aller Varianten werden identisch behandelt — direkte VE.Direct-Geräte und vorgelagerte Aggregatoren senden beide gültige VE.Direct-Blöcke und werden gleich behandelt.

**Mega TX0 / USB:** Serial0 TX (Pin 1) und der USB-Port teilen denselben Chip (16U2 oder CH340G) — beide Verbindungen funktionieren mit identischer Firmware. Einfach ein USB-Kabel für die direkte Host-Verbindung einstecken, oder TX0 extern verdrahten für lange Leitungen. Der Host sieht `/dev/ttyUSB0` oder `/dev/ttyACM0`. `BAUD_OUT` muss auf beiden Seiten übereinstimmen.

**Teensy Ausgangsauswahl:** gesteuert durch `#define OUTPUT_USB` im Firmware-Header:
```c
#define OUTPUT_USB 0   // TX8-Pin — serieller Ausgang
#define OUTPUT_USB 1   // SerialUSB — native USB, Host sieht /dev/ttyACM0
```

**`readtext_sendhex` überall verwenden:** Da `readtext_sendhex` ein Superset von `readtext` ist — der SET/HEX-Kanal tut nichts wenn er nicht benutzt wird — kann es auf alle MCUs einer Topologie geflasht werden ohne funktionalen Unterschied.

---

## Funktionen

### Mehrkanal-Eingang

Alle Hardware-UARTs werden gleichzeitig abgefragt. Jeder Kanal puffert einen vollständigen VE.Direct-Block unabhängig — ein langsames oder zeitweise fehlendes Gerät blockiert die anderen nicht. Alle Ports werden identisch behandelt, unabhängig davon was angeschlossen ist.

### Serieller Ausgang

Blöcke werden sofort gesendet sobald sie vollständig sind, eines nach dem anderen — erster fertiger Port gewinnt. Wenn mehrere Ports gleichzeitig fertig werden, werden die anderen in die Warteschlange gestellt. Datenvermischung ist nicht möglich.

### Kaskadierfähigkeit

Mehrere Aggregatoren können ohne Codeänderungen in Stern- oder Kaskadenform verbunden werden. MCUs ohne angeschlossene Geräte funktionieren als transparente Relais. Die einzige praktische Grenze ist das Timing relativ zum 1-Sekunden-Sendeintervall der Geräte.

### Leistungssteuerung per SET-Befehl

*(nur `readtext_sendhex`)*

Der MCU empfängt SET-Befehle auf dem RX des Ausgangsports, übersetzt sie in VE.Direct HEX-Befehle und sendet sie an den MPPT-Regler mit der passenden PID über den TX-Pin dieses Ports. Nur der Text-Stream des betroffenen Ports pausiert (~50–100 ms typisch). Alle anderen Ports laufen ungestört weiter.

**Befehlsformat:**
```
SET <pid> <watt>\n      einzelnen Regler per PID begrenzen
SET ALL <watt>\n        alle Regler gleichzeitig begrenzen
```

**Antwortformat:**
```
OK <pid> <watt>W <ampere>A\n    Einstellung per Rücklesung bestätigt
ERR <pid> timeout\n             kein HEX-ACK innerhalb 1s
ERR <pid> verify set=XA rb=YA\n Rücklesewert stimmt nicht
```

### Beliebiger HEX-Durchleitungskanal

*(nur `readtext_sendhex`)*

Der Host kann beliebige VE.Direct HEX-Strings direkt per PID an ein Gerät senden. Die Firmware leitet sie unverändert ohne Parsing oder Validierung weiter. Die Antwort wird mit der PID präfixiert zurückgegeben. Der Host ist für korrekte HEX-Formatierung inkl. Prüfsumme verantwortlich und muss bei Bedarf selbst in den Text-Modus zurückschalten (`HEX <pid> :154\n`).

**Befehlsformat:**
```
HEX <pid> <hex_string>\n    an beliebiges direktes VE.Direct-Gerät senden
HEX ALL <hex_string>\n      an alle direkten Geräte senden
```

**Antwortformat:**
```
HEX_REPLY <pid> :<hex_antwort>\n
ERR <pid> timeout\n
```

### Automatische Watt→Ampere-Umrechnung

*(nur `readtext_sendhex`)*

Der MCU lernt die aktuelle Batteriespannung (`Vbat`) aus dem laufenden Text-Stream und rechnet den Wattwert eines SET-Befehls automatisch in Ampere mit 0.1A-Auflösung um:

```
A    = Watt / Vbat                   (float, z.B. 19.5 A)
reg  = round(A × 10)                 (Register 0x2015, Einheit 0.1A)
```

Bis zum ersten empfangenen Vbat-Wert wird eine konfigurierbare Fallback-Spannung verwendet (`VBAT_FALLBACK`, Standard 24V).

### PID-basiertes Routing in Kaskaden

*(nur `readtext_sendhex`)*

Jeder MCU lernt die PIDs aller Geräte aus dem durchlaufenden Text-Stream. Bekannte PIDs werden gezielt an den richtigen Port weitergeleitet. Unbekannte PIDs werden auf allen Ports gesendet (Fallback). Antworten von vorgelagerten MCUs werden auf dem Ausgangs-TX zurückgeleitet. PIDs verfallen nach `PID_TIMEOUT` ms Inaktivität (Standard 10s).

---

## Gerätekompatiblität

Der Aggregator ist ein universeller VE.Direct-Stream-Aggregator — er puffert vollständige Blöcke und leitet sie weiter ohne Feldnamen oder Werte zu interpretieren. Jedes Gerät mit VE.Direct-Textprotokoll funktioniert ohne Codeänderung.

| Gerät | Text-Stream | HEX-Durchleitung | SET-Befehl |
|-------|-------------|------------------|------------|
| MPPT Solarladeregler | ✓ | ✓ vollständig | ✓ |
| BMV-Batteriemonitor (600/700/702/712) | ✓ | ✓ lesen/schreiben | — |
| SmartShunt | ✓ | ✓ lesen | — |
| Phoenix-Wechselrichter | ✓ | ✓ Ein/Aus, Modus | — |
| Blue Smart Charger (AC) | ✓ | ✓ Modus, Strom | — |
| Orion DC/DC-Wandler | ✓ | ✓ Ein/Aus | — |

**SET-Befehl** verwendet Register `0x2015` (Charge Current Limit) — MPPT-spezifisch. SET an eine Nicht-MPPT-PID wird upstream weitergeleitet oder gibt `ERR timeout` zurück.

Verschiedene Gerätetypen können gleichzeitig an verschiedenen Ports betrieben werden — der Aggregator unterscheidet nicht zwischen ihnen.

---

## Unterstützte Topologien

### Direkt — bis zu 3 Geräte an 1 Mega, bis zu 7 an 1 Teensy

```
Gerät 1 ─TX──► [Mega/Teensy] RX1   ──► Ausgang
Gerät 2 ─TX──► [Mega/Teensy] RX2
Gerät 3 ─TX──► [Mega/Teensy] RX3
...
```

### Stern — bis zu 9 Geräte, 3 Megas an 1 zentralen Mega

```
Gerät 1─3 ──► [Mega] ──TTL──┐
Gerät 4─6 ──► [Mega] ──TTL──┼──► [Mega] ──► Ausgang
Gerät 7─9 ──► [Mega] ──TTL──┘
```

Bei `readtext_sendhex` ist jede TTL-Verbindung bidirektional — SET-Befehle fließen vorwärts, OK/ERR-Antworten zurück.

### Kaskade — bis zu 12 Geräte bei 115200 Baud Ausgang

```
Gerät 1─3 ──► [Mega] ──► [Mega] ──► [Mega] ──► [Mega] ──► Ausgang
               3 Blöcke   6 Blöcke   9 Blöcke   12 Blöcke
```

### Relais-Kaskade — leere MCUs als Repeater

```
Gerät 1─3 ──► [Mega] ──► [Mega leer] ──► [Mega leer] ──► Ausgang
```

MCUs ohne angeschlossene Geräte leiten allen Traffic transparent durch. Nützlich wenn Kabellängen zwischen MCUs ~5m TTL-Reichweite überschreiten.

### Gemischte Topologie

Mega- und Teensy-Varianten können frei gemischt werden. Das Protokoll ist hardware-agnostisch.

```
Gerät 1─7  ──► [Teensy] ──TTL──┐
Gerät 8─10 ──► [Mega]   ──TTL──┼──► [Mega] ──► Ausgang
Gerät 11─13──► [Mega]   ──TTL──┘
```

**Signalpegel beim Mischen:** Mega arbeitet mit 5V TTL, Teensy mit 3.3V. Mega-Ausgang → Teensy-Eingang: BSS138-Pegelwandler erforderlich. Teensy-Ausgang → Mega-Eingang: kein Pegelwandler nötig (Mega RX ist 5V-tolerant).

**Hinweis:** 13 Geräte × ~83 ms = ~1079 ms bei 19200 Baud — überschreitet das 1-Sekunden-Fenster. `BAUD_OUT` auf `115200` setzen.

---

## Ausgabeformat

Der Aggregator gibt einen normalen sequenziellen VE.Direct-Textstrom aus. Jeder Block wird sofort gesendet sobald er vollständig ist — keine Trennzeichen, keine Marker zwischen Blöcken.

```
PID\t0xA053\r\n     ┐
VPV\t18240\r\n      │ Block 1 — sofort gesendet wenn fertig
PPV\t120\r\n        │
...\r\n             │
Checksum\tX\r\n     │
\r\n                ┘
PID\t0xA060\r\n     ┐
...\r\n\r\n         ┘ Block 2 — sobald Block 1 fertig
```

Jeder Block beginnt mit `PID\t...` und endet mit `\r\n\r\n`. Der Empfänger identifiziert Geräte anhand ihres `PID`-Feldes.

**Keine Datenvermischung:** Es wird immer nur ein vollständiger Block auf einmal gesendet. Wenn mehrere Ports gleichzeitig fertig werden, werden sie in die Warteschlange gestellt.

### Empfängerkompatibilität

| Empfänger | Kompatibel | Hinweis |
|-----------|-----------|---------|
| Beliebiger VE.Direct-Textparser | ✓ | Standardformat |
| `ve_aggregator` Python-Modul | ✓ | Vollständige Unterstützung |
| Cerbo GX / Venus GX | ✗ | Erwartet ein Gerät pro Port |

---

## SET-Befehlskanal — HEX-Sequenz

*(nur `readtext_sendhex`)*

```
1.  HEX SET senden:  :8<reg_lo><reg_hi>00<val_lo><val_hi><cs>\n
2.  Auf HEX-ACK warten                          (1s Timeout → ERR timeout)
3.  HEX GET senden:  :7<reg_lo><reg_hi>00<cs>\n
4.  Auf HEX GET-Antwort warten                  (1s Timeout → ERR timeout)
5.  Rücklesewert prüfen                         (Abweichung → ERR verify)
6.  Text-Modus wiederherstellen: :154\n
7.  OK/ERR auf Ausgangs-TX senden
    — NICHT auf Wiederaufnahme des Text-Streams warten
```

**Verwendetes Register: `0x2015` — Charge Current Limit**

| Eigenschaft | Wert |
|-------------|------|
| Register | `0x2015` |
| Einheit | 0.1A |
| Speicher | volatil — unbegrenzte Schreibzyklen |
| Umrechnung | `round(Watt / Vbat × 10)` → Registerwert |

`0x2015` ist `0xEDF0` vorzuziehen — letzteres hat begrenzte Schreibzyklen.

Nur der Port des betroffenen Reglers pausiert während der HEX-Sequenz (~50–100 ms typisch, max. 1s). Alle anderen Ports laufen normal weiter.

Bei `SET ALL`: HEX SET wird gleichzeitig an alle direkten Ports gesendet (Pseudo-Multicast), dann werden Antworten nacheinander verifiziert.

---

## Timing-Budget

VE.Direct sendet einmal pro Sekunde. Ein typischer Block (~200 Bytes) belegt ~83 ms bei 19200 Baud, ~14 ms bei 115200 Baud.

### Ausgang bei 19200 Baud

| Geräte | Sendezeit | Auslastung | Reserve |
|--------|-----------|------------|---------|
| 1 | ~83 ms | 8 % | 917 ms |
| 3 | ~249 ms | 25 % | 751 ms |
| 6 | ~498 ms | 50 % | 502 ms |
| 7 | ~581 ms | 58 % | 419 ms |
| 9 | ~747 ms | 75 % | 253 ms |
| 12 | ~996 ms | ~100 % ⚠ | — |

### Ausgang bei 115200 Baud

| Geräte | Sendezeit | Auslastung | Reserve |
|--------|-----------|------------|---------|
| 9 | ~126 ms | 13 % | 874 ms |
| 13 | ~182 ms | 18 % | 818 ms |
| 21 | ~294 ms | 29 % | 706 ms |
| 53 | ~742 ms | 74 % | 258 ms |

### Optimale Topologie für maximale Geräteanzahl

| Ausgang Baud | Topologie | MCUs | Max. Geräte |
|-------------|-----------|------|-------------|
| 19200 | Mega direkt | 1 | 3 |
| 19200 | Teensy direkt | 1 | 7 |
| 19200 | Mega Stern | 4 | 9 |
| 115200 | Mega Kaskade | 4 | 12 |
| 115200 | Gemischter Stern (1× Teensy + 2× Mega → 1 zentraler) | 4 | 13 |
| 115200 | Teensy Stern (3× Teensy → 1 zentraler) | 4 | 21 |

---

## Hardware & Logikpegel

| Plattform | Logikpegel | Pegelwandler erforderlich |
|-----------|------------|--------------------------|
| Arduino Mega 2560 | 5V | Nein — Direktanschluss |
| Teensy 4.1 | 3.3V | Ja — BSS138 pro RX-Eingang |

**Empfehlung:** BSS138-basiertes bidirektionales 4-Kanal-Modul (oft als "I2C Logic Level Converter" beschriftet). Funktioniert für beliebige Signale bis ~1 MBit/s inkl. UART bei 19200 Baud. Ein Modul deckt 4 RX-Eingänge. Zwei Module decken alle 7 Teensy-Eingänge. Bei `readtext_sendhex` benötigen auch die TX-Pins Pegelwandler (3.3V → 5V).

---

## Pinbelegung

### Arduino Mega 2560 — alle Varianten

| Pin | Signal | Beschreibung |
|-----|--------|--------------|
| RX1 (19) | Gerät / Upstream 1 TX | Serial1 — Text einlesen |
| RX2 (17) | Gerät / Upstream 2 TX | Serial2 — Text einlesen |
| RX3 (15) | Gerät / Upstream 3 TX | Serial3 — Text einlesen |
| TX0 (1) | Ausgang | Serial0 — aggregierter Stream |
| GND | Masse alle Eingänge | gemeinsame Masse |
| 5V | Versorgung | externer DC/DC oder USB |

**USB direkt:** Serial0 TX und der USB-Port teilen denselben Chip. Einfach USB einstecken — Host sieht `/dev/ttyUSB0` oder `/dev/ttyACM0`. `BAUD_OUT` muss übereinstimmen.

### Arduino Mega 2560 — `readtext_sendhex` Ergänzungen

| Pin | Signal | Beschreibung |
|-----|--------|--------------|
| TX1 (18) | Gerät / Upstream 1 RX | Serial1 — HEX-Befehle / SET weiterleiten |
| TX2 (16) | Gerät / Upstream 2 RX | Serial2 — HEX-Befehle / SET weiterleiten |
| TX3 (14) | Gerät / Upstream 3 RX | Serial3 — HEX-Befehle / SET weiterleiten |
| RX0 (0) | Befehlseingang | Serial0 — SET/HEX vom Host |

### Teensy 4.1 — `readtext` Variante

| Pin | Signal | Beschreibung |
|-----|--------|--------------|
| RX1–RX7 | Gerät 1–7 TX | Serial1–7 — Text einlesen |
| TX8 / USB | Ausgang | Serial8 oder SerialUSB |
| GND | Masse | gemeinsame Masse |
| 3.3V / VIN | Versorgung | 3.3V-Pin oder VIN (5V-tolerant) |

⚠ Alle RX-Eingänge benötigen BSS138-Pegelwandler (5V → 3.3V).

### Teensy 4.1 — `readtext_sendhex` Ergänzungen

| Pin | Signal | Beschreibung |
|-----|--------|--------------|
| TX1–TX7 | Gerät 1–7 RX | Serial1–7 — HEX-Befehle |
| RX8 | Befehlseingang | Serial8 — SET/HEX vom Host |

⚠ TX-Ausgänge benötigen ebenfalls BSS138-Pegelwandler (3.3V → 5V).

### VE.Direct Stecker (JST-PH 2 mm, 4-polig)

| Pin | Signal | Verwendung |
|-----|--------|------------|
| 1 | GND | an MCU GND |
| 2 | TX (Ausgang) | an MCU RX (Teensy: über Pegelwandler) |
| 3 | RX (Eingang) | an MCU TX — nur `readtext_sendhex` |
| 4 | +5V | max. 10 mA — nicht zur MCU-Versorgung geeignet |

---

## Stromversorgung

| Quelle | Spannung | Max. Strom | Max. Leistung |
|--------|----------|------------|---------------|
| VE.Direct Pin 4 (pro Gerät) | 5V | 10 mA (Mittel), 20 mA/5ms | 50 mW |
| Arduino Mega 2560 (Verbrauch) | 5V | ~80–100 mA | ~400–500 mW |
| Teensy 4.1 (Verbrauch) | 3.3–5V | ~100 mA | ~300–500 mW |

**VE.Direct Pin 4 ist nicht zur MCU-Versorgung geeignet** — liefert nur 10 mA, der MCU benötigt ~100 mA.

MCU versorgen über:
- **DC/DC-Wandler** direkt an der Batterie (z.B. 8–60V Eingang, 5V Ausgang)
- **5V USB-Netzteil** am Installationsort
- **USB-Kabel** vom Host-System — wenn nah genug

---

## Galvanische Trennung

**Unkritisch** solange alle VE.Direct-Geräte dieselbe Batterie und gemeinsame Masse teilen — Geräte und MCU liegen auf demselben Potential.

**Relevant** wenn Geräte an verschiedenen PV-Strings mit getrennter Erdung betrieben werden, oder wenn MCU und Geräte aus getrennten Quellen versorgt werden. In diesen Fällen galvanische Trennung **downstream des MCU** realisieren — z.B. über einen isolierten seriellen Konverter am Ausgang.

---

## Puffergröße

Jeder Port puffert einen vollständigen VE.Direct-Block (512 Bytes). Blöcke werden sofort weitergeleitet sobald sie vollständig sind.

---

## Technische Daten

| Parameter | Mega 2560 | Teensy 4.1 |
|-----------|-----------|------------|
| Taktfrequenz | 16 MHz | 600 MHz |
| Logikpegel | 5V | 3.3V |
| Pegelwandler | Nein | Ja — BSS138 pro RX (+ TX bei sendhex) |
| Hardware-UARTs | 4 | 8 |
| Max. Geräteeingänge | 3 | 7 |
| Eingangs-Baudrate | 19200 Baud, 8N1 | 19200 Baud, 8N1 |
| Ausgangs-Baudrate | 19200 Baud (konfigurierbar) | 19200 Baud (konfigurierbar) |
| Ausgangsanschluss | TX0-Pin oder USB (gleicher Chip) | TX8-Pin oder SerialUSB (`OUTPUT_USB`) |
| Max. Geräte (direkt) | 3 | 7 |
| Max. Geräte (Stern) | 9 | 21 (3× Teensy → 1 zentraler) |
| Max. Geräte (Kaskade) | 9 / 12 bei 115200 Baud | 28 / mehr bei 115200 Baud |
| Typerkennung | keine — alle Ports identisch | keine — alle Ports identisch |
| HEX-Busy-Scope | pro Port — andere ungestört | pro Port — andere ungestört |
| SET-Kanal | ja (readtext_sendhex) | ja (readtext_sendhex) |
| VBAT_FALLBACK | konfigurierbar (Standard 24V) | konfigurierbar (Standard 24V) |
| Versorgung | extern 5V, min. 150 mA | extern 5V, min. 150 mA |
| CPU-Last (nur Text) | < 1 % bei 3 Geräten | < 0.1 % bei 7 Geräten |
| Text-Latenz | < 1 Block-Periode (~83 ms) | < 1 Block-Periode (~83 ms) |
| HEX-Befehl-Latenz | ~50–100 ms typisch, max. 1s | ~50–100 ms typisch, max. 1s |

---

## Einschränkungen

- Baudrate muss auf allen Eingangsstufen identisch sein (19200 Baud)
- Während eines SET- oder HEX-Befehls pausiert nur der betroffene Port — alle anderen laufen normal weiter
- `VBAT_FALLBACK` wird verwendet bis der erste Vbat-Wert empfangen wird — SET-Befehle in den ersten Sekunden nach dem Start können leicht ungenau sein
- VE.Direct Pin 4 liefert max. 10 mA — MCU muss extern versorgt werden
- Cerbo GX / Venus GX nicht als direkter Empfänger nutzbar
- Teensy 4.1: alle RX-Eingänge benötigen BSS138-Pegelwandler (5V → 3.3V); `readtext_sendhex` TX-Ausgänge ebenfalls (3.3V → 5V)
- PIDs werden bei jedem Block neu gelernt — Gerätewechsel werden innerhalb ~1s erkannt. Eine PID verfällt nach `PID_TIMEOUT` ms Inaktivität (Standard 10s) und wird dann upstream weitergeleitet oder gibt `ERR timeout` zurück
