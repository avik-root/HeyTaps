/******************************************************************************
 * HEYTAPS – TRANSMITTER
 * Board  : ESP32-C3 Super Mini
 * Sensors: HC-SR04 Ultrasonic + DHT11 Temperature/Humidity
 * Radio  : NRF24L01+ (2.4 GHz)
 *
 * SECURITY: Each unit has a unique 12-char Device ID derived from chip MAC.
 *   The ID is printed on Serial at boot. The receiver user must enter this
 *   code on the web portal to pair and authenticate before seeing data.
 *
 * WIRING TABLE
 * ┌──────────────┬────────────┬──────────────────────────────────────────┐
 * │  Component   │  ESP32-C3  │  Notes                                   │
 * ├──────────────┼────────────┼──────────────────────────────────────────┤
 * │ NRF24  VCC   │ 3V3        │ 3.3V ONLY. Add 100uF cap on VCC/GND      │
 * │ NRF24  GND   │ GND        │                                          │
 * │ NRF24  CE    │ GPIO3      │                                          │
 * │ NRF24  CSN   │ GPIO10     │                                          │
 * │ NRF24  SCK   │ GPIO4      │                                          │
 * │ NRF24  MISO  │ GPIO5      │                                          │
 * │ NRF24  MOSI  │ GPIO6      │                                          │
 * ├──────────────┼────────────┼──────────────────────────────────────────┤
 * │ HC-SR04 VCC  │ 5V (VBUS)  │ Needs 5V                                 │
 * │ HC-SR04 GND  │ GND        │                                          │
 * │ HC-SR04 TRIG │ GPIO1      │                                          │
 * │ HC-SR04 ECHO │ GPIO2      │ Use voltage divider for 3.3V safety      │
 * ├──────────────┼────────────┼──────────────────────────────────────────┤
 * │ DHT11   VCC  │ 3V3        │                                          │
 * │ DHT11   GND  │ GND        │                                          │
 * │ DHT11   DATA │ GPIO0      │ 10k pull-up to VCC                       │
 * └──────────────┴────────────┴──────────────────────────────────────────┘
 ******************************************************************************/

#include <SPI.h>
#include <RF24.h>
#include <DHT.h>
// NOTE: Preferences library no longer needed – Device ID is hardcoded below

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
const uint8_t RF_CHANNEL   = 108;

// =============================================================================
//  DEVICE ID  –  Exactly 12 characters. Change this to match your unit.
//  The receiver user must enter this code on the web portal to pair.
//  Use only: A-Z 0-9 (uppercase). Count carefully – must be exactly 12.
// =============================================================================
#define DEVICE_ID   "HEYTAPS00001"   // <–– CHANGE THIS (12 chars exactly)

// =============================================================================
//  SHARED DATA PACKET  –  IDENTICAL struct on both ends
//  Size: 4 + 4 + 1 + 2 + 12 = 23 bytes (within RF24's 32-byte max)
// =============================================================================
struct SensorData {
  float    temperature;   // degC    (4 bytes)
  float    humidity;      // %RH     (4 bytes)
  uint8_t  waterLevel;    // unused  (1 byte) – receiver recalculates
  uint16_t distance_mm;   // mm      (2 bytes)
  char     devId[12];     // Device ID, no null terminator in packet (12 bytes)
};
SensorData txData;
//  OBJECTS
// =============================================================================
RF24        radio(CE_PIN, CSN_PIN);
DHT         dht(DHT_PIN, DHT_TYPE);

// =============================================================================
//  COUNTERS
// =============================================================================
unsigned long txCount   = 0;
unsigned long okCount   = 0;
unsigned long failCount = 0;

// =============================================================================
//  HC-SR04  –  returns mm, 9999 on timeout
// =============================================================================
uint16_t readUltrasonic_mm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 38000UL);
  if (dur == 0) {
    Serial.println("  [SONAR] No echo – check HC-SR04 VCC=5V, TRIG=GPIO1, ECHO=GPIO2");
    return 9999;
  }
  return (uint16_t)(dur * 0.1715f);
}

