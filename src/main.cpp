// =====================================================================
//  ATtiny412 (5 V, USB) - LED-Licht 3x0,5W ueber MOSFET
//  Tiefschlaf mit Tagesrhythmus, Start per Lichtmessung
//
//  Zyklus: 16 h LANGSCHLAF -> WARTEN (bis es dunkel ist) -> 5 h ANZEIGE
//          -> 16 h LANGSCHLAF -> ...
//
//  Der Start der Anzeige wird per Lichtmessung bestimmt, nicht per Uhr:
//  sobald es dunkel wird, geht das Licht an. Die Wartephase ist unbegrenzt,
//  es wird in festem Raster gemessen, bis die Schwelle unterschritten ist.
//
//  DerZyklus lässt sich auch mit dem Tater an PA6 durchlaufen. 
//  Wird dieser Taster im Anzeigemodus mehr als eine Sekunde gedrückt, 
//  dann lässt sich die Helligkeit einstellen. Nach 16 Sekunden Untätigkeit 
//  wird dieser Wert im Flash Speicher dauerhaft gespeichert.
//
//  Warum 16 h Schlaf? Das Aufwachen liegt 21 h nach dem letzten Sonnen-
//  untergang, also 3 h VOR dem naechsten: genug Reserve, damit die Drift
//  des internen Oszillators (+/-2,4 h/Tag) und der von November bis
//  Dezember taeglich fruehere Sonnenuntergang das Aufwachen nicht hinter
//  den Sonnenuntergang schieben. Mit 18 h waere die Reserve nur 1 h.
//
//  Stil: ueberwiegend Arduino-API (pinMode, digitalWrite, digitalRead,
//  analogRead, analogWrite, attachInterrupt). Direkte Registerzugriffe gibt
//  es nur dort, wo sie technisch noetig sind:
//    - RTC/PIT (Taktbasis, nicht aenderbar)
//    - BOD-Abschaltung im Schlaf (Strom)
//    - Eingangspuffer unbenutzter Pins (Strom)
//    - ADC ein und ausschalten (Strom)
//  Im Schlafzustand verbraucht die Schaltung weniger als 0,005mA
/*********************************************************************************

Bemerkung zu ATTINY412 in der Arduino Umgebung
In diesem Fall wird analogRead und Write verwendet - die Arduino Umgebung
hat einen Fehler beim ATTINY412 beim Pin Mapping. analogWrite auf PA3 und PA7
funktionieren deshalb nicht, PA6 wird zum wakeup interrupt gebraucht, PA1 ist
für analog read reserviert und PA0 ist ausschließlich Reset und Programming.
Deshalb ist einzig und allein PA2 für analogWrite nutzbar.
SpenceKonde November 2026 - es gibt einen Workaround ist hier aber nicht
nötig

**********************************************************************************/
//  Datenblatt: Microchip ATtiny212/214/412/414/416, DS40002287A
//    11.3.2  Sleep Modes (Tab. 11-1/11-2/11-3)          S.94/95
//    11.5.1  SLPCTRL.CTRLA: SMODE=0b10 -> PDOWN        S.98
//    17.3.3  BOD im Schlaf (FUSE.BODCFG.SLEEP)          S.159/161
//    23.5.2  PIT-Funktion, Prescaler wirkt auch auf PIT S.299
//    23.8    "The PIT will continue to operate in any sleep mode" S.301
//    23.12.7 RTC.CLKSEL: INT1K = 1,024 kHz aus OSCULP32K S.309
//    23.12.11 PIT-Periode: PERIOD in RTC-Clock-Zyklen    S.313
//    Tab.35-5  Power-Down-/Standby-Ströme                S.502
//    Tab.35-7  Peripherie-Ströme (BOD 19 uA, AC 45 uA)  S.503
//    Tab.35-13 Genauigkeit OSCULP32K (+/-3 ... +/-30 %)   S.506
//    Tab.5-1   Pinbelegung: TOSC1 = PB3, TOSC2 = PB2     S.18
// =====================================================================

#include <Arduino.h>
#include <Comparator.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <EEPROM.h>

