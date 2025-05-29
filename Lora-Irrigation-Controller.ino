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
#define RELAY_PUMP   25
#define WIFI_ENABLE_PIN 34

#define MAX_ZONES 4

const int relayPins[MAX_ZONES] = { RELAY_ZONE_1, RELAY_ZONE_2, RELAY_ZONE_3, RELAY_ZONE_4 };

AsyncWebServer server(80);
Preferences prefs;

unsigned long zoneStartTimes[MAX_ZONES] = {0};
unsigned int zoneDurations[MAX_ZONES] = {0};
bool zoneActive[MAX_ZONES] = {false};
bool pumpActive = false;
bool wifiActive = false;
unsigned long wifiStartTime = 0;

const char* xorKey = "LoRaKey";

String xorEncrypt(const String& input) {
  String result = "";
  for (size_t i = 0; i < input.length(); i++) {
    result += (char)(input[i] ^ xorKey[i % strlen(xorKey)]);
  }
  return result;
}

String xorDecrypt(const String& input) {
  return xorEncrypt(input);  // XOR is symmetrical
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
  LoRa.enableCrc();
  LoRa.receive();
  Serial.println("LoRa Initial success!");

  Heltec.display->clear();
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->drawString(0, 0, "Polytunnel Controller");
  Heltec.display->drawString(0, 12, "LoRa OK");
  Heltec.display->display();

  prefs.begin("timers", false);
  for (int i = 0; i < MAX_ZONES; i++) {
    zoneDurations[i] = prefs.getUInt(("zone" + String(i)).c_str(), 0);
  }
  prefs.end();

  pinMode(WIFI_ENABLE_PIN, INPUT);
  wifiActive = digitalRead(WIFI_ENABLE_PIN) == HIGH;

  setupRelays();
  if (wifiActive) {
    setupWiFi();
    setupWebUI();
    wifiStartTime = millis();
    Serial.println("✅ WiFi Enabled");
  } else {
    Serial.println("🛑 WiFi Disabled");
  }
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

    Serial.print("Raw LoRa: ");
    for (int i = 0; i < raw.length(); i++) {
      Serial.printf("%02X ", raw[i]);
    }
    Serial.println();

    String message = xorDecrypt(raw);
    message.trim();

    Serial.print("Received LoRa: ");
    Serial.println(message);

    if (message.startsWith("Z")) {
      String payload = message.substring(1);
      int colonIndex = payload.indexOf(':');
      int cancelIndex = payload.indexOf('C');

      if (colonIndex != -1) {
        String zoneList = payload.substring(0, colonIndex);
        int durationMin = payload.substring(colonIndex + 1).toInt();
        int durationSec = constrain(durationMin * 60, 0, 30 * 60);

        for (char c : zoneList) {
          int zone = c - '1';
          if (zone >= 0 && zone < MAX_ZONES) {
            digitalWrite(relayPins[zone], HIGH);
            zoneActive[zone] = true;
            zoneStartTimes[zone] = millis();
            zoneDurations[zone] = durationSec;
            Serial.printf("⏱️ Zone %d ON for %d min\n", zone + 1, durationMin);
          }
        }

      } else if (cancelIndex == 1 && payload.length() == 2) {
        int zone = payload.charAt(0) - '1';
        if (zone >= 0 && zone < MAX_ZONES) {
          digitalWrite(relayPins[zone], LOW);
          zoneActive[zone] = false;
          zoneDurations[zone] = 0;
          Serial.printf("❌ Zone %d cancelled\n", zone + 1);
        }

      } else if (payload.endsWith("ON") || payload.endsWith("OFF")) {
        int zone = payload.charAt(0) - '1';
        bool turnOn = payload.endsWith("ON");
        if (zone >= 0 && zone < MAX_ZONES) {
          digitalWrite(relayPins[zone], turnOn ? HIGH : LOW);
          zoneActive[zone] = turnOn;
          zoneDurations[zone] = 0;
          if (turnOn) zoneStartTimes[zone] = millis();
          Serial.printf("✅ Zone %d %s\n", zone + 1, turnOn ? "ON" : "OFF");
        }
      } else {
        Serial.println("⚠️ Unrecognized Z command");
      }

      updatePumpState();
      updateOLEDStatus();
    }
    else if (message == "REBOOT") {
      Serial.println("♻️ Rebooting...");
      delay(1000);
      ESP.restart();
    }
    else if (message == "STATUS") {
      String status = "Pump: ";
      status += pumpActive ? "ON; Zones: " : "OFF; Zones: ";
      for (int i = 0; i < MAX_ZONES; i++) {
        if (zoneActive[i]) status += String(i + 1);
      }
      if (status.endsWith(": ")) status += "None";

      String encryptedStatus = xorDecrypt(status);  // Symmetric XOR encryption
      LoRa.beginPacket();
      LoRa.write((const uint8_t*)encryptedStatus.c_str(), encryptedStatus.length());
      LoRa.endPacket();
      Serial.println("📡 Sent encrypted status via LoRa");
    }

    delay(10);
    LoRa.receive();
  }

  for (int i = 0; i < MAX_ZONES; i++) {
    if (zoneActive[i] && zoneDurations[i] > 0) {
      if (millis() - zoneStartTimes[i] >= zoneDurations[i] * 1000UL) {
        zoneActive[i] = false;
        digitalWrite(relayPins[i], LOW);
        updatePumpState();
        updateOLEDStatus();
        Serial.printf("⏱️ Zone %d auto OFF\n", i + 1);
      }
    }
  }

  if (millis() - lastPacketTime > 10000 && millis() - lastCheckTime > 1000) {
    Serial.println("🔄 Restarting LoRa RX");
    LoRa.end();
    LoRa.begin(868000000L, true);
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.setPreambleLength(8);
    LoRa.setSyncWord(0x12);
    LoRa.enableCrc();
    LoRa.receive();
    lastPacketTime = millis();
    lastCheckTime = millis();
  }

  if (wifiActive && millis() - wifiStartTime > 5 * 60 * 1000UL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiActive = false;
    Serial.println("⚡ WiFi disabled to save power.");
  }
}