// (No initDeviceId function needed – ID is hardcoded as DEVICE_ID)

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
  delay(1500);

  printLine('=');
  Serial.println("  HeyTaps TRANSMITTER  |  ESP32-C3 Super Mini");
  Serial.println("  Build: " __DATE__ "  " __TIME__);
  printLine('=');

  // --- Device ID (hardcoded) -------------------------------------------------
  static_assert(sizeof(DEVICE_ID)-1 == 12, "DEVICE_ID must be exactly 12 characters!");
  printLine('*');
  Serial.print("  DEVICE ID : "); Serial.println(DEVICE_ID);
  Serial.println("  Enter this on the receiver web portal to pair.");
  printLine('*');

  // --- HC-SR04 ---------------------------------------------------------------
  Serial.println("[INIT 1/3] HC-SR04");
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  delay(100);
  uint16_t d = readUltrasonic_mm();
  if (d == 9999) Serial.println("  STATUS: FAIL – no echo.");
  else { Serial.print("  STATUS: OK – "); Serial.print(d); Serial.println(" mm"); }

  // --- DHT11 -----------------------------------------------------------------
  Serial.println("[INIT 2/3] DHT11");
  dht.begin(); delay(2000);
  float t = dht.readTemperature(), h = dht.readHumidity();
  if (isnan(t) || isnan(h)) Serial.println("  STATUS: FAIL – check DATA=GPIO0 + 10k pull-up.");
  else { Serial.print("  STATUS: OK – T="); Serial.print(t,1); Serial.print("C H="); Serial.print(h,1); Serial.println("%"); }

  // --- NRF24L01+ -------------------------------------------------------------
  Serial.println("[INIT 3/3] NRF24L01+");
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
  delay(20);
  if (!radio.begin()) {
    Serial.println("  STATUS: FAIL – not responding! Check wiring/3.3V/100uF cap.");
    while (true) delay(1000);
  }
  radio.setChannel(RF_CHANNEL);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_HIGH);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(true);
  radio.setRetries(5, 15);
  radio.openWritingPipe(PIPE_ADDR);
  radio.stopListening();
  Serial.println("  STATUS: OK");
  Serial.print("  Channel: "); Serial.print(RF_CHANNEL);
  Serial.print(" ("); Serial.print(2400+RF_CHANNEL); Serial.println(" MHz)");
  Serial.print("  Pkt size: "); Serial.print(sizeof(txData)); Serial.println(" bytes");

  printLine('=');
  Serial.println("  Ready. Transmitting every 2s.");
  Serial.println("  [TX #N]  Dist  Temp  Hum  RF");
  printLine('=');
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
  txCount++;

  // 1. HC-SR04
  uint16_t dist_mm = readUltrasonic_mm();
  bool sonarOk = (dist_mm != 9999);

  // 2. DHT11
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  bool dhtOk = !(isnan(temp) || isnan(hum));
  if (!dhtOk) { temp = 0.0f; hum = 0.0f; }

  // 3. Build packet — embed Device ID in every transmission
  txData.temperature = temp;
  txData.humidity    = hum;
  txData.waterLevel  = 0;
  txData.distance_mm = sonarOk ? dist_mm : 0;
  memcpy(txData.devId, DEVICE_ID, 12);  // embed hardcoded Device ID in every packet

  // 4. Transmit
  bool rfOk = radio.write(&txData, sizeof(txData));
  if (rfOk) okCount++; else failCount++;

  // 5. Serial log
  Serial.print("[TX #"); Serial.print(txCount); Serial.print("]  ");
  Serial.print("Dist="); Serial.print(sonarOk ? String(dist_mm)+"mm" : "ERR");
  Serial.print("  T=");   Serial.print(dhtOk   ? String(temp,1)+"C"  : "ERR");
  Serial.print("  H=");   Serial.print(dhtOk   ? String(hum,1)+"%"   : "ERR");
  Serial.print("  RF=");  Serial.print(rfOk ? "OK" : "FAIL");
  Serial.print("  (OK:"); Serial.print(okCount); Serial.print("/"); Serial.print(txCount); Serial.println(")");

  if (!rfOk && (failCount % 5 == 0)) {
    printLine();
    Serial.println("  [RF FAIL x5] Check: receiver on? PIPE=HTAP1? CH=108? 100uF caps?");
    printLine();
  }

  delay(2000);
}
