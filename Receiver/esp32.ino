/******************************************************************************
 * HEYTAPS – RECEIVER
 * Board   : ESP32 (38-pin DevKit / Wroom-32)
 * Display : SSD1306 OLED 128x64 (I2C)
 * Radio   : NRF24L01+ (2.4 GHz)
 * Extras  : Buzzer, 4x LEDs, Push Button (mute buzzer), Wi-Fi Web Dashboard
 *
 * WIRING TABLE
 * ┌──────────────┬──────────┬─────────────────────────────────────────────┐
 * │  Component   │ ESP32    │  Notes                                      │
 * ├──────────────┼──────────┼─────────────────────────────────────────────┤
 * │ NRF24  VCC   │ 3V3      │ 3.3V ONLY. Add 100uF cap on VCC/GND        │
 * │ NRF24  GND   │ GND      │                                             │
 * │ NRF24  CE    │ GPIO4    │                                             │
 * │ NRF24  CSN   │ GPIO5    │                                             │
 * │ NRF24  SCK   │ GPIO18   │ VSPI clock                                  │
 * │ NRF24  MISO  │ GPIO19   │ VSPI MISO                                   │
 * │ NRF24  MOSI  │ GPIO23   │ VSPI MOSI                                   │
 * ├──────────────┼──────────┼─────────────────────────────────────────────┤
 * │ OLED   VCC   │ 3V3      │                                             │
 * │ OLED   GND   │ GND      │                                             │
 * │ OLED   SDA   │ GPIO21   │ I2C default                                 │
 * │ OLED   SCL   │ GPIO22   │ I2C default                                 │
 * ├──────────────┼──────────┼─────────────────────────────────────────────┤
 * │ LED Red +    │ GPIO32   │ Via 220 ohm resistor. Water < 25%           │
 * │ LED Yellow + │ GPIO33   │ Via 220 ohm resistor. Water 25-49%          │
 * │ LED Green +  │ GPIO25   │ Via 220 ohm resistor. Water 50-74%          │
 * │ LED Blue +   │ GPIO26   │ Via 220 ohm resistor. Water >= 75%          │
 * │ All LED -    │ GND      │                                             │
 * ├──────────────┼──────────┼─────────────────────────────────────────────┤
 * │ Buzzer +     │ GPIO27   │ Active 5V buzzer. Use NPN transistor for 5V │
 * │ Buzzer -     │ GND      │                                             │
 * ├──────────────┼──────────┼─────────────────────────────────────────────┤
 * │ Button leg 1 │ GPIO14   │ INPUT_PULLUP. Press to mute/unmute buzzer   │
 * │ Button leg 2 │ GND      │                                             │
 * └──────────────┴──────────┴─────────────────────────────────────────────┘
 *
 * WEB DASHBOARD
 *   Connect to Wi-Fi: HeyTaps  |  Password: 12345678  |  IP: 192.168.4.1
 *   - Live water level bar, sensor readings, status badge
 *   - Alarm threshold settings (low %, high %)
 *   - Tank distance calibration (empty cm, full cm) – saved to flash
 ******************************************************************************/

#include <SPI.h>
#include <RF24.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// =============================================================================
//  PIN DEFINITIONS
// =============================================================================
// NRF24L01  (VSPI: SCK=18, MISO=19, MOSI=23 – default, no SPI.begin needed)
#define CE_PIN      4
#define CSN_PIN     5
// OLED (I2C default: SDA=21, SCL=22)
#define SCREEN_W    128
#define SCREEN_H     64
// LEDs
#define LED_RED     32
#define LED_YELLOW  33
#define LED_GREEN   25
#define LED_BLUE    26
// Buzzer
#define BUZZER_PIN  27
// Push button (mute buzzer) – connect between GPIO14 and GND
#define BUTTON_PIN  14

// =============================================================================
//  RF24  (must match transmitter exactly)
// =============================================================================
const byte    PIPE_ADDR[6] = "HTAP1";
const uint8_t RF_CHANNEL   = 108;

// =============================================================================
//  SHARED DATA PACKET  –  IDENTICAL struct on both ends (23 bytes)
// =============================================================================
struct SensorData {
  float    temperature;   // degC   (4 bytes)
  float    humidity;      // %RH    (4 bytes)
  uint8_t  waterLevel;    // unused (1 byte)
  uint16_t distance_mm;   // mm     (2 bytes)
  char     devId[12];     // Device ID, no null (12 bytes)
};
SensorData rxData;

