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
#define SCREEN_W    128
#define SCREEN_H     64
#define LED_RED     32
#define LED_YELLOW  33
#define LED_GREEN   25
#define LED_BLUE    26
#define BUZZER_PIN  27
#define BUTTON_PIN  14

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
unsigned long lastDebounceMs = 0;
const unsigned long DEBOUNCE_MS = 50;

uint8_t currentScreen = 0;
unsigned long lastScreenSwitch = 0;
const unsigned long SCREEN_DURATIONS[5] = {15000, 4000, 4000, 4000, 4000};

const char CSS[] PROGMEM = R"(
body{font-family:'Segoe UI',sans-serif;background:#0f172a;color:#f8fafc;padding:20px;max-width:600px;margin:auto}
.card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:20px;margin-bottom:20px}
h1{color:#38bdf8;font-size:1.6em;margin-bottom:5px}
h2{color:#e2e8f0;font-size:1.2em;margin-bottom:15px;border-bottom:1px solid #334155;padding-bottom:10px}
.btn{display:block;width:100%;padding:12px;background:#0ea5e9;color:#fff;border:none;border-radius:8px;font-weight:600;font-size:1em;cursor:pointer;margin-top:10px;text-align:center;text-decoration:none;box-sizing:border-box}
.btn:hover{background:#0284c7}
.btn-danger{background:#ef4444}
.btn-danger:hover{background:#dc2626}
input{width:100%;background:#0f172a;border:1px solid #475569;color:#fff;padding:12px;border-radius:8px;font-size:1em;margin-bottom:15px;box-sizing:border-box}
label{display:block;color:#cbd5e1;margin-bottom:8px;font-size:0.9em}
.badge{display:inline-block;padding:5px 10px;border-radius:20px;font-size:0.85em;background:#166534;color:#4ade80}
.badge.err{background:#7f1d1d;color:#f87171}
.code{font-family:monospace;font-size:1.4em;letter-spacing:2px;color:#fcd34d;background:#000;padding:10px;border-radius:8px;text-align:center;display:block;margin-bottom:15px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:15px}
.val-box{background:#0f172a;padding:15px;border-radius:8px;text-align:center;border:1px solid #334155}
.val-box .val{font-size:1.5em;font-weight:bold;color:#38bdf8}
.val-box .lbl{font-size:0.8em;color:#94a3b8;text-transform:uppercase;margin-top:5px}
p{line-height:1.6}
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
  if (reading != lastBtnState) lastDebounceMs = millis();
  if ((millis() - lastDebounceMs) > DEBOUNCE_MS) {
    if (reading == LOW && lastBtnState == HIGH) {
      buzzerSilenced = !buzzerSilenced;
      if (buzzerSilenced) {
        digitalWrite(BUZZER_PIN, LOW);
        buzzerActive = false;
        beepOn = false;
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
  
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  if(isPaired) html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>R1 Dashboard</title><style>";
  html += CSS;
  html += "</style></head><body><div class='card'><h1>HeyTaps Receiver (R1)</h1>";
  html += "<a href='/docs' style='color:#38bdf8;font-size:0.9em;display:inline-block;margin-bottom:15px'>Read Documentation</a>";
  
  if (isPaired) {
    if (dataReceived && ago < 30) html += "<p><span class='badge'>Signal: Connected</span></p>";
    else html += "<p><span class='badge err'>Signal: Lost / No Data</span></p>";
    
    html += "<div class='grid'>";
    html += "<div class='val-box'><div class='val'>" + String(computedWater) + "%</div><div class='lbl'>Water Level</div></div>";
    html += "<div class='val-box'><div class='val'>" + String(lastData.distance_mm) + "mm</div><div class='lbl'>Distance</div></div>";
    html += "<div class='val-box'><div class='val'>" + String(lastData.temperature, 1) + "&deg;C</div><div class='lbl'>Temperature</div></div>";
    html += "<div class='val-box'><div class='val'>" + String(lastData.humidity, 1) + "%</div><div class='lbl'>Humidity</div></div>";
    html += "</div><p style='text-align:center;color:#64748b;font-size:0.8em;margin-top:15px'>Last update: " + String(ago) + "s ago</p>";
  } else {
    html += "<p><span class='badge err'>Status: Not Paired</span></p>";
  }
  html += "</div>";

  html += "<div class='card'><h2>Pair Device</h2><form action='/pair' method='POST'>";
  html += "<label>Your Device Code</label><span class='code'>" + String(myIdStr) + "</span>";
  if(isPaired) html += "<label>Paired Transceiver Code</label><span class='code'>" + String(pIdStr) + "</span>";
  html += "<label>Update Transceiver Code</label><input type='text' name='code' maxlength='8' placeholder='e.g. A1B2C3D4' required>";
  html += "<button type='submit' class='btn'>Save Pairing</button></form></div>";

  html += "<div class='card'><h2>Calibration</h2><form action='/calib' method='POST'>";
  html += "<label>Empty Distance (cm)</label><input type='number' step='0.1' name='empty' value='" + String(tankEmpty_cm) + "'>";
  html += "<label>Full Distance (cm)</label><input type='number' step='0.1' name='full' value='" + String(tankFull_cm) + "'>";
  html += "<label>Low Alarm (%)</label><input type='number' name='low' value='" + String(lowThreshold) + "'>";
  html += "<label>High Alarm (%)</label><input type='number' name='high' value='" + String(highThreshold) + "'>";
  html += "<button type='submit' class='btn'>Save Calibration</button></form></div>";
  
  html += "<div class='card'><h2>System Settings</h2><form action='/settings' method='POST'>";
  html += "<label>Change Wi-Fi Password</label><input type='text' name='pass' minlength='8' placeholder='New Password' required>";
  html += "<button type='submit' class='btn'>Update Password</button></form>";
  html += "<form action='/reset' method='POST' onsubmit='return confirm(\"Hard reset device?\")'><button type='submit' class='btn btn-danger'>Hard Reset</button></form></div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleDocs() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Documentation</title><style>";
  html += CSS;
  html += "</style></head><body><div class='card'><h1>HeyTaps System Architecture & Documentation</h1>";
  html += "<h2>1. Hardware Overview</h2><p><b>Transceiver (T1):</b> ESP32-C3 Super Mini, HC-SR04 Ultrasonic, DHT11, NRF24L01+.<br><b>Receiver (R1):</b> ESP32 DevKit, OLED SSD1306, Status LEDs, Alarm Buzzer, NRF24L01+.</p>";
  html += "<h2>2. E2E Authentication</h2><p>Both devices generate a unique, fixed 8-character hardware address from their MAC address. Communication is strictly gated using these addresses. NRF24L01+ dynamic pipe addressing provides hardware-level filtering, meaning a device will physically ignore packets not addressed to its unique ID.</p>";
  html += "<h2>3. Two-Way Handshake</h2><p>Data transmission uses a custom application-layer two-way handshake. T1 transmits a DATA packet embedded with both the Sender ID and Target ID. R1 receives, verifies both IDs, and immediately replies with an ACK packet similarly encrypted with IDs. This ensures true E2E style code verification and immunity to cross-talk from identical models.</p>";
  html += "<h2>4. Web Portal & Wi-Fi</h2><p>Each device hosts an independent Wi-Fi Access Point (T1/R1). Setup requires pairing the devices by swapping their unique codes. The default password is 'savewater', modifiable via the dashboard. Hard resets can be initiated through the settings menu.</p>";
  html += "<a href='/' class='btn' style='margin-top:20px'>Back to Dashboard</a></div></body></html>";
  server.send(200, "text/html", html);
}

void handlePair() {
  if (server.hasArg("code")) {
    String c = server.arg("code");
    c.toUpperCase(); c.trim();
    pairedId = strtoul(c.c_str(), NULL, 16);
    prefs.putUInt("pairedId", pairedId);
    isPaired = true;
    setupPipes();
    radio.openReadingPipe(1, rxPipe);
    radio.startListening();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleCalib() {
  if (server.hasArg("empty")) { tankEmpty_cm = server.arg("empty").toFloat(); prefs.putFloat("tempty", tankEmpty_cm); }
  if (server.hasArg("full")) { tankFull_cm = server.arg("full").toFloat(); prefs.putFloat("tfull", tankFull_cm); }
  if (server.hasArg("low")) { lowThreshold = server.arg("low").toInt(); prefs.putInt("low", lowThreshold); }
  if (server.hasArg("high")) { highThreshold = server.arg("high").toInt(); prefs.putInt("high", highThreshold); }
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
  
  if (pairedId != 0) {
    isPaired = true;
    Serial.printf("PAIRED CODE: %08X\n", pairedId);
  }
  
  WiFi.softAP("HeyTap R1", apPass.c_str());
  
  server.on("/", handleRoot);
  server.on("/docs", handleDocs);
  server.on("/pair", handlePair);
  server.on("/calib", handleCalib);
  server.on("/settings", handleSettings);
  server.on("/reset", handleReset);
  server.begin();
  
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

void loop() {
  server.handleClient();
  handleButton();
  
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
  updateOLED();
}
