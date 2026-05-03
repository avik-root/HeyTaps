#include <SPI.h>
#include <RF24.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#define CE_PIN    3
#define CSN_PIN   10
#define SCK_PIN   4
#define MISO_PIN  5
#define MOSI_PIN  6
#define TRIG_PIN  1
#define ECHO_PIN  2
#define DHT_PIN   0
#define DHT_TYPE  DHT11

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

void handleRoot() {
  char myIdStr[9], pIdStr[9];
  sprintf(myIdStr, "%08X", myId);
  sprintf(pIdStr, "%08X", pairedId);
  
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  if(isPaired) html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>T1 Dashboard</title><style>";
  html += CSS;
  html += "</style></head><body><div class='card'><h1>HeyTaps Transceiver (T1)</h1>";
  
  if (isPaired) {
    html += "<p><span class='badge'>Status: Paired & Active</span></p>";
    html += "<label>Your Device Code</label><span class='code'>" + String(myIdStr) + "</span>";
    html += "<label>Paired Receiver Code</label><span class='code'>" + String(pIdStr) + "</span>";
    html += "<p>Transmissions: " + String(txCount) + " | ACKs OK: " + String(okCount) + "</p>";
  } else {
    html += "<p><span class='badge err'>Status: Not Paired</span></p>";
    html += "<label>Your Device Code</label><span class='code'>" + String(myIdStr) + "</span>";
    html += "<p>Enter this code on the Receiver's setup page, and enter the Receiver's code below.</p>";
  }
  
  html += "<h2>Pair Device</h2><form action='/pair' method='POST'>";
  html += "<label>Enter Receiver Code</label><input type='text' name='code' maxlength='8' placeholder='e.g. A1B2C3D4' required>";
  html += "<button type='submit' class='btn'>Save Pairing</button></form></div>";
  
  html += "<div class='card'><h2>Settings</h2><form action='/settings' method='POST'>";
  html += "<label>Change Wi-Fi Password</label><input type='text' name='pass' minlength='8' placeholder='New Password' required>";
  html += "<button type='submit' class='btn'>Update Password</button></form>";
  html += "<form action='/reset' method='POST' onsubmit='return confirm(\"Hard reset device?\")'><button type='submit' class='btn btn-danger'>Hard Reset</button></form></div>";
  
  html += "</body></html>";
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

unsigned long lastTx = 0;
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
          break;
        }
      }
      delay(1);
    }
    
    if (ackRcvd) {
      okCount++;
      Serial.printf("[TX %lu] Dist:%u T:%.1f H:%.1f - ACK OK\n", txCount, pkt.distance_mm, pkt.temperature, pkt.humidity);
    } else {
      failCount++;
      Serial.printf("[TX %lu] Dist:%u T:%.1f H:%.1f - ACK FAIL\n", txCount, pkt.distance_mm, pkt.temperature, pkt.humidity);
    }
  }
}