// =============================================================================
//  OBJECTS
// =============================================================================
RF24               radio(CE_PIN, CSN_PIN);
Adafruit_SSD1306   display(SCREEN_W, SCREEN_H, &Wire, -1);
WebServer          server(80);
Preferences        prefs;

// =============================================================================
//  WI-FI ACCESS POINT
// =============================================================================
const char* AP_SSID     = "HeyTaps";
const char* AP_PASSWORD = "savewater";
IPAddress   AP_IP(192, 168, 4, 1);

// =============================================================================
//  PERSISTENT SETTINGS  (NVS flash)
// =============================================================================
int   lowThreshold  = 20;    // buzzer alarm when water level below this %
int   highThreshold = 95;    // buzzer alarm when water level above this %
float tankEmpty_cm  = 30.0f; // sensor-to-bottom distance when tank is empty
float tankFull_cm   =  5.0f; // sensor-to-water  distance when tank is full

// =============================================================================
//  RUNTIME STATE
// =============================================================================
uint8_t       computedWater     = 0;
unsigned long lastReceiveMillis = 0;
bool          dataReceived      = false;

// --- Security ---
char pairedId[13]     = {0};  // 12-char device ID saved in NVS after pairing
bool devicePaired     = false;
bool webAuthenticated = false; // true after successful login; reset on restart

// --- Buzzer (non-blocking) ---
bool          buzzerActive       = false;
bool          buzzerSilenced     = false; // true: button muted the buzzer
unsigned long prevBeepMillis     = 0;
int           beepInterval       = 500;
bool          beepOn             = false;

// --- Button debounce ---
bool          lastBtnState       = HIGH;
unsigned long lastDebounceMs     = 0;
const unsigned long DEBOUNCE_MS  = 50;

// --- OLED screen cycling ---
// Screens: 0=Main  1=Temperature  2=Humidity  3=Bar Graph  4=Numerical
uint8_t       currentScreen      = 0;
unsigned long lastScreenSwitch   = 0;
// Screen durations: screen 0 (Main) = 15s, screens 1-4 = 4s each
const unsigned long SCREEN_DURATIONS[5] = {15000, 4000, 4000, 4000, 4000};