// ---------------------------------------------------------------------
//  Zeitraster:  1 PIT-Tick = 16,000 s (16,384 RTC-Zyklen / 1,024 kHz)
//    Schlaf  16 h = 57 600 s = 3600 Ticks
//    Anzeige  5 h = 18 000 s = 1125 Ticks
//    Warten        = unbegrenzt, Messung alle 1 Tick
// ---------------------------------------------------------------------
constexpr uint16_t SEK_PRO_TICK = 16;
constexpr uint32_t SCHLAF_SEK = 16UL * 3600UL;                 // 16 Stunden
constexpr uint32_t ANZEIGE_SEK = 5UL * 3600UL;                 // 5 Stunden
constexpr uint32_t SCHLAF_TICKS = SCHLAF_SEK / SEK_PRO_TICK;   // 3600
constexpr uint32_t ANZEIGE_TICKS = ANZEIGE_SEK / SEK_PRO_TICK; // 1125

// Messintervall der Wartephase: 1 = bei jedem Tick messen.
constexpr uint32_t WIEDERHOLEN = 1;

// Heartbeat beim Warten: alle N Messungen blinken (1 = bei jeder Messung).
constexpr uint16_t BLINK_ALLE_TICKS = 4;

static_assert(SCHLAF_TICKS >= 1, "Schlaf muss >= 1 Tick sein");
static_assert(ANZEIGE_TICKS >= 1, "Anzeige muss >= 1 Tick sein");
static_assert(WIEDERHOLEN >= 1, "Wiederholen muss >= 1 sein");
static_assert(BLINK_ALLE_TICKS >= 1, "Blinkintervall muss >= 1 sein");

// ---------------------------------------------------------------------
//  Laufzeitvariablen
// ---------------------------------------------------------------------
volatile uint32_t pit_ticks = 0; // freilaufender Tick-Zaehler (ISR)
uint32_t pit_ticks_start = 0;    // Tick bei Beginn der aktuellen Phase
uint32_t warte_ticks = 0;        // Ticks seit Beginn der Wartephase

// ---------------------------------------------------------------------
//  Helligkeit
// ---------------------------------------------------------------------
const uint8_t pwmStufen[] = {5, 150, 170, 190}; //ausprobiert für 3V 0,5W
const uint8_t ANZAHL_STUFEN = sizeof(pwmStufen);
volatile uint8_t pwmIndex = 0;

// Bedienmodus Helligkeit
bool helligkeitEinstellen = false;
bool pwmGeaendert = false;

uint32_t letzteHelligkeitsAktion = 0;

constexpr uint32_t LANGDRUECKEN_MS = 2000;
constexpr uint32_t HELLIGKEIT_TIMEOUT_MS = 16000;

// ---------------------------------------------------------------------
//  PINS
// ---------------------------------------------------------------------
const uint8_t FET_PIN = PIN_PA2;        // MOSFET-Gate (PWM-Ausgang)
const uint8_t SENSOR_IN = PIN_PA1;      // ADC-Eingang Phototransistor
const uint8_t SENSOR_VCC = PIN_PA7;     // Spannungsteiler einschalten
const uint8_t TASTER_ZUSTAND = PIN_PA6; // Betriebszustand umschalten
// PIN_A3 ist unbenutzt

// Pins, die im Schlaf nie als Ausgang gefuehrt werden (hochohmig).
const uint8_t HOCHZ_PINS[] = {PIN_PA1, PIN_PA2, PIN_PA3, PIN_PA7};

// ---------------------------------------------------------------------
//  Zustandsautomat
// ---------------------------------------------------------------------
enum Betriebszustand
{
  LANGSCHLAF,
  WARTEN,
  MESSEN,
  ANZEIGE
};
volatile Betriebszustand Zustand;
volatile uint8_t taste6_ereignis = 0;

void wechselZustand(Betriebszustand neu);
static void schlafVorbereiten();
static void schlafen();
static bool lichtmessen();
static void blinken();
static void blinkxn(int x);
static void taste6Bearbeiten();
static void pwmSpeichernPruefen();

