#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include "heltec.h"

// WiFi Credentials
const char* ssid = "WiFi";
const char* password = "password";

// XOR Key
const char* xorKey = "LoRaKey";

// Globals
AsyncWebServer server(80);
String lastMessage = "(none)";
String lastStatus = "(none)";
unsigned long lastSend = 0;
unsigned long lastStatusPoll = 0;
unsigned long lastReceiveTime = 0;
unsigned long awaitingSince = 0;
bool awaitingReply = false;

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

void loop() {
  // Check if waiting too long for reply
  if (awaitingReply && millis() - awaitingSince > 1500) {
    Serial.println("⏱️ Timeout waiting for reply, clearing awaitingReply");
    awaitingReply = false;
    LoRa.receive();
  }

  if (millis() - lastStatusPoll >= 30000 && !awaitingReply) {
    sendLoRaMessage("STATUS");
    lastStatusPoll = millis();
  }

  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    String encrypted = "";
    while (LoRa.available()) {
      encrypted += (char)LoRa.read();
    }

    String reply = xorDecrypt(encrypted);
    Serial.println("📥 LoRa RX (decrypted): " + reply);
    lastStatus = reply;
    lastReceiveTime = millis();
    awaitingReply = false;
    LoRa.receive();
  }

  if (millis() - lastReceiveTime > 10000) {
    Serial.println("🔄 LoRa RX recovery");
    LoRa.end();
    delay(20);
    LoRa.begin(868000000L, true);
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.setPreambleLength(8);
    LoRa.setSyncWord(0x12);
    LoRa.enableCrc();
    LoRa.receive();
    lastReceiveTime = millis();
    awaitingReply = false;
  }

  // Ensure LoRa stays in receive mode every 2s
  static unsigned long lastReceiveCall = 0;
  if (millis() - lastReceiveCall > 2000) {
    LoRa.receive();
    lastReceiveCall = millis();
  }
}

void sendLoRaMessage(const String& msg) {
  String encrypted = xorEncrypt(msg);
  Serial.println("Preparing to send: " + msg);

  LoRa.idle();
  LoRa.beginPacket();
  LoRa.write((const uint8_t*)encrypted.c_str(), encrypted.length());
  bool success = LoRa.endPacket(true);  // Blocking send
  delay(10);
  LoRa.receive();

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
    LoRa.enableCrc();
    LoRa.receive();
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
  awaitingReply = true;
  awaitingSince = millis();
}

void setup() {
  Serial.begin(115200);
  delay(100);

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
  LoRa.enableCrc();
  LoRa.receive();

  Heltec.display->clear();
  Heltec.display->drawString(0, 0, "LoRa Gateway Ready");
  Heltec.display->display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
<html><head><title>LoRa Gateway</title>
<style>
  body { font-family: sans-serif; margin: 20px; }
  button { margin: 2px; padding: 5px 10px; }
  input[type=range] { width: 100px; }
  .zone-block { margin-bottom: 10px; }
</style></head>
<body>
<h1>LoRa Gateway</h1>
<p><b>Last Sent:</b> <span id="lastMsg">)rawliteral" + lastMessage + R"rawliteral(</span></p>
<p><b>Last Status:</b> <span id="lastStatus">)rawliteral" + lastStatus + R"rawliteral(</span></p>

<div class="zone-block">
<h3>Zone Control</h3>
)rawliteral";
    for (int i = 1; i <= 4; i++) {
      html += "<p>Zone " + String(i) + ": ";
      html += "<button onclick=\"sendCmd('Z" + String(i) + "ON')\">ON</button> ";
      html += "<button onclick=\"sendCmd('Z" + String(i) + "OFF')\">OFF</button> ";
      html += "<button onclick=\"sendCmd('Z" + String(i) + "C')\">Cancel</button> ";
      html += "Run: <input type='range' min='1' max='30' value='5' id='slider" + String(i) + "' oninput=\"this.nextElementSibling.value=this.value\"> <output>5</output> min ";
      html += "<button onclick=\"sendCmd('Z" + String(i) + ":' + document.getElementById('slider" + String(i) + "').value)\">Start</button></p>";
    }

    html += R"rawliteral(
</div>
<div class="zone-block">
<h3>Multi-Zone</h3>
<p>Zones (e.g. 123): <input id="multiZones" value="123">
Duration: <input type="number" id="multiDuration" min="1" max="30" value="5"> min
<button onclick="sendCmd('Z' + document.getElementById('multiZones').value + ':' + document.getElementById('multiDuration').value)">Send</button></p>
</div>

<div class="zone-block">
<h3>System</h3>
<button onclick="sendCmd('STATUS')">Get Status</button>
<button onclick="sendCmd('REBOOT')">Reboot</button>
</div>

<script>
function sendCmd(cmd) {
  fetch('/send?msg=' + encodeURIComponent(cmd)).then(r => location.reload());
}
</script>
</body></html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  server.on("/send", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("msg")) {
      String msg = request->getParam("msg")->value();
      if ((millis() - lastSend >= 500) && (!awaitingReply || millis() - awaitingSince > 1500)) {
        sendLoRaMessage(msg);
        if (msg == "STATUS") lastStatusPoll = millis();
      } else {
        Serial.println("🚩 Send ignored: too soon or awaiting reply");
      }
    }
    request->redirect("/");
  });

  server.begin();
}
