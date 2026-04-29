/******************************************************************************
 * HEYTAPS – TRANSMITTER
 * Board  : ESP32-C3 Super Mini
 * Sensors: HC-SR04 Ultrasonic + DHT11 Temperature/Humidity
 * Radio  : NRF24L01+ (2.4 GHz)
 *
 * WIRING TABLE
 * ┌──────────────┬────────────┬──────────────────────────────────────────┐
 * │  Component   │  ESP32-C3  │  Notes                                   │
 * ├──────────────┼────────────┼──────────────────────────────────────────┤
 * │ NRF24  VCC   │ 3V3        │ 3.3V ONLY. Add 100uF cap on VCC/GND      │
 * │ NRF24  GND   │ GND        │                                          │
 * │ NRF24  CE    │ GPIO3      │                                          │
 * │ NRF24  CSN   │ GPIO10     │                                          │
 * │ NRF24  SCK   │ GPIO4      │ SPI clock                                │
 * │ NRF24  MISO  │ GPIO5      │ SPI data in                              │
 * │ NRF24  MOSI  │ GPIO6      │ SPI data out                             │
 * ├──────────────┼────────────┼──────────────────────────────────────────┤
 * │ HC-SR04 VCC  │ 5V (VBUS)  │ Needs 5V                                 │
 * │ HC-SR04 GND  │ GND        │                                          │
 * │ HC-SR04 TRIG │ GPIO1      │ 10us trigger pulse                       │
 * │ HC-SR04 ECHO │ GPIO2      │ Use voltage divider for 3.3V safety      │
 * ├──────────────┼────────────┼──────────────────────────────────────────┤
 * │ DHT11   VCC  │ 3V3        │                                          │
 * │ DHT11   GND  │ GND        │                                          │
 * │ DHT11   DATA │ GPIO0      │ 10k pull-up resistor to VCC              │
 * └──────────────┴────────────┴──────────────────────────────────────────┘
 *
 * Open Serial Monitor at 115200 baud for full diagnostics.
 ******************************************************************************/

#include <SPI.h>
#include <RF24.h>
#include <DHT.h>

// =============================================================================
//  PIN DEFINITIONS
// =============================================================================
#define CE_PIN    3
#define CSN_PIN   10
#define SCK_PIN   4
#define MISO_PIN  5
#define MOSI_PIN  6

#define TRIG_PIN  1
#define ECHO_PIN  2

#define DHT_PIN   0
#define DHT_TYPE  DHT11

// =============================================================================
//  RF24 SETTINGS  –  must match receiver exactly
// =============================================================================
const byte    PIPE_ADDR[6] = "HTAP1";
const uint8_t RF_CHANNEL   = 108;    // 2508 MHz, above Wi-Fi band

// =============================================================================
//  SHARED DATA PACKET  –  identical struct on both ends
// =============================================================================
struct SensorData {
  float    temperature;   // degC  (4 bytes)
  float    humidity;      // %RH   (4 bytes)
  uint8_t  waterLevel;    // 0-100 (1 byte)  – computed on receiver side
  uint16_t distance_mm;   // mm    (2 bytes)
};                        // Total = 11 bytes
SensorData txData;

// =============================================================================
//  OBJECTS
// =============================================================================
RF24 radio(CE_PIN, CSN_PIN);
DHT  dht(DHT_PIN, DHT_TYPE);

// =============================================================================
//  COUNTERS
// =============================================================================
unsigned long txCount   = 0;
unsigned long okCount   = 0;
unsigned long failCount = 0;

// =============================================================================
//  HC-SR04 – returns distance in mm, 9999 on timeout/error
// =============================================================================
uint16_t readUltrasonic_mm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 38000UL); // 38ms timeout ~ 6.5m max
  if (duration == 0) {
    Serial.println("  [SONAR] No echo pulse! Check HC-SR04 VCC=5V, TRIG=GPIO1, ECHO=GPIO2");
    return 9999;
  }
  return (uint16_t)(duration * 0.1715f); // mm = us * 0.1715
}