// =============================================================================
//  FORWARD DECLARATIONS
// =============================================================================
uint8_t recalcWater();
void    handleButton();
void    updateLEDs(uint8_t pct);
void    updateOLED();
void    screenMain();
void    screenTemp();
void    screenHumid();
void    screenBarGraph();
void    screenNumerical();
void    checkBuzzer();
void    updateBuzzerTone();
bool    isAuthenticated();
void    handleRoot();
void    handleLogin();
void    handlePair();
void    handleLogout();
void    handleSet();

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[BOOT] HeyTaps Receiver starting...");

  // --- LEDs ------------------------------------------------------------------
  uint8_t leds[] = {LED_RED, LED_YELLOW, LED_GREEN, LED_BLUE};
  for (auto p : leds) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }

  // --- Buzzer ----------------------------------------------------------------
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // --- Button ----------------------------------------------------------------
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("[BOOT] Buzzer-mute button ready on GPIO14");

  // --- OLED ------------------------------------------------------------------
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED SSD1306 not found! Check SDA=GPIO21, SCL=GPIO22");
    for (;;);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("HeyTaps v3.0");
  display.println("Booting...");
  display.display();

  // --- Load settings from NVS ------------------------------------------------
  prefs.begin("heytaps", false);
  lowThreshold  = prefs.getInt  ("low",    20);
  highThreshold = prefs.getInt  ("high",   95);
  tankEmpty_cm  = prefs.getFloat("tempty", 30.0f);
  tankFull_cm   = prefs.getFloat("tfull",   5.0f);
  String pid = prefs.getString("pairedid", "");
  if (pid.length() == 12) {
    pid.toCharArray(pairedId, 13);
    devicePaired = true;
    Serial.printf("[SEC] Paired device ID: %s\n", pairedId);
  } else {
    Serial.println("[SEC] No device paired yet. Open web portal to pair.");
  }
  Serial.printf("[BOOT] Thresholds: LOW=%d%%  HIGH=%d%%\n", lowThreshold, highThreshold);
  Serial.printf("[BOOT] Tank cal  : Empty=%.1fcm  Full=%.1fcm\n", tankEmpty_cm, tankFull_cm);

  // --- Wi-Fi AP -------------------------------------------------------------
  // Full reset before starting AP prevents stale state from causing auth failures
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  // Channel 6, not hidden, max 4 clients
  bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 4);
  delay(1000);  // must wait for AP to fully start before clients can connect
  Serial.println("[BOOT] ===== Wi-Fi Access Point =====");
  Serial.printf("[BOOT]   Status  : %s\n", apOk ? "STARTED" : "FAILED");
  Serial.printf("[BOOT]   SSID    : %s\n", AP_SSID);
  Serial.printf("[BOOT]   Password: %s\n", AP_PASSWORD);
  Serial.printf("[BOOT]   IP      : %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("[BOOT] ==================================");
  if (!apOk) Serial.println("[BOOT] WARNING: AP failed! Check password >= 8 chars.");

  // --- Web server routes -----------------------------------------------------
  server.on("/",       handleRoot);
  server.on("/set",    handleSet);
  server.on("/login",  handleLogin);
  server.on("/pair",   handlePair);
  server.on("/logout", handleLogout);
  const char* hdrs[] = {"Cookie"};
  server.collectHeaders(hdrs, 1);  // MUST be before begin() so Cookie header is read
  server.begin();
  Serial.println("[BOOT] HTTP server started");

  // --- NRF24L01+ -------------------------------------------------------------
  if (!radio.begin()) {
    Serial.println("[ERROR] NRF24L01 not found! Check wiring and 3.3V supply.");
    display.clearDisplay(); display.setCursor(0,0);
    display.println("NRF24 ERROR!"); display.println("Check wiring."); display.display();
    while (true) delay(1000);
  }
  radio.setChannel(RF_CHANNEL);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_HIGH);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(true);
  radio.openReadingPipe(1, PIPE_ADDR); // pipe 1 (pipe 0 reserved for ACK)
  radio.startListening();

  Serial.printf("[BOOT] NRF24 listening on CH=%d (%d MHz), pipe=\"%s\"\n",
                RF_CHANNEL, 2400 + RF_CHANNEL, (char*)PIPE_ADDR);

  // --- Ready splash (shows WiFi credentials so user knows what to enter) ---
  display.clearDisplay(); display.setCursor(0,0);
  display.setTextSize(1);
  display.println("== HeyTaps Ready ==");
  display.print("WiFi: ");    display.println(AP_SSID);
  display.print("Pass: ");    display.println(AP_PASSWORD);
  display.print("IP: ");      display.println(WiFi.softAPIP());
  display.display();
  delay(3000);  // show for 3 seconds so user can read it
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
  server.handleClient();
  handleButton();

  // --- Auto-advance OLED screen (Main=15s, others=4s) ----------------------
  if (millis() - lastScreenSwitch >= SCREEN_DURATIONS[currentScreen]) {
    lastScreenSwitch = millis();
    currentScreen = (currentScreen + 1) % 5;
    updateOLED();
  }

  // --- Receive RF data -------------------------------------------------------
  if (radio.available()) {
    radio.read(&rxData, sizeof(rxData));

    // Security gate: ONLY accept if device is paired AND devId matches exactly
    char inId[13] = {0};
    memcpy(inId, rxData.devId, 12);

    bool paired = devicePaired;
    bool idMatch = (strncmp(inId, pairedId, 12) == 0);

    if (!paired) {
      // No device paired yet – ignore all incoming data
      Serial.println("[SEC] Data blocked: no device paired. Use web portal to pair.");
    } else if (!idMatch) {
      // Wrong device ID – reject
      Serial.printf("[SEC] Rejected unknown device ID: %s (expected: %s)\n", inId, pairedId);
    } else {
      // Authorised – process data
      lastReceiveMillis = millis();
      dataReceived      = true;
      computedWater     = recalcWater();
      Serial.printf("[RX] ID=%s Dist=%dmm Water=%d%% T=%.1fC H=%.1f%% Muted=%s\n",
                    inId, rxData.distance_mm, computedWater,
                    rxData.temperature, rxData.humidity,
                    buzzerSilenced ? "Y" : "N");
      updateLEDs(computedWater);
      updateOLED();
      checkBuzzer();
    }
  }

  updateBuzzerTone();

  // --- No-signal warning (30s timeout) – overrides screen cycling ------------
  if (dataReceived && (millis() - lastReceiveMillis > 30000UL)) {
    unsigned long ago = (millis() - lastReceiveMillis) / 1000;
    display.clearDisplay(); display.setCursor(0,0);
    display.setTextSize(1);
    display.println("!! NO SIGNAL !!");
    display.println("Check transmitter");
    display.print("Last: "); display.print(ago); display.println("s ago");
    display.display();
  }
}

