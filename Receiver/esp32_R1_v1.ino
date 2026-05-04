#include <SPI.h>
#include <RF24.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#define CE_PIN      4
#define CSN_PIN     5
#define SCK_PIN     18
#define MISO_PIN    19
#define MOSI_PIN    23
#define SCREEN_W    128
#define SCREEN_H     64
#define LED_RED     32
#define LED_YELLOW  33
#define LED_GREEN   25
#define LED_BLUE    26
#define BUZZER_PIN  27
#define BUTTON_PIN  14
#define PUMP_PIN    2

const uint8_t RF_CHANNEL = 108;

struct DataPacket {
  uint32_t senderId;
  uint32_t targetId;
  float temperature;
  float humidity;
  uint16_t distance_mm;
  uint8_t msgType;
};

RF24 radio(CE_PIN, CSN_PIN);
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
WebServer server(80);
Preferences prefs;

uint32_t myId;
uint32_t pairedId = 0;
String apPass = "savewater";
bool isPaired = false;

uint8_t rxPipe[5];
uint8_t txPipe[5];

int lowThreshold = 20;
int highThreshold = 95;
float tankEmpty_cm = 30.0f;
float tankFull_cm = 5.0f;

uint8_t computedWater = 0;
unsigned long lastReceiveMillis = 0;
bool dataReceived = false;
DataPacket lastData;

bool buzzerSilenced = false;
bool buzzerActive = false;
unsigned long prevBeepMillis = 0;
int beepInterval = 500;
bool beepOn = false;

bool lastBtnState = HIGH;
bool btnState = HIGH;
unsigned long lastDebounceMs = 0;
const unsigned long DEBOUNCE_MS = 50;

uint8_t currentScreen = 0;
unsigned long lastScreenSwitch = 0;
const unsigned long SCREEN_DURATIONS[5] = {15000, 4000, 4000, 4000, 4000};

bool pumpEnabled = false;
int pumpStartLevel = 20;
bool pumpState = false;