// =============================================================================
//  HELPER
// =============================================================================
void printLine(char c = '-', uint8_t n = 55) {
  for (uint8_t i = 0; i < n; i++) Serial.print(c);
  Serial.println();
}

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(1500); // wait for Serial Monitor to connect

  printLine('=');
  Serial.println("  HeyTaps TRANSMITTER  |  ESP32-C3 Super Mini");
  Serial.println("  Build: " __DATE__ "  " __TIME__);
  printLine('=');

  // --- HC-SR04 ---------------------------------------------------------------
  Serial.println("[INIT 1/3] HC-SR04 Ultrasonic Sensor");
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  delay(100);
  uint16_t d = readUltrasonic_mm();
  if (d == 9999)
    Serial.println("  STATUS: FAIL – no echo. Check wiring.");
  else {
    Serial.print("  STATUS: OK – reading = "); Serial.print(d); Serial.println(" mm");
  }

  // --- DHT11 -----------------------------------------------------------------
  Serial.println("[INIT 2/3] DHT11 Temperature/Humidity Sensor");
  dht.begin();
  delay(2000); // DHT11 needs 2s warmup
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h))
    Serial.println("  STATUS: FAIL – check DATA=GPIO0 and 10k pull-up resistor.");
  else {
    Serial.print("  STATUS: OK – Temp="); Serial.print(t,1);
    Serial.print("C  Hum="); Serial.print(h,1); Serial.println("%");
  }

  // --- NRF24L01+ -------------------------------------------------------------
  Serial.println("[INIT 3/3] NRF24L01+ Radio Module");
  Serial.println("  Pins: CE=GPIO3  CSN=GPIO10  SCK=GPIO4  MISO=GPIO5  MOSI=GPIO6");

  // Important: begin SPI without SS argument; RF24 drives CSN itself
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
  delay(20);

  if (!radio.begin()) {
    Serial.println("  STATUS: FAIL – NRF24L01 not responding!");
    Serial.println("  Checklist:");
    Serial.println("    1. VCC = 3.3V ONLY (NOT 5V)");
    Serial.println("    2. 100uF capacitor across VCC & GND of the module");
    Serial.println("    3. CE=GPIO3  CSN=GPIO10  SCK=GPIO4  MISO=GPIO5  MOSI=GPIO6");
    Serial.println("    4. All GNDs connected together");
    Serial.println("  Halted. Fix wiring then press RESET.");
    while (true) delay(1000);
  }

  radio.setChannel(RF_CHANNEL);
  radio.setDataRate(RF24_250KBPS);  // best range & sensitivity
  radio.setPALevel(RF24_PA_HIGH);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(true);
  radio.setRetries(5, 15);          // up to 15 retries, 1.25ms spacing
  radio.openWritingPipe(PIPE_ADDR);
  radio.stopListening();            // TX mode

  Serial.println("  STATUS: OK");
  Serial.print("  Channel : "); Serial.print(RF_CHANNEL);
  Serial.print("  ("); Serial.print(2400 + RF_CHANNEL); Serial.println(" MHz)");
  Serial.print("  Address : "); Serial.println((char*)PIPE_ADDR);
  Serial.print("  Pkt size: "); Serial.print(sizeof(txData)); Serial.println(" bytes");
  radio.printDetails(); // full register dump for debugging

  printLine('=');
  Serial.println("  All sensors OK. Starting transmit loop...");
  Serial.println("  Format: [TX #N]  Dist  Water  Temp  Hum  RF-Status");
  printLine('=');
  Serial.println();
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
  txCount++;

  // 1. Read HC-SR04
  uint16_t dist_mm = readUltrasonic_mm();
  bool sonarOk = (dist_mm != 9999);

  // 2. Read DHT11
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  bool dhtOk = !(isnan(temp) || isnan(hum));
  if (!dhtOk) { temp = 0.0f; hum = 0.0f; }

  // 3. Build packet
  // NOTE: waterLevel is calculated on the RECEIVER using tank calibration
  //       set via the web dashboard. We send 0 here; receiver ignores it.
  txData.temperature = temp;
  txData.humidity    = hum;
  txData.waterLevel  = 0;                          // receiver calculates this
  txData.distance_mm = sonarOk ? dist_mm : 0;

  // 4. Transmit
  bool rfOk = radio.write(&txData, sizeof(txData));
  if (rfOk) okCount++; else failCount++;

  // 5. Serial output
  Serial.print("[TX #"); Serial.print(txCount); Serial.print("]  ");

  Serial.print("Dist=");
  if (sonarOk) { Serial.print(dist_mm); Serial.print("mm"); }
  else Serial.print("ERR");

  Serial.print("  Temp=");
  if (dhtOk) { Serial.print(temp, 1); Serial.print("C"); }
  else Serial.print("ERR");

  Serial.print("  Hum=");
  if (dhtOk) { Serial.print(hum, 1); Serial.print("%"); }
  else Serial.print("ERR");

  Serial.print("  RF="); Serial.print(rfOk ? "OK" : "FAIL");
  Serial.print("  (OK:"); Serial.print(okCount);
  Serial.print("/"); Serial.print(txCount); Serial.println(")");

  // Print troubleshooting hint after 5 consecutive failures
  if (!rfOk && (failCount % 5 == 0)) {
    printLine();
    Serial.println("  [RF FAIL x5] Receiver troubleshooting:");
    Serial.println("    - Is receiver powered on and running?");
    Serial.println("    - Both must use: PIPE_ADDR=\"HTAP1\", CH=108, 250KBPS");
    Serial.println("    - Add 100uF cap on NRF24 VCC/GND on BOTH modules");
    printLine();
  }

  delay(2000); // transmit every 2 seconds
}