// ---------------------------------------------------------------------
//  Brown-out Detector im Schlafmodus abschalten
//  BOD.CTRLA Bit1:0 = SLEEP : 0 = Disabled
//  Im Power-Down kostet der BOD 1 uA (sampled) ... 19 uA (continuous).
//  Zusaetzlich sollte FUSE.BODCFG.SLEEP = 0 sein (Werkseinstellung 0x00).
// ---------------------------------------------------------------------
#ifndef CPU_CCP
#define CPU_CCP (*(volatile uint8_t *)0x0034) // ATtiny412: CPU.CCP
#endif

// Schwellwert analogRead - darunter gilt es als dunkel.
#define HMIN 720

static void bodImSchlafAusschalten()
{
  uint8_t v = BOD.CTRLA & ~BOD_SLEEP_gm; // SLEEP-Bits = 0 (Disabled)
  CPU_CCP = CCP_IOREG_gc;                // IOREG-Schutz aufheben
  BOD.CTRLA = v;                         // muss direkt danach erfolgen
}

// ---------------------------------------------------------------------
//  Taster-Interrupts (klassisch per attachInterrupt)
//  Der ATtiny412 hat nur EINEN Interrupt-Vektor je Port (PORTA_PORT_vect).
//  attachInterrupt() nimmt diesen Vektor intern ein und ruft die hier
//  registrierten Funktionen auf. Ob dieses Vorgehen immer mit dem mega
//  tiny core von spenceKonde sauber implementiert wird ist nicht sicher,
//  wird aber an mehreren Stellen beschrieben.
// ---------------------------------------------------------------------
static void tasterZustandISR()
{
  taste6_ereignis = 1;
}

// =====================================================================
//  Setup
// =====================================================================
void setup()
{
  // LED-/Sensor-Pins hochohmig: Pin als Eingang, Ausgangslatch auf 0
  for (uint8_t p : HOCHZ_PINS)
  {
    pinMode(p, INPUT);
    digitalWrite(p, LOW);
  }

  // Brown-out Detector im Schlaf abschalten (-> bis zu 19 uA gespart)
  bodImSchlafAusschalten();

  // Comparator bleibt dauerhaft aus (45-92 uA). Die Lichtmessung
  // erfolgt ueber den ADC an PA7/PA1.
  Comparator0.stop();

  // Taster: Pull-up, Interrupt bei fallender Flanke (Taster gegen GND)
  pinMode(TASTER_ZUSTAND, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(TASTER_ZUSTAND), tasterZustandISR, FALLING);

  // --- Stromoptimierung, ATtiny-spezifisch -----------------------------
  // ADC wird erst zur Lichtmessung benutzt -> nach Reset bereits aus,
  // besser explizit abschalten (sonst 325 uA).
  ADC0.CTRLA &= ~ADC_ENABLE_bm;

  // Eingangspuffer der unbenutzten Pins abschalten: verhindert undefinierte
  // Eingangspegel und reduziert den Ruhestrom.
  PORTA.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTA.PIN4CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTA.PIN5CTRL = PORT_ISC_INPUT_DISABLE_gc;
  // ---------------------------------------------------------------------

  // interner ULP-Oszillator OSCULP32K / 32 = 1,024 kHz
  RTC.CLKSEL = RTC_CLKSEL_INT1K_gc;
  RTC.PITINTCTRL = RTC_PI_bm;
  RTC.PITCTRLA = RTC_PERIOD_CYC16384_gc | RTC_PITEN_bm;

  // Schlafmodus: POWER-DOWN statt STANDBY
  // Power-Down: nur PIT (RTC), ULP-Oszillator, BOD, WDT aktiv (S.94),
  // Wake-up ueber PIT ist erlaubt (Tab. 11-3, S.95).
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();

  // gespeicherte Helligkeitsstufe laden
  uint8_t gespeichert = EEPROM.read(0);
  pwmIndex = (gespeichert < ANZAHL_STUFEN) ? gespeichert : 0;

  // starten: zuerst warten, bis es dunkel ist
  blinkxn(2);
  Zustand = Betriebszustand::WARTEN;
}