// =============================================================================
//  RECALCULATE WATER LEVEL  –  uses receiver-side tank calibration
// =============================================================================
uint8_t recalcWater() {
  if (rxData.distance_mm == 0) return 0;
  float span = tankEmpty_cm - tankFull_cm;
  if (span <= 0.5f) return 0; // guard against bad calibration
  float dist_cm = rxData.distance_mm / 10.0f;
  float pct = ((tankEmpty_cm - dist_cm) / span) * 100.0f;
  return (uint8_t)constrain(pct, 0.0f, 100.0f);
}

// =============================================================================
//  BUTTON HANDLER  –  debounced, toggle-mutes buzzer
// =============================================================================
void handleButton() {
  bool reading = digitalRead(BUTTON_PIN); // LOW when pressed
  if (reading != lastBtnState) lastDebounceMs = millis();
  if ((millis() - lastDebounceMs) > DEBOUNCE_MS) {
    if (reading == LOW && lastBtnState == HIGH) {       // falling edge
      buzzerSilenced = !buzzerSilenced;
      if (buzzerSilenced) {
        digitalWrite(BUZZER_PIN, LOW);
        buzzerActive = false;
        beepOn       = false;
        Serial.println("[BTN] Buzzer MUTED");
      } else {
        Serial.println("[BTN] Buzzer UN-MUTED");
      }
    }
  }
  lastBtnState = reading;
}

// =============================================================================
//  OLED – dispatcher
// =============================================================================
void updateOLED() {
  switch (currentScreen) {
    case 0: screenMain();      break;
    case 1: screenTemp();      break;
    case 2: screenHumid();     break;
    case 3: screenBarGraph();  break;
    case 4: screenNumerical(); break;
    default: screenMain();     break;
  }
}

// -----------------------------------------------------------------------------
// Screen 0: MAIN – all values at a glance
// -----------------------------------------------------------------------------
void screenMain() {
  display.clearDisplay();
  display.setTextSize(1);

  // Header with screen indicator dots
  display.setCursor(0, 0);
  display.print("HeyTaps ");
  display.print("[1/5]");

  // Water % – medium
  display.setTextSize(2);
  display.setCursor(0, 12);
  display.print(computedWater);
  display.print("%");

  // Right column – small stats
  display.setTextSize(1);
  display.setCursor(68, 12);
  display.print("D:"); display.print(rxData.distance_mm); display.println("mm");
  display.setCursor(68, 22);
  display.print("T:"); display.print(rxData.temperature, 1); display.println("C");
  display.setCursor(68, 32);
  display.print("H:"); display.print(rxData.humidity, 1); display.println("%");

  // Mini bar graph
  display.drawRect(0, 44, 126, 8, SSD1306_WHITE);
  uint8_t fill = (uint8_t)((computedWater / 100.0f) * 124);
  if (fill > 0) display.fillRect(1, 45, fill, 6, SSD1306_WHITE);

  // Status line
  display.setCursor(0, 56);
  display.setTextSize(1);
  if (buzzerSilenced)                              display.print("ALARM MUTED");
  else if (computedWater <= (uint8_t)lowThreshold) display.print("!! LOW WATER");
  else if (computedWater >= (uint8_t)highThreshold)display.print("!! OVERFLOW");
  else                                             display.print("Level: OK");

  display.display();
}

// -----------------------------------------------------------------------------
// Screen 1: TEMPERATURE – big font
// -----------------------------------------------------------------------------
void screenTemp() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("TEMPERATURE [2/5]");

  display.setTextSize(3);
  display.setCursor(8, 16);
  display.print(rxData.temperature, 1);

  display.setTextSize(2);
  display.setCursor(90, 20);
  display.print("C");

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("DHT11 Sensor");
  display.display();
}

