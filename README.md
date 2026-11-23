## GNU GPL V3

Copyright (c) 2026 Andreas Mascheck

# Weihnachtsstern – intelligente Lichtsteuerung mit ATtiny412

Eine stromsparende Lichtsteuerung für einen beleuchteten Weihnachtsstern. Die Steuerung verwendet einen **ATtiny412**, einen Lichtsensor und eine PWM-gesteuerte MOSFET-Endstufe.

Das Besondere: Der Stern orientiert sich am natürlichen Tagesrhythmus. Er schaltet sich bei Einbruch der Dunkelheit automatisch ein und bleibt für eine definierte Zeit eingeschaltet. Anschließend geht der Controller in einen extrem stromsparenden Tiefschlaf.

## Funktionen

- Automatisches Einschalten bei Einbruch der Dunkelheit
- Lichtmessung über einen analogen Sensor
- Einstellbare LED-Helligkeit mit vier PWM-Stufen
- Speicherung der gewählten Helligkeit im EEPROM
- Manuelle Steuerung über einen einzigen Taster
- Automatischer Tagesrhythmus ohne Echtzeituhr
- Extrem niedriger Stromverbrauch im Tiefschlaf

## Betriebsablauf

Der Controller arbeitet mit einem Zustandsautomaten:

1. **LANGSCHLAF:** 16 Stunden Tiefschlaf.
2. **WARTEN:** Regelmäßige Lichtmessung, bis die Umgebungshelligkeit unter den eingestellten Schwellwert fällt.
3. **ANZEIGE:** Beleuchtung für 5 Stunden.
4. Anschließend beginnt der Zyklus erneut mit 16 Stunden Tiefschlaf.

Die Zeitbasis liefert der interne Ultra-Low-Power-Oszillator des ATtiny412 über den RTC Periodic Interrupt Timer (PIT).

Die Beleuchtung wird somit nicht über eine feste Uhrzeit, sondern über die tatsächlichen Lichtverhältnisse gesteuert.

## Hardware

| Bauteil | Funktion | ATtiny412 |
|---|---|---|
| ATtiny412 | Mikrocontroller | – |
| MOSFET | LED-Leistungssteuerung | PA2 |
| Lichtsensor | Helligkeitsmessung | PA1 |
| Sensorversorgung | Spannungsteiler schalten | PA7 |
| Taster | Bedienung und Zustandswechsel | PA6 |

Die LED-Beleuchtung wird über einen MOSFET mit PWM angesteuert.

Die Sensorversorgung wird nur während der Messung eingeschaltet. Dadurch wird unnötiger Stromverbrauch vermieden.

## Bedienung

Der gesamte Betrieb wird mit einem einzigen Taster gesteuert.

**Kurzer Tastendruck:**

- Während der Anzeige: Beleuchtung ausschalten und in den Langschlaf wechseln.
- Im Langschlaf: Messphase starten.
- Während der Wartephase: Beleuchtung manuell einschalten.

**Langer Tastendruck (2 Sekunden) während der Anzeige:**

Aktiviert die Helligkeitseinstellung.

Jeder weitere Tastendruck schaltet zur nächsten der vier Helligkeitsstufen.

Wird der Taster 16 Sekunden lang nicht betätigt, wird die eingestellte Helligkeit im EEPROM gespeichert und der normale Betrieb fortgesetzt.

## Energiesparen

Ein Schwerpunkt des Projekts ist die Minimierung des Ruhestroms.

Dazu werden folgende Maßnahmen verwendet:

- POWER-DOWN-Schlafmodus des ATtiny412
- Abschaltung des Brown-out-Detectors während des Schlafs
- Abschaltung des ADC außerhalb der Messung
- Abschaltung des Analogkomparators
- Deaktivierung nicht benötigter digitaler Eingangspuffer
- Hochohmige Schaltung der nicht benötigten Pins im Schlafzustand
- Abschaltung der Sensorversorgung außerhalb der Messung

Der angestrebte Ruhestrom der Schaltung liegt unter 5 µA.

## Software

Die Software wurde mit **PlatformIO** und dem Arduino-Framework auf Basis von **megaTinyCore (Spence Konde)** entwickelt.

### Entwicklungsumgebung

- Mikrocontroller: ATtiny412
- Framework: Arduino
- Core: megaTinyCore
- CPU-Takt: 1 MHz
- Programmierschnittstelle: UPDI
- Upload-Protokoll: jtag2updi

### Projekt kompilieren und flashen

Repository klonen:

```bash
git clone https://github.com/mascheck55/eintasterStern.git
```

Projekt in PlatformIO öffnen und die Umgebung `Upload_UPDI` auswählen.

Die UPDI-Schnittstelle muss unter dem in `platformio.ini` eingetragenen Port erreichbar sein.

## Projektstruktur

| Verzeichnis / Datei | Inhalt |
|---|---|
| `src/` | Hauptprogramm |
| `include/` | Header-Dateien |
| `lib/` | Projektbibliotheken |
| `documents/` | Dokumentation |
| `kicad/` | Schaltungsunterlagen |
| `3Dprint/` | 3D-Druckdateien |
| `test/` | Testprogramme |
| `platformio.ini` | PlatformIO-Konfiguration |

## Hinweise

Die Schaltung ist für den Betrieb mit einer LED-Beleuchtung über MOSFET ausgelegt.

Die verwendeten PWM-Stufen und der Lichtsensor-Schwellwert können im Quellcode an die jeweilige Hardware angepasst werden.

**Hinweis:** Die Zeitsteuerung verwendet den internen Oszillator des ATtiny412. Die tatsächlichen Zeitintervalle können daher abhängig von Bauteiltoleranzen und Temperatur von den programmierten Werten abweichen.

## Lizenz

## GNU GPL V3

Copyright (c) 2026 Andreas Mascheck