// =====================================================================
//  Hauptschleife / Zustandsautomat
// =====================================================================
void loop()
{
  taste6Bearbeiten();
  pwmSpeichernPruefen();
  switch (Zustand)
  {
  case LANGSCHLAF:
  {
    cli();

    if (pit_ticks - pit_ticks_start >= SCHLAF_TICKS)
    {
      warte_ticks = 0; // Wartephase beginnt
      wechselZustand(WARTEN);
    }
    else
    {
      schlafen(); // bis zum naechsten PIT-Tick
    }
    break;
  }

  case WARTEN:
  {
    cli();

    if (pit_ticks - pit_ticks_start >= WIEDERHOLEN)
    {
      wechselZustand(MESSEN);
    }
    else
    {
      schlafen(); // bis zum naechsten PIT-Tick
    }
    break;
  }

  case MESSEN:
  {
    ++warte_ticks;

    if (lichtmessen())
    {
      wechselZustand(ANZEIGE); // dunkel -> Licht an
    }
    else
    {
      // Heartbeat als Statusanzeige ("warte auf Dunkelheit").
      // Erst NACH der Messung blinken, damit die LEDs den Lichtsensor
      // nicht stoeren.
      if ((warte_ticks % BLINK_ALLE_TICKS) == 0)
      {
        blinken();
      }
      // Noch hell -> Timer neu starten, unbegrenzt weiter warten
      wechselZustand(WARTEN);
    }
    break;
  }

  case ANZEIGE:
  {
    pinMode(FET_PIN, OUTPUT);
    analogWrite(FET_PIN, pwmStufen[pwmIndex]);

    delay(500);

    cli();
    if ((pit_ticks - pit_ticks_start) >= ANZEIGE_TICKS)
    {
      helligkeitEinstellen = false;
      pinMode(FET_PIN, INPUT);
      wechselZustand(LANGSCHLAF);
    }
    sei();

    break;
  }
  }
}

// ---------------------------------------------------------------------
//  Alles, was im Schlaf keinen Strom ziehen darf
// ---------------------------------------------------------------------
static void schlafVorbereiten()
{
  for (uint8_t p : HOCHZ_PINS)
  {
    pinMode(p, INPUT);
    digitalWrite(p, LOW); // Latch auf 0 -> keine Pull-ups
  }

  Comparator0.stop(); // Comparator ist nach der Messung sowieso aus
}

// Einmal schlafen, bis der PIT oder ein Taster weckt.
static void schlafen()
{
  schlafVorbereiten();
  sleep_enable(); // SEN = 1
  sei();          // Interrupts an -> PIT/Taster koennen wecken
  sleep_cpu();    // <= 0,1 uA + OSCULP32K (0,5 uA)
  sleep_disable();
}

// ---------------------------------------------------------------------
//  Lichtmessung
//  Phototransistor an PA1, Spannungsteiler ueber PA7 (HIGH).
//  PA1 wird als ADC-Eingang gelesen. Ergebnis < HMIN => dunkel.
// ---------------------------------------------------------------------
static bool lichtmessen()
{
  // PA1 als Ausgang kurz auf LOW (Entladen/Reset des Teilers)
  pinMode(SENSOR_IN, OUTPUT);
  digitalWrite(SENSOR_IN, LOW);
  delayMicroseconds(20);

  // PA1 wieder hochohmig
  pinMode(SENSOR_IN, INPUT);

  // PA7 als Ausgang HIGH: Spannungsteiler einschalten
  pinMode(SENSOR_VCC, OUTPUT);
  digitalWrite(SENSOR_VCC, HIGH);

  analogReference(VDD);
  ADC0.CTRLA = ADC_ENABLE_bm;
  // Einschwingzeit
  delayMicroseconds(100);

  // Erste Messung verwerfen
  analogRead(SENSOR_IN);
  const uint16_t sensorValue = analogRead(SENSOR_IN);

  // ADC abschalten (Strom)
  ADC0.CTRLA = 0;

  const bool below_threshold = (sensorValue < HMIN);

  // PA7 wieder hochohmig
  pinMode(SENSOR_VCC, INPUT);
  digitalWrite(SENSOR_VCC, LOW);

  // PA1 wieder hochohmig
  pinMode(SENSOR_IN, INPUT);
  digitalWrite(SENSOR_IN, LOW);

  return (below_threshold);
}