// -----------------------------------------------------------------------------
// Screen 2: HUMIDITY – big font
// -----------------------------------------------------------------------------
void screenHumid() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("HUMIDITY    [3/5]");

  display.setTextSize(3);
  display.setCursor(8, 16);
  display.print(rxData.humidity, 1);

  display.setTextSize(2);
  display.setCursor(96, 20);
  display.print("%");

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("DHT11 Sensor");
  display.display();
}

// -----------------------------------------------------------------------------
// Screen 3: WATER LEVEL – large bar graph
// -----------------------------------------------------------------------------
void screenBarGraph() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("WATER LEVEL [4/5]");

  // Outer border
  display.drawRect(4, 12, 120, 36, SSD1306_WHITE);
  // Fill proportional to water level
  uint8_t barW = (uint8_t)((computedWater / 100.0f) * 118);
  if (barW > 0) display.fillRect(5, 13, barW, 34, SSD1306_WHITE);

  // Percentage label centred over bar
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(2);
  display.setCursor(42, 20);
  if (barW < 50) {                          // text outside bar when nearly empty
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(42, 20);
  }
  display.print(computedWater);
  display.print("%");
  display.setTextColor(SSD1306_WHITE);

  // Scale marks: 25 / 50 / 75
  display.setTextSize(1);
  display.setCursor(27, 52);  display.print("25");
  display.setCursor(57, 52);  display.print("50");
  display.setCursor(87, 52);  display.print("75");
  display.drawFastVLine(34,  48, 4, SSD1306_WHITE);
  display.drawFastVLine(64,  48, 4, SSD1306_WHITE);
  display.drawFastVLine(94,  48, 4, SSD1306_WHITE);

  display.display();
}

// -----------------------------------------------------------------------------
// Screen 4: WATER LEVEL – numerical only, huge font
// -----------------------------------------------------------------------------
void screenNumerical() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("WATER LEVEL [5/5]");

  display.setTextSize(4);
  // Centre the number
  uint8_t digits = computedWater < 10 ? 1 : (computedWater < 100 ? 2 : 3);
  int16_t x = (128 - digits * 24) / 2;
  display.setCursor(x, 14);
  display.print(computedWater);

  display.setTextSize(2);
  display.setCursor(92, 24);
  display.print("%");

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(rxData.distance_mm);
  display.print("mm  ");
  if (buzzerSilenced)                               display.print("MUTED");
  else if (computedWater <= (uint8_t)lowThreshold)  display.print("LOW!");
  else if (computedWater >= (uint8_t)highThreshold) display.print("FULL!");
  else                                              display.print("OK");
  display.display();
}

// =============================================================================
//  LED UPDATE
// =============================================================================
void updateLEDs(uint8_t pct) {
  digitalWrite(LED_RED,    pct < 25                  ? HIGH : LOW);
  digitalWrite(LED_YELLOW, (pct >= 25 && pct < 50)   ? HIGH : LOW);
  digitalWrite(LED_GREEN,  (pct >= 50 && pct < 75)   ? HIGH : LOW);
  digitalWrite(LED_BLUE,   pct >= 75                 ? HIGH : LOW);
}



// =============================================================================
//  BUZZER LOGIC
// =============================================================================
void checkBuzzer() {
  bool alarm    = false;
  int  interval = 500;

  if      (computedWater <= (uint8_t)lowThreshold)  { alarm = true; interval = 600; } // slow
  else if (computedWater >= (uint8_t)highThreshold) { alarm = true; interval = 150; } // fast

  // New alarm event re-arms mute so user is always notified of a NEW alarm
  if (alarm && !buzzerActive) buzzerSilenced = false;

  if (alarm && !buzzerSilenced) {
    buzzerActive = true;
    beepInterval = interval;
    if (!beepOn) prevBeepMillis = millis();
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
    beepOn       = false;
  }
}

void updateBuzzerTone() {
  if (!buzzerActive) return;
  unsigned long now = millis();
  if (now - prevBeepMillis >= (unsigned long)beepInterval) {
    prevBeepMillis = now;
    beepOn = !beepOn;
    digitalWrite(BUZZER_PIN, beepOn ? HIGH : LOW);
  }
}

