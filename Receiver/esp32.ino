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
//  SHARED DATA PACKET  –  identical struct on both ends
// =============================================================================
struct SensorData {
  float    temperature;   // degC  (4 bytes)
  float    humidity;      // %RH   (4 bytes)
  uint8_t  waterLevel;    // unused from TX, receiver recalculates (1 byte)
  uint16_t distance_mm;   // raw ultrasonic reading in mm (2 bytes)
};                        // Total = 11 bytes
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
const char* AP_PASSWORD = "12345678";
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

// =============================================================================
//  FORWARD DECLARATIONS
// =============================================================================
uint8_t recalcWater();
void    handleButton();
void    updateLEDs(uint8_t pct);
void    updateOLED();
void    checkBuzzer();
void    updateBuzzerTone();
void    handleRoot();
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
  Serial.printf("[BOOT] Thresholds: LOW=%d%%  HIGH=%d%%\n", lowThreshold, highThreshold);
  Serial.printf("[BOOT] Tank cal  : Empty=%.1fcm  Full=%.1fcm\n", tankEmpty_cm, tankFull_cm);

  // --- Wi-Fi AP --------------------------------------------------------------
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("[BOOT] AP started. SSID=%s  IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  // --- Web server routes -----------------------------------------------------
  server.on("/",    handleRoot);
  server.on("/set", handleSet);
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

  // --- Ready splash ----------------------------------------------------------
  display.clearDisplay(); display.setCursor(0,0);
  display.println("HeyTaps Ready");
  display.print("WiFi: "); display.println(AP_SSID);
  display.print("IP: ");   display.println(WiFi.softAPIP());
  display.println("Waiting for data...");
  display.display();
  delay(1500);
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
  server.handleClient();
  handleButton();

  // --- Receive RF data -------------------------------------------------------
  if (radio.available()) {
    radio.read(&rxData, sizeof(rxData));
    lastReceiveMillis = millis();
    dataReceived      = true;
    computedWater     = recalcWater();

    Serial.printf("[RX] Dist=%dmm  Water=%d%%  Temp=%.1fC  Hum=%.1f%%  Muted=%s\n",
                  rxData.distance_mm, computedWater,
                  rxData.temperature, rxData.humidity,
                  buzzerSilenced ? "YES" : "NO");

    updateLEDs(computedWater);
    updateOLED();
    checkBuzzer();
  }

  updateBuzzerTone();

  // --- No-signal warning (30s timeout) ---------------------------------------
  if (dataReceived && (millis() - lastReceiveMillis > 30000UL)) {
    unsigned long ago = (millis() - lastReceiveMillis) / 1000;
    display.clearDisplay(); display.setCursor(0,0);
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
//  LED UPDATE
// =============================================================================
void updateLEDs(uint8_t pct) {
  digitalWrite(LED_RED,    pct < 25                  ? HIGH : LOW);
  digitalWrite(LED_YELLOW, (pct >= 25 && pct < 50)   ? HIGH : LOW);
  digitalWrite(LED_GREEN,  (pct >= 50 && pct < 75)   ? HIGH : LOW);
  digitalWrite(LED_BLUE,   pct >= 75                 ? HIGH : LOW);
}

// =============================================================================
//  OLED UPDATE
// =============================================================================
void updateOLED() {
  display.clearDisplay();

  // Title
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("=== HeyTaps ===");

  // Water % – large
  display.setTextSize(2);
  display.setCursor(0, 12);
  display.print(computedWater);
  display.print("%");

  // Sensor stats – right column
  display.setTextSize(1);
  display.setCursor(66, 12);
  display.print("D:"); display.print(rxData.distance_mm); display.println("mm");
  display.setCursor(66, 22);
  display.print("T:"); display.print(rxData.temperature, 1); display.println("C");
  display.setCursor(66, 32);
  display.print("H:"); display.print(rxData.humidity, 1); display.println("%");

  // Status bar
  display.setCursor(0, 50);
  if (buzzerSilenced) {
    display.println("ALARM MUTED     ");
  } else if (computedWater <= (uint8_t)lowThreshold) {
    display.println("!! LOW WATER !!");
  } else if (computedWater >= (uint8_t)highThreshold) {
    display.println("!! OVERFLOW !!  ");
  } else {
    display.println("Level: OK       ");
  }

  display.display();
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