// ---------------------------------------------------------------------
//  Zustandswechsel: Timer neu starten
// ---------------------------------------------------------------------
void wechselZustand(Betriebszustand neu)
{
  cli();
  pit_ticks_start = pit_ticks; // neuer Phasenbeginn
  Zustand = neu;
  sei();
}

// ---------------------------------------------------------------------
//  Heartbeat: kurzes Blinken der LEDs
// ---------------------------------------------------------------------
static void blinken()
{
  pinMode(FET_PIN, OUTPUT);
  digitalWrite(FET_PIN, HIGH);
  delay(1);
  digitalWrite(FET_PIN, LOW);
  delay(4);
  pinMode(FET_PIN, INPUT);
}

static void blinkxn(int x)
{
  for (int i = 0; i < x; i++)
  {
    blinken();
    delay(300);
  }
  delay(1000);
}

// Entprellter Taster PA6: Zustandswechsel des Automaten.
// ---------------------------------------------------------------------
//  PA6: Zustandssteuerung und Helligkeitseinstellung
// ---------------------------------------------------------------------
static void taste6Bearbeiten()
{
  if (!taste6_ereignis)
    return;

  cli();
  taste6_ereignis = 0;
  sei();

  // Kontaktprellen abwarten
  delay(30);

  if (digitalRead(TASTER_ZUSTAND) != LOW)
    return;

  // Beginn der Tastendruckmessung
  uint32_t drueckStart = millis();

  // Warten bis Taster losgelassen
  while (digitalRead(TASTER_ZUSTAND) == LOW)
    delay(1);

  uint32_t drueckDauer = millis() - drueckStart;

  // Kontaktprellen beim Loslassen abwarten
  delay(20);

  // ---------------------------------------------------------------
  // Helligkeitseinstellmodus
  // ---------------------------------------------------------------
  if (helligkeitEinstellen)
  {
    // Jeder Tastendruck schaltet eine Stufe weiter
    pwmIndex++;

    if (pwmIndex >= ANZAHL_STUFEN)
      pwmIndex = 0;

    pwmGeaendert = true;
    letzteHelligkeitsAktion = millis();

    return;
  }

  // ---------------------------------------------------------------
  // Langer Tastendruck im Anzeigemodus:
  // Helligkeitseinstellung aktivieren
  // ---------------------------------------------------------------
  if (Zustand == ANZEIGE &&
      drueckDauer >= LANGDRUECKEN_MS)
  {
    helligkeitEinstellen = true;
    pwmGeaendert = false;
    letzteHelligkeitsAktion = millis();
    blinkxn(11);
    return;
  }

  // ---------------------------------------------------------------
  // Bisherige Funktion des Zustandstasters bei kurzem Tastendruck
  // ---------------------------------------------------------------
  switch (Zustand)
  {
  case ANZEIGE:
  {
    wechselZustand(LANGSCHLAF);
    blinkxn(7);
    break;
  }

  case LANGSCHLAF:
  {
    wechselZustand(MESSEN);
    blinkxn(4);
    break;
  }

  case WARTEN:
  case MESSEN:
  {
    wechselZustand(ANZEIGE);
    break;
  }

  default:
    break;
  }
}

// ---------------------------------------------------------------------
//  Helligkeit speichern und Einstellmodus verlassen
// ---------------------------------------------------------------------
static void pwmSpeichernPruefen()
{
  if (!helligkeitEinstellen)
    return;

  if ((uint32_t)(millis() - letzteHelligkeitsAktion) >= HELLIGKEIT_TIMEOUT_MS)
  {
    // Nur schreiben, wenn sich die Helligkeit geändert hat
    if (pwmGeaendert)
    {
      EEPROM.update(0, pwmIndex);
      pwmGeaendert = false;
    }

    // Zurück in den normalen Anzeigemodus
    helligkeitEinstellen = false;
    blinkxn(3);
  }
}

// ---------------------------------------------------------------------
//  PIT-Interrupt: 1 Tick pro 16 s
//  Der Takt kommt direkt aus dem RTC und bleibt unveraendert.
// ---------------------------------------------------------------------
ISR(RTC_PIT_vect)
{
  RTC.PITINTFLAGS = RTC_PI_bm;
  ++pit_ticks;
}