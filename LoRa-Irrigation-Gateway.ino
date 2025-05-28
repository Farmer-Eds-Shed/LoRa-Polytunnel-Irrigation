#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include "heltec.h"

// WiFi Credentials
const char* ssid = "WiFi";
const char* password = "Password";

// XOR Key
const char* xorKey = "LoRaKey";

// Globals
AsyncWebServer server(80);
String lastMessage = "(none)";

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

void setup() {
  Serial.begin(115200);
  delay(100);

  // OLED + LoRa Init
  Heltec.begin(true, true, true, true, 868000000L);
  delay(100);

  if (!LoRa.begin(868000000L, true)) {
    Serial.println("LoRa init failed!");
    Heltec.display->drawString(0, 0, "LoRa FAILED");
    Heltec.display->display();
    while (true);
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(0x12);

  Heltec.display->clear();
  Heltec.display->drawString(0, 0, "LoRa Gateway Ready");
  Heltec.display->display();

  // WiFi Init
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  // Web Interface
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head><title>LoRa Gateway</title></head><body>";
    html += "<h1>LoRa Gateway</h1>";
    html += "<p>Last Message: " + lastMessage + "</p>";
    for (int i = 1; i <= 4; i++) {
      html += "<p>Zone " + String(i) + ": ";
      html += "<a href=\"/send?msg=Z" + String(i) + "ON\"><button>ON</button></a> ";
      html += "<a href=\"/send?msg=Z" + String(i) + "OFF\"><button>OFF</button></a></p>";
    }
    html += "</body></html>";
    request->send(200, "text/html", html);
  });

  server.on("/send", HTTP_GET, [](AsyncWebServerRequest *request){
    static unsigned long lastSend = 0;
    if (millis() - lastSend < 500) {
      Serial.println("🛑 Send ignored: too soon");
      request->redirect("/");
      return;
    }

    if (request->hasParam("msg")) {
      String msg = request->getParam("msg")->value();
      String encrypted = xorEncrypt(msg);

      Serial.println("Preparing to send: " + msg);
      LoRa.idle();
      delay(5);
      LoRa.beginPacket();
      LoRa.write((const uint8_t*)encrypted.c_str(), encrypted.length());
      bool success = LoRa.endPacket(true);

      if (!success) {
        Serial.println("❌ LoRa TX failed — resetting radio");
        LoRa.end();
        delay(50);
        LoRa.begin(868000000L, true);
        LoRa.setSpreadingFactor(7);
        LoRa.setSignalBandwidth(125E3);
        LoRa.setCodingRate4(5);
        LoRa.setPreambleLength(8);
        LoRa.setSyncWord(0x12);
      } else {
        Serial.print("Sending HEX: ");
        for (int i = 0; i < encrypted.length(); i++) {
          Serial.printf("%02X ", encrypted[i]);
        }
        Serial.println();
        Serial.println("✅ Sent: " + msg);
      }

      lastMessage = "Sent: " + msg;
      lastSend = millis();
    }
    request->redirect("/");
  });

  server.begin();
}

void loop() {
  // Nothing to do here — web requests trigger transmissions
}
