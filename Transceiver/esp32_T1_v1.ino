#include <SPI.h>
#include <RF24.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#define CE_PIN    4
#define CSN_PIN   5
#define SCK_PIN   18
#define MISO_PIN  19
#define MOSI_PIN  23
#define TRIG_PIN  12
#define ECHO_PIN  14
#define DHT_PIN   27
#define DHT_TYPE  DHT11

#define SCREEN_W    128
#define SCREEN_H     64
#define LED_RED     32
#define LED_YELLOW  33
#define LED_GREEN   25
#define LED_BLUE    26

const uint8_t RF_CHANNEL = 108;

struct DataPacket {
  uint32_t senderId;
  uint32_t targetId;
  float temperature;
  float humidity;
  uint16_t distance_mm;
  uint8_t msgType; // 0=DATA, 1=ACK
};

RF24 radio(CE_PIN, CSN_PIN);
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
WebServer server(80);
Preferences prefs;

uint32_t myId;
uint32_t pairedId = 0;
String apPass = "savewater";
bool isPaired = false;

uint8_t rxPipe[5];
uint8_t txPipe[5];

unsigned long txCount = 0;
unsigned long okCount = 0;
unsigned long failCount = 0;

DataPacket lastSentData;
uint8_t computedWater = 0;
bool ackReceivedRecently = false;
unsigned long lastAckMillis = 0;

uint8_t currentScreen = 0;
unsigned long lastScreenSwitch = 0;
const unsigned long SCREEN_DURATIONS[5] = {15000, 4000, 4000, 4000, 4000};

