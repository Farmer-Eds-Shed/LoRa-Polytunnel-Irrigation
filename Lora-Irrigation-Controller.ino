#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include "heltec.h"
#include <Preferences.h>
#include "esp_system.h"

// --- Relay Pins ---
#define RELAY_ZONE_1 33
#define RELAY_ZONE_2 13
#define RELAY_ZONE_3 12
#define RELAY_ZONE_4 32
#define RELAY_PUMP   25  // Onboard LED

const int relayPins[4] = { RELAY_ZONE_1, RELAY_ZONE_2, RELAY_ZONE_3, RELAY_ZONE_4 };

AsyncWebServer server(80);
Preferences prefs;

unsigned long zoneTimers[4] = {0, 0, 0, 0};
bool zoneActive[4] = {false, false, false, false};
bool pumpActive = false;

const char* xorKey = "LoRaKey";

String xorDecrypt(const String& input) {
  String result = "";
  for (size_t i = 0; i < input.length(); i++) {
    result += (char)(input[i] ^ xorKey[i % strlen(xorKey)]);
  }
  return result;
}

void setupRelays();
void setupWiFi();
void setupWebUI();
void updateOLEDStatus();
void updatePumpState();

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("========== BOOTING ==========");

  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("Reset reason: %d (%s)\n", reason, 
    reason == ESP_RST_POWERON ? "Power-On" :
    reason == ESP_RST_EXT ? "External Reset" :
    reason == ESP_RST_SW ? "Software Reset" :
    reason == ESP_RST_PANIC ? "Panic" :
    reason == ESP_RST_BROWNOUT ? "Brownout" :
    reason == ESP_RST_SDIO ? "SDIO" :
    reason == ESP_RST_TASK_WDT ? "Task WDT" :
    reason == ESP_RST_WDT ? "WDT" : "Other");

  Heltec.begin(true, true, true, true, 868000000L);
  delay(100);
  Serial.println("Serial + OLED init done.");

  if (!LoRa.begin(868000000L, true)) {
    Serial.println("LoRa init failed!");
    Heltec.display->drawString(0, 12, "LoRa FAILED");
    Heltec.display->display();
    while (true);
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(0x12);
  Serial.println("LoRa Initial success!");

  LoRa.enableCrc();
  LoRa.setSyncWord(0x12);
  LoRa.receive();

  Heltec.display->clear();
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->drawString(0, 0, "Polytunnel Controller");
  Heltec.display->drawString(0, 12, "LoRa OK");
  Heltec.display->display();

  prefs.begin("boot", false);
  int boots = prefs.getInt("count", 0);
  prefs.putInt("count", boots + 1);
  Serial.printf("Boot count: %d\n", boots + 1);
  prefs.end();

  setupRelays();
  setupWiFi();
  setupWebUI();
  updatePumpState();
  updateOLEDStatus();
}

unsigned long lastPacketTime = 0;
unsigned long lastCheckTime = 0;

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    lastPacketTime = millis();
    String raw = "";
    while (LoRa.available()) {
      raw += (char)LoRa.read();
    }

    String message = xorDecrypt(raw);
    message.trim();

    Serial.print("Received LoRa: ");
    Serial.println(message);

    Serial.print("HEX: ");
    for (int i = 0; i < raw.length(); i++) {
      Serial.printf("%02X ", raw[i]);
    }
    Serial.println();

    if (message.startsWith("Z") && message.length() >= 4) {
      int zone = message.charAt(1) - '1';
      if (zone >= 0 && zone < 4) {
        bool turnOn = message.substring(2) == "ON";
        zoneActive[zone] = turnOn;
        digitalWrite(relayPins[zone], turnOn ? HIGH : LOW);
        updatePumpState();
        updateOLEDStatus();
        Serial.printf("✅ Zone %d %s (via LoRa)\n", zone + 1, turnOn ? "ON" : "OFF");
      }
    } else {
      Serial.println("⚠️ Unrecognized packet");
    }

    delay(10);
    LoRa.receive();
  }

  if (millis() - lastPacketTime > 10000 && millis() - lastCheckTime > 1000) {
    Serial.println("🔄 LoRa timeout — restarting radio");
    LoRa.end();
    LoRa.begin(868000000L, true);
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.setPreambleLength(8);
    LoRa.setSyncWord(0x12);
    LoRa.receive();
    lastPacketTime = millis();
    lastCheckTime = millis();
  }
}

void setupRelays() {
  for (int i = 0; i < 4; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }
  pinMode(RELAY_PUMP, OUTPUT);
  digitalWrite(RELAY_PUMP, LOW);
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin("WiFi", "password");

  unsigned long startAttempt = millis();
  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("WiFi failed to connect.");
  }
}

void setupWebUI() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head><title>Irrigation Controller</title></head><body>";
    html += "<h1>Polytunnel Irrigation Control</h1>";
    for (int i = 0; i < 4; i++) {
      html += "<p>Zone " + String(i+1) + ": ";
      html += "<a href=\"/zone?z=" + String(i) + "&s=1\"><button>ON</button></a> ";
      html += "<a href=\"/zone?z=" + String(i) + "&s=0\"><button>OFF</button></a></p>";
    }
    html += "<p>Pump: " + String(pumpActive ? "ON" : "OFF") + "</p>";
    html += "</body></html>";
    request->send(200, "text/html", html);
  });

  server.on("/zone", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("z") && request->hasParam("s")) {
      int zone = request->getParam("z")->value().toInt();
      int state = request->getParam("s")->value().toInt();
      if (zone >= 0 && zone < 4) {
        digitalWrite(relayPins[zone], state ? HIGH : LOW);
        zoneActive[zone] = state;
        updatePumpState();
        updateOLEDStatus();
      }
    }
    request->redirect("/");
  });

  server.begin();
}

void updatePumpState() {
  bool anyZoneOn = false;
  for (int i = 0; i < 4; i++) {
    if (zoneActive[i]) {
      anyZoneOn = true;
      break;
    }
  }
  digitalWrite(RELAY_PUMP, anyZoneOn ? HIGH : LOW);
  pumpActive = anyZoneOn;
}

void updateOLEDStatus() {
  Heltec.display->clear();
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->drawString(0, 0, "Polytunnel Controller");
  Heltec.display->drawString(0, 12, WiFi.isConnected() ? "WiFi OK" : "WiFi Failed");
  Heltec.display->drawString(0, 24, pumpActive ? "Pump: ON" : "Pump: OFF");

  String zoneLine = "Zone on:";
  for (int i = 0; i < 4; i++) {
    if (zoneActive[i]) {
      zoneLine += " " + String(i + 1);
    }
  }
  if (zoneLine == "Zone on:") zoneLine += " None";
  Heltec.display->drawString(0, 36, zoneLine);
  Heltec.display->display();
}