// =============================================================================
//  WEB DASHBOARD HTML
// =============================================================================
const char HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta http-equiv="refresh" content="5">
  <title>HeyTaps – Water Tank Monitor</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:'Segoe UI',Arial,sans-serif;background:#0d1117;color:#e0e0e0;padding:20px;max-width:600px;margin:auto}
    h1{color:#00d4aa;font-size:1.5em;margin-bottom:2px}
    .sub{color:#555;font-size:.8em;margin-bottom:20px}
    /* --- Water bar --- */
    .bar-wrap{background:#161b22;border:1px solid #21262d;border-radius:12px;padding:16px;margin-bottom:16px}
    .bar-bg{background:#21262d;border-radius:8px;height:30px;overflow:hidden}
    .bar-fill{height:100%;border-radius:8px;background:linear-gradient(90deg,#00d4aa,#00aaff);transition:width .6s}
    .bar-lbl{display:flex;justify-content:space-between;font-size:.78em;color:#555;margin-top:6px}
    /* --- Cards --- */
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:12px;margin-bottom:16px}
    .card{background:#161b22;border:1px solid #21262d;border-radius:10px;padding:14px;text-align:center}
    .card .val{font-size:2em;font-weight:700;color:#00d4aa}
    .card .lbl{font-size:.72em;color:#777;margin-top:4px}
    /* --- Status badge --- */
    .badge{display:inline-block;padding:4px 14px;border-radius:20px;font-size:.8em;margin-bottom:14px;font-weight:600}
    .ok  {background:#0d2818;color:#00d4aa;border:1px solid #00d4aa}
    .warn{background:#2d1212;color:#ff6b6b;border:1px solid #ff6b6b}
    /* --- Forms --- */
    .panel{background:#161b22;border:1px solid #21262d;border-radius:12px;padding:16px;margin-bottom:12px}
    .panel h2{color:#00aaff;font-size:.95em;margin-bottom:10px}
    .panel p.hint{font-size:.75em;color:#555;margin-bottom:10px;line-height:1.4}
    .row{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
    .row label{font-size:.83em;color:#aaa}
    .row input{background:#0d1117;border:1px solid #21262d;color:#fff;padding:6px 10px;border-radius:6px;width:90px;font-size:.9em}
    .btn{display:block;width:100%;padding:9px;background:#00d4aa;color:#0d1117;border:none;border-radius:7px;font-weight:700;font-size:.9em;cursor:pointer;margin-top:4px}
    .btn:hover{background:#00aaff}
    .ago{font-size:.75em;color:#444;margin-bottom:14px}
    .footer{text-align:center;font-size:.72em;color:#333;margin-top:20px}
  </style>
</head>
<body>
  <h1>HeyTaps Monitor</h1>
  <p class="sub">Water Tank Dashboard  &middot;  auto-refresh 5s</p>

  <div class="bar-wrap">
    <div class="bar-bg"><div class="bar-fill" style="width:{water}%"></div></div>
    <div class="bar-lbl"><span>0%</span><span>Water: <b>{water}%</b> ({dist} mm)</span><span>100%</span></div>
  </div>

  <span class="badge {sc}">{sm}</span>
  <p class="ago">Last update: {ago} seconds ago</p>

  <div class="grid">
    <div class="card"><div class="val">{water}%</div><div class="lbl">Water Level</div></div>
    <div class="card"><div class="val">{dist}mm</div><div class="lbl">Distance</div></div>
    <div class="card"><div class="val">{temp}&deg;C</div><div class="lbl">Temperature</div></div>
    <div class="card"><div class="val">{hum}%</div><div class="lbl">Humidity</div></div>
  </div>

  <form action="/set" method="get">
    <div class="panel">
      <h2>Alarm Thresholds</h2>
      <div class="row"><label>Low Level Alert (%):</label><input type="number" name="low"  min="0" max="100" value="{lv}"></div>
      <div class="row"><label>Overflow Alert (%):</label><input  type="number" name="high" min="0" max="100" value="{hv}"></div>
      <button class="btn" type="submit">Save Thresholds</button>
    </div>
  </form>

  <form action="/set" method="get">
    <div class="panel">
      <h2>Tank Distance Calibration</h2>
      <p class="hint">Mount HC-SR04 at the TOP of tank pointing DOWN.<br>
        &bull; <b>Empty distance</b>: measure from sensor to tank bottom when tank is empty.<br>
        &bull; <b>Full distance</b>: measure from sensor to water surface when tank is full.</p>
      <div class="row"><label>Empty Distance (cm):</label><input type="number" name="tempty" min="1" max="500" step="0.5" value="{ev}"></div>
      <div class="row"><label>Full Distance (cm):</label> <input type="number" name="tfull"  min="1" max="500" step="0.5" value="{fv}"></div>
      <button class="btn" type="submit">Save Calibration</button>
    </div>
  </form>

  <p class="footer">Connect to Wi-Fi <b>HeyTaps</b> &bull; Password: <b>12345678</b> &bull; IP: 192.168.4.1</p>
</body>
</html>
)html";

// =============================================================================
//  WEB HANDLER – root
// =============================================================================
void handleRoot() {
  if (!devicePaired) { server.sendHeader("Location","/pair");  server.send(302,"text/plain",""); return; }
  if (!isAuthenticated()) { server.sendHeader("Location","/login"); server.send(302,"text/plain",""); return; }

  String page = HTML;
  unsigned long ago = (millis() - lastReceiveMillis) / 1000;

  const char *sc = "ok", *sm = "Level Normal";
  if      (!dataReceived)                               { sc="warn"; sm="No Data Yet"; }
  else if (buzzerSilenced)                              { sc="warn"; sm="ALARM MUTED (press button to re-enable)"; }
  else if (computedWater <= (uint8_t)lowThreshold)     { sc="warn"; sm="LOW WATER"; }
  else if (computedWater >= (uint8_t)highThreshold)    { sc="warn"; sm="OVERFLOW RISK"; }

  page.replace("{water}", String(computedWater));
  page.replace("{dist}",  String(rxData.distance_mm));
  page.replace("{temp}",  String(rxData.temperature, 1));
  page.replace("{hum}",   String(rxData.humidity, 1));
  page.replace("{ago}",   String(ago));
  page.replace("{lv}",    String(lowThreshold));
  page.replace("{hv}",    String(highThreshold));
  page.replace("{ev}",    String(tankEmpty_cm, 1));
  page.replace("{fv}",    String(tankFull_cm,  1));
  page.replace("{sc}",    sc);
  page.replace("{sm}",    sm);

  server.send(200, "text/html", page);
}

// =============================================================================
//  WEB HANDLER – save settings
// =============================================================================
void handleSet() {
  if (!isAuthenticated()) { server.sendHeader("Location","/login"); server.send(302,"text/plain",""); return; }
  if (server.hasArg("low")) {
    lowThreshold = constrain(server.arg("low").toInt(), 0, 100);
    prefs.putInt("low", lowThreshold);
  }
  if (server.hasArg("high")) {
    highThreshold = constrain(server.arg("high").toInt(), 0, 100);
    prefs.putInt("high", highThreshold);
  }
  if (server.hasArg("tempty")) {
    tankEmpty_cm = constrain(server.arg("tempty").toFloat(), 1.0f, 500.0f);
    prefs.putFloat("tempty", tankEmpty_cm);
  }
  if (server.hasArg("tfull")) {
    tankFull_cm = constrain(server.arg("tfull").toFloat(), 0.5f, 499.0f);
    prefs.putFloat("tfull", tankFull_cm);
  }
  Serial.printf("[CFG] Thresholds LOW=%d%%  HIGH=%d%%  TankEmpty=%.1fcm  TankFull=%.1fcm\n",
                lowThreshold, highThreshold, tankEmpty_cm, tankFull_cm);
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// =============================================================================
// =============================================================================
//  AUTH HELPER  –  simple server-side flag (reliable on local AP)
// =============================================================================
bool isAuthenticated() {
  return webAuthenticated;
}

// =============================================================================
//  LOGIN PAGE  –  user enters 12-char device code
// =============================================================================
void handleLogin() {
  if (!devicePaired) { server.sendHeader("Location","/pair"); server.send(302,"text/plain",""); return; }

  // Process submitted code
  if (server.hasArg("code")) {
    String code = server.arg("code");
    code.toUpperCase();
    code.trim();
    if (code.length() == 12 && code.equals(String(pairedId))) {
      webAuthenticated = true;  // grant access – no cookies needed
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "");
      Serial.println("[SEC] Login successful – dashboard unlocked");
      return;
    } else {
      Serial.println("[SEC] Login FAILED – wrong code");
    }
  }

  // Show login form
  String pg = "<!DOCTYPE html><html><head>"
    "<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>HeyTaps Login</title>"
    "<style>*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:Arial,sans-serif;background:#0d1117;color:#e0e0e0;"
    "display:flex;align-items:center;justify-content:center;height:100vh}"
    ".box{background:#161b22;border:1px solid #21262d;border-radius:14px;padding:32px 28px;width:320px;text-align:center}"
    "h2{color:#00d4aa;margin-bottom:6px}p{color:#666;font-size:.82em;margin-bottom:20px}"
    "input{width:100%;background:#0d1117;border:1px solid #21262d;color:#fff;"
    "padding:10px;border-radius:7px;font-size:1.1em;letter-spacing:3px;text-align:center;margin-bottom:14px}"
    "button{width:100%;background:#00d4aa;color:#0d1117;border:none;padding:10px;"
    "border-radius:7px;font-weight:700;font-size:1em;cursor:pointer}"
    "button:hover{background:#00aaff}.err{color:#ff6b6b;font-size:.82em;margin-bottom:10px}</style>"
    "</head><body><div class=box>"
    "<h2>HeyTaps</h2><p>Enter your 12-digit Device Code<br>set in the transmitter firmware</p>";
  if (server.hasArg("code")) pg += "<p class=err>Incorrect code. Try again.</p>";
  pg += "<form method=get action=/login>"
    "<input name=code maxlength=12 placeholder='A1B2C3D4E5F6' autofocus>"
    "<button type=submit>Unlock Dashboard</button></form></div></body></html>";
  server.send(200, "text/html", pg);
}

// =============================================================================
//  PAIR PAGE  –  first-time setup, stores device ID in NVS
// =============================================================================
void handlePair() {
  if (server.hasArg("code")) {
    String code = server.arg("code");
    code.toUpperCase(); code.trim();
    if (code.length() == 12) {
      code.toCharArray(pairedId, 13);
      prefs.putString("pairedid", code);
      devicePaired = true;
      Serial.printf("[SEC] Device paired: %s\n", pairedId);
      server.sendHeader("Location", "/login");
      server.send(302, "text/plain", "");
      return;
    }
  }

  String pg = "<!DOCTYPE html><html><head>"
    "<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>HeyTaps – Pair Device</title>"
    "<style>*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:Arial,sans-serif;background:#0d1117;color:#e0e0e0;"
    "display:flex;align-items:center;justify-content:center;height:100vh}"
    ".box{background:#161b22;border:1px solid #21262d;border-radius:14px;padding:32px 28px;width:340px;text-align:center}"
    "h2{color:#00d4aa;margin-bottom:6px}.step{background:#0d1117;border-radius:8px;padding:12px;"
    "font-size:.82em;color:#aaa;text-align:left;margin-bottom:18px;line-height:1.7}"
    "input{width:100%;background:#0d1117;border:1px solid #21262d;color:#fff;"
    "padding:10px;border-radius:7px;font-size:1.1em;letter-spacing:3px;text-align:center;margin-bottom:14px}"
    "button{width:100%;background:#00d4aa;color:#0d1117;border:none;padding:10px;"
    "border-radius:7px;font-weight:700;cursor:pointer}button:hover{background:#00aaff}</style>"
    "</head><body><div class=box>"
    "<h2>Pair New Device</h2>"
    "<div class=step>Enter the 12-character <b>DEVICE ID</b> defined in<br>"
    "the transmitter code (<b>DEVICE_ID</b> in esp32c3.ino)<br>"
    "then click Pair to unlock this receiver.</div>"
    "<form method=get action=/pair>"
    "<input name=code maxlength=12 placeholder='A1B2C3D4E5F6' autofocus>"
    "<button type=submit>Pair &amp; Continue</button></form></div></body></html>";
  server.send(200, "text/html", pg);
}

// =============================================================================
//  LOGOUT  –  clear session
// =============================================================================
void handleLogout() {
  webAuthenticated = false;
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
  Serial.println("[SEC] User logged out");
}
