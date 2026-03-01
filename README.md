# ESPHome – Landis+Gyr T330 Wärmezähler

ESPHome External Component für den **Landis+Gyr T330 / Ultraheat** Wärmezähler. Liest alle Messwerte über die IR-Schnittstelle aus und überträgt sie an Home Assistant.

---

## Ergebnis

![Home Assistant Dashboard mit allen 10 Sensoren](docs/ha_dashboard.png)

| Sensor | Einheit |
|---|---|
| Wärmeenergie | kWh |
| Wasservolumen | m³ |
| Heizleistung | kW |
| Volumenstrom | m³/h |
| Vorlauftemperatur | °C |
| Rücklauftemperatur | °C |
| Temperaturspreizung | K |
| Betriebszeit | h |
| Aktivitätsdauer | s |
| Fabriknummer | – |

---

## Hardware

### Komponenten

| Teil | Beschreibung |
|---|---|
| **LilyGo T-Internet-POE V2** | ESP32-Board mit LAN8720A Ethernet-PHY und PoE-Versorgung |
| **IR-Optikkopf** | Bidirektionaler IR-Schreib-/Lesekopf für M-Bus-Zähler (z.B. Volkszähler, All-in-one) |
| **Landis+Gyr T330 / Ultraheat** | M-Bus Wärmezähler |

### Verkabelung IR-Kopf → T-Internet-POE V2

```
IR-Kopf TX (Daten vom Zähler)  →  GPIO36   (UART2 RX, Input-only)
IR-Kopf RX (Daten zum Zähler)  →  GPIO4    (UART2 TX)
IR-Kopf VCC                    →  3V3
IR-Kopf GND                    →  GND
```

> **Wichtig:** Die folgenden GPIOs sind vom LAN8720A-Ethernet-PHY belegt und dürfen **nicht** verwendet werden:
> GPIO 17 (REF_CLK), 18 (MDIO), 19 (TXD0), 21 (TX_EN), 22 (TXD1), 23 (MDC), 25 (RXD0), 26 (RXD1), 27 (CRS_DV)

---

## Funktionsweise

### M-Bus Kommunikationsprotokoll

Der T330 kommuniziert über das M-Bus-Protokoll in einem 4-Sequenz-Handshake:

```
Seq1  (2400 Baud)  Wake-up + Versionsanfrage
Seq2  (2400 Baud)  Application Reset  →  ACK 0xE5
Seq3  (2400 Baud)  Slave-Auswahl      →  11-Byte-Bestätigung
Seq4               Daten anfordern, zweiphasig lesen (siehe unten)
```

### Zweiphasige Datenerfassung (Kernstück)

Der T330 hat ein ungewöhnliches Sendeverhalten nach dem Daten-Request:

- **Phase 1 – 2400 Baud (800 ms):** Der Zähler sendet zunächst Frame 0 mit den aktuellen Messwerten (`stor=0`) noch bei 2400 Baud.
- **Phase 2 – 9600 Baud:** Der Zähler wechselt selbst auf 9600 Baud und sendet 27 weitere Frames mit Archivdaten (`stor=1..25`).

**Warum ist Phase 1 notwendig?**
Ein Linux-UART-Treiber puffert eingehende Bytes unabhängig vom eingestellten Baud – daher funktionieren auf Linux basierende Lösungen (z.B. Perl-Skripte mit 1,5 s Verzögerung) ohne diese Unterscheidung. Die **ESP32-UART-Hardware hingegen verwirft Bytes mit Framing-Fehlern sofort**. Wechselt man auf der ESP32-Seite zu früh auf 9600 Baud, gehen die 2400-Baud-Daten aus Phase 1 verloren; bleibt man zu lange bei 2400 Baud, verpasst man Phase 2. Die Lösung ist, Phase 1 explizit bei 2400 Baud zu lesen und erst danach auf 9600 zu wechseln.

### Parser

Der Parser verarbeitet alle empfangenen M-Bus-Frames (DIF/VIF-Dekodierung nach EN 13757-3) und filtert ausschließlich Datensätze mit `stor=0 / tar=0 / su=0` – das entspricht den Aktualdaten (identisch zur Filterung im Perl-Referenzskript über MQTT-Topic `/storage/0/0/0/`).

### FreeRTOS-Architektur

Alle blockierenden UART-Operationen laufen in einem dedizierten **FreeRTOS-Task auf Core 0**. Der ESPHome-Hauptloop auf Core 1 wird nie blockiert, womit Task-Watchdog-Abstürze ausgeschlossen sind. Die Datenübergabe zwischen Task und Hauptloop erfolgt über Mutex und ein `volatile`-Flag.

---

## Verzeichnisstruktur

```
/config/esphome/
├── t330.yaml
├── secrets.yaml
└── components/
    └── t330_meter/
        ├── __init__.py
        └── t330_meter.h
```

---

## Installation

### 1. Dateien kopieren

Die Dateien aus diesem Repository in das ESPHome-Konfigurationsverzeichnis kopieren (üblicherweise `/config/esphome/` in Home Assistant).

### 2. `secrets.yaml` anpassen

```yaml
api_encryption_key: "DEIN_32_BYTE_BASE64_KEY"
ota_password: "DEIN_OTA_PASSWORT"
```

### 3. `t330.yaml` anpassen

Statische IP, Gateway und Subnetz auf das eigene Netzwerk einstellen:

```yaml
ethernet:
  manual_ip:
    static_ip: 192.168.2.246   # ← anpassen
    gateway:   192.168.2.1       # ← anpassen
    subnet:    255.255.255.0     # ← anpassen
```

GPIO-Pins falls abweichend von der Standardverkabelung:

```yaml
t330_meter:
  tx_pin: 4    # ← UART2 TX → IR-Kopf RX
  rx_pin: 36   # ← UART2 RX ← IR-Kopf TX
```

### 4. Flashen

```bash
esphome run t330.yaml
```

Beim ersten Flash muss das Board per USB angeschlossen sein. Alle weiteren Updates laufen per OTA über Ethernet.

---

## Konfiguration (`t330.yaml`)

```yaml
t330_meter:
  id: t330
  tx_pin: 4
  rx_pin: 36

  energy_kwh:
    name: "Wärmeenergie"
  volume_qm:
    name: "Wasservolumen"
  power_kw:
    name: "Heizleistung"
  volume_flow:
    name: "Volumenstrom"
  flow_temp:
    name: "Vorlauftemperatur"
  return_temp:
    name: "Rücklauftemperatur"
  temp_diff:
    name: "Temperaturspreizung"
  operating_time:
    name: "Betriebszeit"
  activity_duration:
    name: "Aktivitätsdauer"
  fabrication_number:
    name: "Fabriknummer"
```

Alle Sensoren sind optional. Nicht benötigte Felder können einfach weggelassen werden.

---

## Home Assistant

Nach dem ersten Start erscheinen alle konfigurierten Sensoren automatisch unter dem Gerät **„Wärmezähler T330"**. Zusätzlich stehen bereit:

- **Button „Wärmezähler ablesen"** – löst eine sofortige Ablesung aus (unabhängig vom konfigurierten Intervall)
- **Button „Gerät neu starten"** – Reboot per HA
- **Sensor „Uptime"** – Laufzeit des ESP32
- **Sensor „IP Adresse"** – aktuelle Ethernet-IP

### Empfohlenes Abfrageintervall

Der T330 überträgt bei jeder Abfrage ca. 4,5 KB Daten (27 Frames). Die Kommunikation dauert etwa 30 Sekunden. Ein Intervall von **5–60 Minuten** ist sinnvoll.

```yaml
# In t330.yaml – Abfrageintervall einstellen:
# (Die Komponente erbt von PollingComponent, update_interval ist in __init__.py definiert)
t330_meter:
  update_interval: 15min
```

---

## Typischer Log-Verlauf

```
[I][t330]: Triggering read cycle
[I][t330_task]: Task: starting meter read
[I][t330_task]: ── Starte M-Bus Kommunikation (4 Sequenzen) ──
[I][t330_task]: Seq1 OK - Versionsstring empfangen (22 Bytes)
[I][t330_task]: Seq2 OK - ACK (0xE5) empfangen
[I][t330_task]: Seq3 OK - Bestaetigung (11 Bytes)
[I][t330_task]: Seq4 - Phase 1: 211 Bytes @ 2400 Baud
[I][t330_task]: Seq4 - Phase 2: 4332 Bytes @ 9600 Baud
[I][t330_task]: Seq4 OK - 4543 Bytes gesamt empfangen
[I][t330_task]: Alle Sequenzen erfolgreich - starte Parser
[I][t330_task]:   energy_kwh            = 15136.000 kWh
[I][t330_task]:   volume_qm             = 204.0300 m3
[I][t330_task]:   power_kw              = 700.000 kW
[I][t330_task]:   volume_flow_qm_h      = 0.0110 m3/h
[I][t330_task]:   flow_temp_c           = 79.90 C
[I][t330_task]:   return_temp_c         = 23.70 C
[I][t330_task]:   temp_diff_k           = 56.20 K
[I][t330_task]:   operating_time_hours  = 5398 h
[I][t330_task]:   activity_duration_sec = 4 s
[I][t330_task]:   fabrication_number    = 72180089
[I][t330_task]: Task: read successful
[I][t330]:      Waermeenergie : 15136.0000 kWh
...
[I][t330]: Data published to Home Assistant
```

---

## Troubleshooting

**Seq1: keine Antwort**
IR-Kopf nicht korrekt auf der Zähler-IR-Schnittstelle positioniert. Der Kopf muss magnetisch am Zähler haften und zentriert auf dem IR-Auge sitzen.

**Seq1: Echo empfangen**
TX und RX vertauscht – GPIO4 und GPIO36 in der Konfiguration tauschen.

**Seq4: keine Nutzdaten**
IR-Kopf während der Übertragung verrutscht. Der Zähler sendet nach dem Befehl sofort ~200 Bytes bei 2400 Baud, danach ~4300 Bytes bei 9600 Baud – die gesamte Übertragung dauert ca. 15 Sekunden und darf nicht unterbrochen werden.

**Parser: Keine Messwerte gefunden**
Prüfen ob der empfangene Datenstrom (Byte-Anzahl in Phase 1 + Phase 2) plausibel ist. Ein vollständiger Datenstrom des T330 umfasst ca. 4500 Bytes.

---

## Referenzen

- [karwho/landisgyr_t330](https://codeberg.org/karwho/landisgyr_t330) – Perl-Referenzimplementierung für Raspberry Pi (MQTT)
- [EN 13757-3](https://www.bsigroup.com/) – M-Bus Kommunikationsprotokoll für Zähler
- [ESPHome External Components](https://esphome.io/components/external_components.html)
- [LilyGo T-Internet-POE](https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-POE)

---

## Lizenz

MIT