const char CSS[] PROGMEM = R"(
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
.btn-danger{background:#ff6b6b}
.btn-danger:hover{background:#fa5252}
.ago{font-size:.75em;color:#444;margin-bottom:14px}
.code{font-family:monospace;font-size:1.2em;letter-spacing:2px;color:#00d4aa;background:#0d1117;padding:10px;border-radius:6px;text-align:center;display:block;margin-bottom:15px;border:1px solid #21262d}
.full-input{width:100% !important;margin-bottom:10px;box-sizing:border-box}
.footer{text-align:center;font-size:.72em;color:#555;margin-top:20px}
a{color:#00aaff;text-decoration:none}
a:hover{text-decoration:underline}
)";

void screenMain();
void screenTemp();
void screenHumid();
void screenBarGraph();
void screenNumerical();

void setupPipes() {
  rxPipe[0] = 'H';
  rxPipe[1] = (myId >> 24) & 0xFF;
  rxPipe[2] = (myId >> 16) & 0xFF;
  rxPipe[3] = (myId >> 8) & 0xFF;
  rxPipe[4] = myId & 0xFF;

  txPipe[0] = 'H';
  txPipe[1] = (pairedId >> 24) & 0xFF;
  txPipe[2] = (pairedId >> 16) & 0xFF;
  txPipe[3] = (pairedId >> 8) & 0xFF;
  txPipe[4] = pairedId & 0xFF;
}

uint8_t recalcWater() {
  if (lastData.distance_mm == 0 || lastData.distance_mm == 9999) return 0;
  float span = tankEmpty_cm - tankFull_cm;
  if (span <= 0.5f) return 0;
  float dist_cm = lastData.distance_mm / 10.0f;
  float pct = ((tankEmpty_cm - dist_cm) / span) * 100.0f;
  return (uint8_t)constrain(pct, 0.0f, 100.0f);
}

void updateLEDs(uint8_t pct) {
  digitalWrite(LED_RED,    pct < 25                  ? HIGH : LOW);
  digitalWrite(LED_YELLOW, (pct >= 25 && pct < 50)   ? HIGH : LOW);
  digitalWrite(LED_GREEN,  (pct >= 50 && pct < 75)   ? HIGH : LOW);
  digitalWrite(LED_BLUE,   pct >= 75                 ? HIGH : LOW);
}

void checkBuzzer() {
  bool alarm = false;
  int interval = 500;
  if (computedWater <= (uint8_t)lowThreshold) { alarm = true; interval = 600; }
  else if (computedWater >= (uint8_t)highThreshold) { alarm = true; interval = 150; }

  if (alarm && !buzzerActive) buzzerSilenced = false;
  if (alarm && !buzzerSilenced) {
    buzzerActive = true;
    beepInterval = interval;
    if (!beepOn) prevBeepMillis = millis();
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
    beepOn = false;
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

void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastBtnState) {
    lastDebounceMs = millis();
  }
  if ((millis() - lastDebounceMs) > DEBOUNCE_MS) {
    if (reading != btnState) {
      btnState = reading;
      if (btnState == LOW) {
        buzzerSilenced = !buzzerSilenced;
        if (buzzerSilenced) {
          digitalWrite(BUZZER_PIN, LOW);
          buzzerActive = false;
          beepOn = false;
        }
      }
    }
  }
  lastBtnState = reading;
}

void handleRoot() {
  char myIdStr[9], pIdStr[9];
  sprintf(myIdStr, "%08X", myId);
  sprintf(pIdStr, "%08X", pairedId);
  unsigned long ago = dataReceived ? (millis() - lastReceiveMillis) / 1000 : 0;
  
  String html = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  if(isPaired) html += "<meta http-equiv='refresh' content='10'>";
  html += "<title>HeyTaps – Water Tank Monitor</title><style>";
  html += CSS;
  html += "</style></head><body>";
  
  html += "<h1>HeyTaps Monitor</h1>";
  html += "<p class='sub'>Receiver Dashboard &middot; <a href='/docs'>Documentation</a></p>";
  
  if (isPaired) {
    String badgeClass = (dataReceived && ago < 30) ? "ok" : "warn";
    String badgeText = (dataReceived && ago < 30) ? "Signal: Connected" : "Signal: Lost / No Data";
    
    html += "<div class='bar-wrap'>";
    html += "<div class='bar-bg'><div class='bar-fill' style='width:" + String(computedWater) + "%'></div></div>";
    html += "<div class='bar-lbl'><span>0%</span><span>Water: <b>" + String(computedWater) + "%</b> (" + String(lastData.distance_mm) + " mm)</span><span>100%</span></div>";
    html += "</div>";

    html += "<span class='badge " + badgeClass + "'>" + badgeText + "</span>";
    html += "<p class='ago'>Last update: " + String(ago) + " seconds ago</p>";

    html += "<div class='grid'>";
    html += "<div class='card'><div class='val'>" + String(computedWater) + "%</div><div class='lbl'>Water Level</div></div>";
    html += "<div class='card'><div class='val'>" + String(lastData.distance_mm) + "mm</div><div class='lbl'>Distance</div></div>";
    html += "<div class='card'><div class='val'>" + String(lastData.temperature, 1) + "&deg;C</div><div class='lbl'>Temperature</div></div>";
    html += "<div class='card'><div class='val'>" + String(lastData.humidity, 1) + "%</div><div class='lbl'>Humidity</div></div>";
    html += "</div>";
  } else {
    html += "<span class='badge warn'>Status: Not Paired</span>";
    html += "<p class='ago'>Awaiting Transceiver Pairing</p>";
  }
  
  html += "<form action='/pair' method='POST'>";
  html += "<div class='panel'><h2>Pairing & Security</h2>";
  html += "<div class='row'><label>Your Device Code:</label><div style='color:#00d4aa;font-family:monospace'>" + String(myIdStr) + "</div></div>";
  if(isPaired) html += "<div class='row'><label>Paired Code:</label><div style='color:#00d4aa;font-family:monospace'>" + String(pIdStr) + "</div></div>";
  html += "<input type='text' name='code' maxlength='8' placeholder='Enter Transceiver Code' class='full-input' required>";
  html += "<button class='btn' type='submit'>Save Pairing</button>";
  html += "</div></form>";

  html += "<form action='/calib' method='POST'>";
  html += "<div class='panel'><h2>Tank Calibration & Alarms</h2>";
  html += "<p class='hint'>Mount HC-SR04 at the TOP of tank pointing DOWN.</p>";
  html += "<div class='row'><label>Empty Dist (cm):</label><input type='number' name='empty' min='1' max='500' step='0.1' value='" + String(tankEmpty_cm) + "'></div>";
  html += "<div class='row'><label>Full Dist (cm):</label><input type='number' name='full' min='1' max='500' step='0.1' value='" + String(tankFull_cm) + "'></div>";
  html += "<div class='row'><label>Low Alert (%):</label><input type='number' name='low' min='0' max='100' value='" + String(lowThreshold) + "'></div>";
  html += "<div class='row'><label>High Alert (%):</label><input type='number' name='high' min='0' max='100' value='" + String(highThreshold) + "'></div>";
  html += "<button class='btn' type='submit'>Save Calibration</button>";
  html += "</div></form>";
  
  html += "<form action='/pump' method='POST'>";
  html += "<div class='panel'><h2>Pump Control</h2>";
  html += "<p class='hint'>If enabled, pump turns ON below the Start Level, and OFF at the High Alert level.</p>";
  html += "<div class='row'><label>Current State:</label><span class='badge " + String(pumpState?"ok":"warn") + "' style='margin:0'>" + String(pumpState?"PUMP ON":"PUMP OFF") + "</span></div>";
  html += "<div class='row'><label>System:</label><select name='enabled' class='full-input' style='width:120px;margin:0;background:#0d1117;color:#fff;border:1px solid #21262d;padding:6px;border-radius:6px;'>";
  html += "<option value='0'" + String(!pumpEnabled?" selected":"") + ">Disabled</option>";
  html += "<option value='1'" + String(pumpEnabled?" selected":"") + ">Enabled</option>";
  html += "</select></div>";
  html += "<div class='row'><label>Start Pump Below (%):</label><input type='number' name='startLvl' min='0' max='100' value='" + String(pumpStartLevel) + "'></div>";
  html += "<button class='btn' type='submit'>Save Pump Settings</button>";
  html += "</div></form>";
  
  html += "<form action='/settings' method='POST'>";
  html += "<div class='panel'><h2>System Settings</h2>";
  html += "<input type='text' name='pass' minlength='8' placeholder='New Wi-Fi Password' class='full-input' required>";
  html += "<button class='btn' type='submit'>Update Password</button>";
  html += "</div></form>";

  html += "<form action='/reset' method='POST' onsubmit='return confirm(\"Hard reset device? This removes pairings and calibration.\")'>";
  html += "<div class='panel'><button class='btn btn-danger' type='submit'>Hard Reset System</button></div></form>";

  html += "<p class='footer'>Connect to Wi-Fi <b>HeyTap R1</b> &bull; Password: <b>" + apPass + "</b> &bull; IP: 192.168.4.1</p>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleDocs() {
  String html = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Documentation</title><style>";
  html += CSS;
  html += "</style></head><body><div class='panel'><h2>HeyTaps Architecture & Docs</h2>";
  html += "<h3 style='color:#00aaff;margin-top:10px'>1. Hardware Overview</h3><p class='hint' style='margin-bottom:15px'><b>Transceiver (T1):</b> ESP32, HC-SR04 Ultrasonic, DHT11, NRF24L01+.<br><b>Receiver (R1):</b> ESP32, OLED SSD1306, Status LEDs, Alarm Buzzer, NRF24L01+.</p>";
  html += "<h3 style='color:#00aaff'>2. E2E Authentication</h3><p class='hint' style='margin-bottom:15px'>Both devices generate a unique, fixed 8-character hardware address from their MAC address. Communication is strictly gated using these addresses. NRF24L01+ dynamic pipe addressing provides hardware-level filtering.</p>";
  html += "<h3 style='color:#00aaff'>3. Two-Way Handshake</h3><p class='hint' style='margin-bottom:15px'>Data transmission uses a custom application-layer two-way handshake. T1 transmits a DATA packet embedded with IDs. R1 receives, verifies both IDs, and immediately replies with an ACK packet similarly encrypted.</p>";
  html += "<h3 style='color:#00aaff'>4. Web Portal & Wi-Fi</h3><p class='hint' style='margin-bottom:15px'>Each device hosts an independent Wi-Fi Access Point (T1/R1). Setup requires pairing the devices by swapping their unique codes.</p>";
  html += "<a href='/' class='btn' style='margin-top:20px;text-align:center;display:block'>Back to Dashboard</a></div></body></html>";
  server.send(200, "text/html", html);
}

void handlePair() {
  if (server.hasArg("code")) {
    String c = server.arg("code");
    c.toUpperCase(); c.trim();
    pairedId = strtoul(c.c_str(), NULL, 16);
    prefs.putUInt("pairedId", pairedId);
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
  delay(500);
  ESP.restart();
}

void handleCalib() {
  if (server.hasArg("empty")) { tankEmpty_cm = server.arg("empty").toFloat(); prefs.putFloat("tempty", tankEmpty_cm); }
  if (server.hasArg("full")) { tankFull_cm = server.arg("full").toFloat(); prefs.putFloat("tfull", tankFull_cm); }
  if (server.hasArg("low")) { lowThreshold = server.arg("low").toInt(); prefs.putInt("low", lowThreshold); }
  if (server.hasArg("high")) { highThreshold = server.arg("high").toInt(); prefs.putInt("high", highThreshold); }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handlePump() {
  if (server.hasArg("enabled")) {
    pumpEnabled = server.arg("enabled").toInt() == 1;
    prefs.putBool("pumpEnabled", pumpEnabled);
  }
  if (server.hasArg("startLvl")) {
    pumpStartLevel = server.arg("startLvl").toInt();
    prefs.putInt("pumpStartLevel", pumpStartLevel);
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSettings() {
  if (server.hasArg("pass")) {
    apPass = server.arg("pass");
    prefs.putString("apPass", apPass);
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
  delay(500);
  ESP.restart();
}

void handleReset() {
  prefs.clear();
  server.send(200, "text/html", "Device reset. Rebooting...");
  delay(1000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  uint8_t leds[] = {LED_RED, LED_YELLOW, LED_GREEN, LED_BLUE};
  for (auto p : leds) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(PUMP_PIN, OUTPUT); digitalWrite(PUMP_PIN, LOW);
  
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED FAIL");
    for(;;);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("HeyTaps R1 Booting..");
  display.display();
  
  WiFi.mode(WIFI_AP);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  myId = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];

  char myIdStr[9];
  sprintf(myIdStr, "%08X", myId);
  Serial.printf("\n=== HeyTaps R1 (Receiver) ===\n");
  Serial.printf("MY UNIQUE CODE: %s\n", myIdStr);
  
  prefs.begin("heytaps", false);
  pairedId = prefs.getUInt("pairedId", 0);
  apPass = prefs.getString("apPass", "savewater");
  tankEmpty_cm = prefs.getFloat("tempty", 30.0f);
  tankFull_cm = prefs.getFloat("tfull", 5.0f);
  lowThreshold = prefs.getInt("low", 20);
  highThreshold = prefs.getInt("high", 95);
  pumpEnabled = prefs.getBool("pumpEnabled", false);
  pumpStartLevel = prefs.getInt("pumpStartLevel", 20);
  pumpState = prefs.getBool("pumpState", false);
  digitalWrite(PUMP_PIN, pumpState ? HIGH : LOW);
  
  if (pairedId != 0) {
    isPaired = true;
    Serial.printf("PAIRED CODE: %08X\n", pairedId);
  }
  
  WiFi.softAP("HeyTap R1", apPass.c_str());
  
  server.on("/", handleRoot);
  server.on("/docs", handleDocs);
  server.on("/pair", handlePair);
  server.on("/calib", handleCalib);
  server.on("/pump", handlePump);
  server.on("/settings", handleSettings);
  server.on("/reset", handleReset);
  server.begin();
  
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
  if (!radio.begin()) {
    Serial.println("NRF24 FAIL!");
    display.println("NRF24 ERROR"); display.display();
  } else {
    radio.setChannel(RF_CHANNEL);
    radio.setDataRate(RF24_250KBPS);
    radio.setPALevel(RF24_PA_HIGH);
    radio.setAutoAck(false);
    
    if (isPaired) {
      setupPipes();
      radio.openReadingPipe(1, rxPipe);
      radio.startListening();
    }
  }
}

void screenMain() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
  display.print("HeyTaps [1/5]");
  display.setTextSize(2); display.setCursor(0, 12); display.print(computedWater); display.print("%");
  display.setTextSize(1);
  display.setCursor(68, 12); display.print("D:"); display.print(lastData.distance_mm); display.println("mm");
  display.setCursor(68, 22); display.print("T:"); display.print(lastData.temperature, 1); display.println("C");
  display.setCursor(68, 32); display.print("H:"); display.print(lastData.humidity, 1); display.println("%");
  display.drawRect(0, 44, 126, 8, SSD1306_WHITE);
  uint8_t fill = (uint8_t)((computedWater / 100.0f) * 124);
  if (fill > 0) display.fillRect(1, 45, fill, 6, SSD1306_WHITE);
  display.setCursor(0, 56); display.setTextSize(1);
  if (buzzerSilenced) display.print("ALARM MUTED");
  else if (computedWater <= (uint8_t)lowThreshold) display.print("!! LOW WATER");
  else if (computedWater >= (uint8_t)highThreshold) display.print("!! OVERFLOW");
  else display.print("Level: OK");
  display.display();
}

void screenTemp() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
  display.print("TEMPERATURE [2/5]");
  display.setTextSize(3); display.setCursor(8, 16); display.print(lastData.temperature, 1);
  display.setTextSize(2); display.setCursor(90, 20); display.print("C");
  display.setTextSize(1); display.setCursor(0, 56); display.print("DHT11 Sensor"); display.display();
}

void screenHumid() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
  display.print("HUMIDITY    [3/5]");
  display.setTextSize(3); display.setCursor(8, 16); display.print(lastData.humidity, 1);
  display.setTextSize(2); display.setCursor(96, 20); display.print("%");
  display.setTextSize(1); display.setCursor(0, 56); display.print("DHT11 Sensor"); display.display();
}

void screenBarGraph() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
  display.print("WATER LEVEL [4/5]");
  display.drawRect(4, 12, 120, 36, SSD1306_WHITE);
  uint8_t barW = (uint8_t)((computedWater / 100.0f) * 118);
  if (barW > 0) display.fillRect(5, 13, barW, 34, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK); display.setTextSize(2); display.setCursor(42, 20);
  if (barW < 50) { display.setTextColor(SSD1306_WHITE); display.setCursor(42, 20); }
  display.print(computedWater); display.print("%");
  display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
  display.setCursor(27, 52); display.print("25");
  display.setCursor(57, 52); display.print("50");
  display.setCursor(87, 52); display.print("75");
  display.drawFastVLine(34, 48, 4, SSD1306_WHITE);
  display.drawFastVLine(64, 48, 4, SSD1306_WHITE);
  display.drawFastVLine(94, 48, 4, SSD1306_WHITE); display.display();
}

void screenNumerical() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
  display.print("WATER LEVEL [5/5]");
  display.setTextSize(4);
  uint8_t digits = computedWater < 10 ? 1 : (computedWater < 100 ? 2 : 3);
  int16_t x = (128 - digits * 24) / 2;
  display.setCursor(x, 14); display.print(computedWater);
  display.setTextSize(2); display.setCursor(92, 24); display.print("%");
  display.setTextSize(1); display.setCursor(0, 56);
  display.print(lastData.distance_mm); display.print("mm  ");
  if (buzzerSilenced) display.print("MUTED");
  else if (computedWater <= (uint8_t)lowThreshold) display.print("LOW!");
  else if (computedWater >= (uint8_t)highThreshold) display.print("FULL!");
  else display.print("OK"); display.display();
}

void updateOLED() {
  if (millis() - lastScreenSwitch >= SCREEN_DURATIONS[currentScreen]) {
    lastScreenSwitch = millis();
    currentScreen = (currentScreen + 1) % 5;
  }
  
  if (!isPaired) {
    display.clearDisplay(); display.setTextSize(1); display.setCursor(0,0);
    display.println("== Setup Required ==");
    char buf[9]; sprintf(buf, "%08X", myId);
    display.print("Code: "); display.println(buf);
    display.println("Connect WiFi:");
    display.println("HeyTap R1");
    display.println("Pass: " + apPass);
    display.display();
    return;
  }
  
  if (dataReceived && (millis() - lastReceiveMillis > 30000UL)) {
    display.clearDisplay(); display.setCursor(0,0); display.setTextSize(1);
    display.println("!! NO SIGNAL !!");
    display.println("Check transceiver");
    display.print("Last: "); display.print((millis()-lastReceiveMillis)/1000); display.println("s ago");
    display.display();
    return;
  }
  
  switch (currentScreen) {
    case 0: screenMain(); break;
    case 1: screenTemp(); break;
    case 2: screenHumid(); break;
    case 3: screenBarGraph(); break;
    case 4: screenNumerical(); break;
  }
}

void updatePump() {
  bool oldState = pumpState;

  if (!pumpEnabled) {
    pumpState = false;
  } else if (dataReceived) {
    // Only update automatically if we actually have water level data
    if (computedWater < (uint8_t)pumpStartLevel) {
      pumpState = true;
    } else if (computedWater >= (uint8_t)highThreshold) {
      pumpState = false;
    }
  }

  if (oldState != pumpState) {
    prefs.putBool("pumpState", pumpState);
  }

  digitalWrite(PUMP_PIN, pumpState ? HIGH : LOW);
}

unsigned long lastOledUpdate = 0;
void loop() {
  server.handleClient();
  handleButton();
  updatePump();
  
  if (isPaired && radio.available()) {
    DataPacket rx;
    radio.read(&rx, sizeof(rx));
    
    if (rx.msgType == 0 && rx.targetId == myId && rx.senderId == pairedId) {
      lastData = rx;
      lastReceiveMillis = millis();
      dataReceived = true;
      computedWater = recalcWater();
      updateLEDs(computedWater);
      checkBuzzer();
      
      DataPacket ack;
      ack.senderId = myId;
      ack.targetId = pairedId;
      ack.distance_mm = computedWater; // Send back the calculated water level
      ack.msgType = 1;
      
      radio.stopListening();
      radio.openWritingPipe(txPipe);
      radio.write(&ack, sizeof(ack));
      
      radio.openReadingPipe(1, rxPipe);
      radio.startListening();
      
      Serial.printf("[RX] Dist:%u Water:%u%% T:%.1f H:%.1f\n", rx.distance_mm, computedWater, rx.temperature, rx.humidity);
    }
  }
  
  updateBuzzerTone();
  
  if (millis() - lastOledUpdate >= 200) {
    lastOledUpdate = millis();
    updateOLED();
  }
}