const char CSS[] PROGMEM = R"(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',Arial,sans-serif;background:#0d1117;color:#e0e0e0;padding:20px;max-width:600px;margin:auto}
h1{color:#00d4aa;font-size:1.5em;margin-bottom:2px}
.sub{color:#555;font-size:.8em;margin-bottom:20px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:12px;margin-bottom:16px}
.card{background:#161b22;border:1px solid #21262d;border-radius:10px;padding:14px;text-align:center}
.card .val{font-size:2em;font-weight:700;color:#00d4aa}
.card .lbl{font-size:.72em;color:#777;margin-top:4px}
.badge{display:inline-block;padding:4px 14px;border-radius:20px;font-size:.8em;margin-bottom:14px;font-weight:600}
.ok  {background:#0d2818;color:#00d4aa;border:1px solid #00d4aa}
.warn{background:#2d1212;color:#ff6b6b;border:1px solid #ff6b6b}
.panel{background:#161b22;border:1px solid #21262d;border-radius:12px;padding:16px;margin-bottom:12px}
.panel h2{color:#00aaff;font-size:.95em;margin-bottom:10px}
.row{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
.row label{font-size:.83em;color:#aaa}
.btn{display:block;width:100%;padding:9px;background:#00d4aa;color:#0d1117;border:none;border-radius:7px;font-weight:700;font-size:.9em;cursor:pointer;margin-top:4px}
.btn:hover{background:#00aaff}
.btn-danger{background:#ff6b6b}
.btn-danger:hover{background:#fa5252}
.code{font-family:monospace;font-size:1.2em;letter-spacing:2px;color:#00d4aa;background:#0d1117;padding:10px;border-radius:6px;text-align:center;display:block;margin-bottom:15px;border:1px solid #21262d}
.full-input{width:100% !important;margin-bottom:10px;box-sizing:border-box;background:#0d1117;border:1px solid #21262d;color:#fff;padding:8px 10px;border-radius:6px;font-size:.9em}
.footer{text-align:center;font-size:.72em;color:#555;margin-top:20px}
)";

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

void updateLEDs(uint8_t pct) {
  if (!ackReceivedRecently || (millis() - lastAckMillis > 30000UL)) {
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, LOW);
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_BLUE, LOW);
    return;
  }
  digitalWrite(LED_RED,    pct < 25                  ? HIGH : LOW);
  digitalWrite(LED_YELLOW, (pct >= 25 && pct < 50)   ? HIGH : LOW);
  digitalWrite(LED_GREEN,  (pct >= 50 && pct < 75)   ? HIGH : LOW);
  digitalWrite(LED_BLUE,   pct >= 75                 ? HIGH : LOW);
}

void handleRoot() {
  char myIdStr[9], pIdStr[9];
  sprintf(myIdStr, "%08X", myId);
  sprintf(pIdStr, "%08X", pairedId);
  
  String html = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  if(isPaired) html += "<meta http-equiv='refresh' content='10'>";
  html += "<title>HeyTaps – Transceiver</title><style>";
  html += CSS;
  html += "</style></head><body>";
  
  html += "<h1>HeyTaps Monitor</h1>";
  html += "<p class='sub'>Transceiver Dashboard &middot; auto-refresh 10s</p>";
  
  if (isPaired) {
    html += "<span class='badge ok'>Status: Paired & Active</span>";
    
    html += "<div class='grid'>";
    html += "<div class='card'><div class='val'>" + String(txCount) + "</div><div class='lbl'>Total TX Sent</div></div>";
    html += "<div class='card'><div class='val'>" + String(okCount) + "</div><div class='lbl'>ACKs Verified</div></div>";
    html += "</div>";
  } else {
    html += "<span class='badge warn'>Status: Not Paired</span>";
    html += "<p style='font-size:.8em;color:#aaa;margin-bottom:15px'>Please pair this device with the Receiver.</p>";
  }
  
  html += "<form action='/pair' method='POST'>";
  html += "<div class='panel'><h2>Pairing & Security</h2>";
  html += "<div class='row'><label>Your Device Code:</label><div class='code' style='margin-bottom:0;padding:6px'>" + String(myIdStr) + "</div></div>";
  if(isPaired) html += "<div class='row'><label>Paired Receiver:</label><div class='code' style='margin-bottom:0;padding:6px'>" + String(pIdStr) + "</div></div>";
  html += "<input type='text' name='code' maxlength='8' placeholder='Enter Receiver Code' class='full-input' style='margin-top:10px' required>";
  html += "<button class='btn' type='submit'>Save Pairing</button>";
  html += "</div></form>";
  
  html += "<form action='/settings' method='POST'>";
  html += "<div class='panel'><h2>System Settings</h2>";
  html += "<input type='text' name='pass' minlength='8' placeholder='New Wi-Fi Password' class='full-input' required>";
  html += "<button class='btn' type='submit'>Update Password</button>";
  html += "</div></form>";
  
  html += "<form action='/reset' method='POST' onsubmit='return confirm(\"Hard reset device? This removes pairings.\")'>";
  html += "<div class='panel'><button class='btn btn-danger' type='submit'>Hard Reset System</button></div></form>";
  
  html += "<p class='footer'>Connect to Wi-Fi <b>HeyTaps T1</b> &bull; Password: <b>" + apPass + "</b></p>";
  
  html += "</body></html>";
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
  
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED FAIL");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("HeyTaps T1 Booting..");
    display.display();
  }
  
  WiFi.mode(WIFI_AP);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  myId = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];

  char myIdStr[9];
  sprintf(myIdStr, "%08X", myId);
  Serial.printf("\n=== HeyTaps T1 (Transceiver) ===\n");
  Serial.printf("MY UNIQUE CODE: %s\n", myIdStr);
  
  prefs.begin("heytaps", false);
  pairedId = prefs.getUInt("pairedId", 0);
  apPass = prefs.getString("apPass", "savewater");
  if (pairedId != 0) {
    isPaired = true;
    Serial.printf("PAIRED CODE: %08X\n", pairedId);
  } else {
    Serial.println("STATUS: Not paired.");
  }
  
  WiFi.softAP("HeyTaps T1", apPass.c_str());
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  
  server.on("/", handleRoot);
  server.on("/pair", handlePair);
  server.on("/settings", handleSettings);
  server.on("/reset", handleReset);
  server.begin();
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  dht.begin();
  
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
  if (!radio.begin()) {
    Serial.println("NRF24 FAIL!");
    display.clearDisplay(); display.setCursor(0,0);
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

uint16_t readUltrasonic() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 38000UL);
  if (dur == 0) return 9999;
  return dur * 0.1715f;
}

void screenMain() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
  display.print("HeyTaps [1/5]");
  display.setTextSize(2); display.setCursor(0, 12); display.print(computedWater); display.print("%");
  display.setTextSize(1);
  display.setCursor(68, 12); display.print("D:"); display.print(lastSentData.distance_mm); display.println("mm");
  display.setCursor(68, 22); display.print("T:"); display.print(lastSentData.temperature, 1); display.println("C");
  display.setCursor(68, 32); display.print("H:"); display.print(lastSentData.humidity, 1); display.println("%");
  display.drawRect(0, 44, 126, 8, SSD1306_WHITE);
  uint8_t fill = (uint8_t)((computedWater / 100.0f) * 124);
  if (fill > 0) display.fillRect(1, 45, fill, 6, SSD1306_WHITE);
  display.setCursor(0, 56); display.setTextSize(1);
  display.print("Status: T1 Transmitting");
  display.display();
}

void screenTemp() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
  display.print("TEMPERATURE [2/5]");
  display.setTextSize(3); display.setCursor(8, 16); display.print(lastSentData.temperature, 1);
  display.setTextSize(2); display.setCursor(90, 20); display.print("C");
  display.setTextSize(1); display.setCursor(0, 56); display.print("DHT11 Sensor"); display.display();
}

void screenHumid() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0);
  display.print("HUMIDITY    [3/5]");
  display.setTextSize(3); display.setCursor(8, 16); display.print(lastSentData.humidity, 1);
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
  display.print(lastSentData.distance_mm); display.print("mm  ");
  display.display();
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
    display.println("HeyTaps T1");
    display.println("Pass: " + apPass);
    display.display();
    return;
  }
  
  if (!ackReceivedRecently || (millis() - lastAckMillis > 30000UL)) {
    display.clearDisplay(); display.setCursor(0,0); display.setTextSize(1);
    display.println("!! NO ACK !!");
    display.println("Receiver Offline?");
    if (ackReceivedRecently) {
      display.print("Last: "); display.print((millis()-lastAckMillis)/1000); display.println("s ago");
    }
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

unsigned long lastTx = 0;
unsigned long lastOledUpdate = 0;
void loop() {
  server.handleClient();
  
  if (isPaired && millis() - lastTx >= 2000) {
    lastTx = millis();
    txCount++;
    
    DataPacket pkt;
    pkt.senderId = myId;
    pkt.targetId = pairedId;
    pkt.temperature = dht.readTemperature();
    if(isnan(pkt.temperature)) pkt.temperature = 0;
    pkt.humidity = dht.readHumidity();
    if(isnan(pkt.humidity)) pkt.humidity = 0;
    pkt.distance_mm = readUltrasonic();
    pkt.msgType = 0;
    
    lastSentData = pkt; // Save for display
    
    radio.stopListening();
    radio.openWritingPipe(txPipe);
    radio.write(&pkt, sizeof(pkt));
    
    radio.openReadingPipe(1, rxPipe);
    radio.startListening();
    
    unsigned long st = millis();
    bool ackRcvd = false;
    while(millis() - st < 150) {
      if (radio.available()) {
        DataPacket ack;
        radio.read(&ack, sizeof(ack));
        if (ack.msgType == 1 && ack.targetId == myId && ack.senderId == pairedId) {
          ackRcvd = true;
          computedWater = ack.distance_mm; // We receive computed water level from R1 in this field!
          ackReceivedRecently = true;
          lastAckMillis = millis();
          break;
        }
      }
      delay(1);
    }
    
    if (ackRcvd) {
      okCount++;
      Serial.printf("[TX %lu] Dist:%u T:%.1f H:%.1f - ACK OK (Water: %u%%)\n", txCount, pkt.distance_mm, pkt.temperature, pkt.humidity, computedWater);
    } else {
      failCount++;
      Serial.printf("[TX %lu] Dist:%u T:%.1f H:%.1f - ACK FAIL\n", txCount, pkt.distance_mm, pkt.temperature, pkt.humidity);
    }
  }
  
  updateLEDs(computedWater);
  
  if (millis() - lastOledUpdate >= 200) {
    lastOledUpdate = millis();
    updateOLED();
  }
}