void setupRelays() {
  for (int i = 0; i < MAX_ZONES; i++) {
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
    Serial.println(" WiFi connected");
  } else {
    Serial.println(" WiFi failed");
  }
}

void setupWebUI() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head><title>Irrigation Controller</title></head><body>";
    html += "<h1>Polytunnel Irrigation Control</h1>";
    html += "<p><b>Pump:</b> <span id='pstatus'>";
    html += pumpActive ? "<span style='color:green'>ON</span>" : "<span style='color:gray'>OFF</span>";
    html += "</span></p>";

    for (int i = 0; i < MAX_ZONES; i++) {
      html += "<form action=\"/zone\" method=\"get\">";
      html += "<p>Zone " + String(i + 1) + ": ";
      html += "<span id='zstatus" + String(i) + "'>";
      html += zoneActive[i] ? "<b style='color:green'>(Active)</b>" : "<span style='color:gray'>(Off)</span>";
      html += "</span><br>";
      html += "<input type=\"hidden\" name=\"z\" value=\"" + String(i) + "\">";
      html += "<input type=\"range\" name=\"d\" min=\"1\" max=\"30\" value=\"" + String(zoneDurations[i] / 60) + "\" oninput=\"this.nextElementSibling.value = this.value\">";
      html += "<output>" + String(zoneDurations[i] / 60) + "</output> min ";
      html += "<button name=\"s\" value=\"1\">ON</button> ";
      html += "<button name=\"s\" value=\"0\">OFF</button></p>";
      html += "</form>";
    }
    html += "</body></html>";
    html += R"rawliteral(
    <script>
    function updateStatus() {
      fetch('/status.json')
        .then(res => res.json())
        .then(data => {
          data.zones.forEach((active, i) => {
            let el = document.getElementById('zstatus' + i);
            if (el) el.innerHTML = active ? "<b style='color:green'>(Active)</b>" : "<span style='color:gray'>(Off)</span>";
          });
          let pump = document.getElementById('pstatus');
          if (pump) pump.innerHTML = data.pump ? "<span style='color:green'>ON</span>" : "<span style='color:gray'>OFF</span>";
        });
    }
    setInterval(updateStatus, 3000);
    window.onload = updateStatus;
    </script>
    )rawliteral";

    request->send(200, "text/html", html);
  });

  server.on("/zone", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("z") && request->hasParam("s")) {
      int zone = request->getParam("z")->value().toInt();
      int state = request->getParam("s")->value().toInt();
      int duration = request->hasParam("d") ? request->getParam("d")->value().toInt() : 0;

      if (zone >= 0 && zone < MAX_ZONES) {
        digitalWrite(relayPins[zone], state ? HIGH : LOW);
        zoneActive[zone] = state;

        if (state) {
          zoneStartTimes[zone] = millis();
          zoneDurations[zone] = constrain(duration * 60, 0, 30 * 60);
        } else {
          zoneDurations[zone] = 0;
        }

        prefs.begin("timers", false);
        prefs.putUInt(("zone" + String(zone)).c_str(), zoneDurations[zone]);
        prefs.end();

        updatePumpState();
        updateOLEDStatus();
      }
    }
    request->redirect("/");
  });

  server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"zones\":[";
    for (int i = 0; i < MAX_ZONES; i++) {
      json += zoneActive[i] ? "true" : "false";
      if (i < MAX_ZONES - 1) json += ",";
    }
    json += "],";
    json += "\"pump\":" + String(pumpActive ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });


  server.begin();
}

void updatePumpState() {
  bool anyZoneOn = false;
  for (int i = 0; i < MAX_ZONES; i++) {
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
  Heltec.display->drawString(0, 12, WiFi.isConnected() ? "WiFi OK" : "WiFi Off");
  Heltec.display->drawString(0, 24, pumpActive ? "Pump: ON" : "Pump: OFF");
  String zoneLine = "Zone on:";
  for (int i = 0; i < MAX_ZONES; i++) {
    if (zoneActive[i]) {
      zoneLine += " " + String(i + 1);
    }
  }
  if (zoneLine == "Zone on:") zoneLine += " None";
  Heltec.display->drawString(0, 36, zoneLine);
  Heltec.display->display();
}
