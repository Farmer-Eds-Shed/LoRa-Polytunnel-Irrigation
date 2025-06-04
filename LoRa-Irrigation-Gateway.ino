#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include "heltec.h"
#include <PubSubClient.h>

// WiFi Credentials
const char* ssid = "WiFi";
const char* password = "password";

// MQTT
const char* mqtt_server = "192.168.1.100";
const char* mqtt_user = "user";
const char* mqtt_pass = "password";
WiFiClient espClient;
PubSubClient client(espClient);

// XOR Key
const char* xorKey = "LoRaKey";

// Globals
AsyncWebServer server(80);
String lastMessage = "(none)";
String lastStatus = "(none)";
String lastStatusReply = "";
unsigned long lastSend = 0;
unsigned long lastStatusPoll = 0;
unsigned long lastReceiveTime = 0;
unsigned long lastCommandTime = 0;
unsigned long lastQueueEmptyTime = 0;
bool awaitingReply = false;
bool sentStatusSinceLastCommand = true;
bool statusUpdated = false;
bool wasQueueEmpty = true;

#define MAX_QUEUE 10
String messageQueue[MAX_QUEUE];
int queueStart = 0;
int queueEnd = 0;

bool queueIsEmpty() {
  return queueStart == queueEnd;
}

bool queueIsFull() {
  return ((queueEnd + 1) % MAX_QUEUE) == queueStart;
}

void enqueueMessage(const String& msg) {
  if (!queueIsFull()) {
    messageQueue[queueEnd] = msg;
    queueEnd = (queueEnd + 1) % MAX_QUEUE;
    sentStatusSinceLastCommand = false;
    lastCommandTime = millis();
  } else {
    Serial.println("⚠️ Queue full, ignoring message: " + msg);
  }
}

String dequeueMessage() {
  if (!queueIsEmpty()) {
    String msg = messageQueue[queueStart];
    queueStart = (queueStart + 1) % MAX_QUEUE;
    return msg;
  }
  return "";
}

String xorEncrypt(const String& input) {
  String result = "";
  for (size_t i = 0; i < input.length(); i++) {
    result += (char)(input[i] ^ xorKey[i % strlen(xorKey)]);
  }
  return result;
}

String xorDecrypt(const String& input) {
  return xorEncrypt(input);
}

void sendLoRaMessage(const String& msg) {
  String encrypted = xorEncrypt(msg);
  Serial.println("Preparing to send: " + msg);

  LoRa.idle();
  LoRa.beginPacket();
  LoRa.write((const uint8_t*)encrypted.c_str(), encrypted.length());
  bool success = LoRa.endPacket(true);
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
}

void callback(char* topic, byte* message, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)message[i];
  }
  Serial.println("MQTT RX: " + msg);
  enqueueMessage(msg);
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("LoRaGateway", mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      client.subscribe("irrigation/cmd");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  static unsigned long lastDequeue = 0;
  unsigned long now = millis();

  if (!queueIsEmpty() && now - lastDequeue > 300 && !awaitingReply) {
    String msg = dequeueMessage();
    sendLoRaMessage(msg);
    if (msg == "STATUS" || msg == "REBOOT") awaitingReply = true;
    if (msg == "STATUS") statusUpdated = false;
    lastDequeue = now;
  }

  if (queueIsEmpty()) {
    if (!wasQueueEmpty) {
      lastQueueEmptyTime = now;
      wasQueueEmpty = true;
    }
  } else {
    wasQueueEmpty = false;
  }

  bool allowImmediateStatus = (now - lastQueueEmptyTime > 4000) && !sentStatusSinceLastCommand && !awaitingReply;
  bool allowPeriodicStatus = (now - lastStatusPoll > 30000) && sentStatusSinceLastCommand && !awaitingReply;

  if (queueIsEmpty() && (allowImmediateStatus || allowPeriodicStatus)) {
    enqueueMessage("STATUS");
    lastStatusPoll = now;
  }

  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    String encrypted = "";
    while (LoRa.available()) {
      encrypted += (char)LoRa.read();
    }

    String decrypted = xorDecrypt(encrypted);
    Serial.println("📥 LoRa RX (decrypted): " + decrypted);
    lastReceiveTime = now;
    awaitingReply = false;

    if (decrypted.startsWith("SENSOR|")) {
      int secondPipe = decrypted.indexOf('|', 7);
      if (secondPipe != -1) {
        String nodeId = decrypted.substring(7, secondPipe);
        String jsonPayload = decrypted.substring(secondPipe + 1);
        String topic = "irrigation/sensors/" + nodeId;

        Serial.println("📡 Forwarding sensor data:");
        Serial.println(" → Topic: " + topic);
        Serial.println(" → Payload: " + jsonPayload);

        client.publish(topic.c_str(), jsonPayload.c_str());
      } else {
        Serial.println("⚠️ SENSOR| message malformed");
      }
    }
    else if (decrypted.indexOf("Zones:") != -1 || decrypted.indexOf("Pump:") != -1) {
      lastStatusReply = decrypted;
      lastStatus = decrypted;
      sentStatusSinceLastCommand = true;
      statusUpdated = true;

      client.publish("irrigation/status", decrypted.c_str());
    }
    else {
      Serial.println("ℹ️ Unknown message format");
    }

    LoRa.receive();
  }

  if (now - lastReceiveTime > 10000) {
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
    lastReceiveTime = now;
    awaitingReply = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Heltec.begin(true, true, true, true, 868000000L);
  delay(100);

  if (!LoRa.begin(868000000L, true)) {
    Serial.println("LoRa init failed!");
    while (true);
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();
  LoRa.receive();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  // Web UI routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
<html><head><title>LoRa Gateway</title><style>
body { font-family: sans-serif; margin: 20px; }
button { margin: 2px; padding: 5px 10px; }
input[type=range] { width: 100px; }
.zone-block { margin-bottom: 10px; }
.status { width: 10px; height: 10px; display: inline-block; margin-left: 5px; border-radius: 50%; }
</style></head><body>
<h1>LoRa Gateway</h1>
<p><b>Last Sent:</b> <span id="lastMsg">)rawliteral" + lastMessage + R"rawliteral(</span></p>
<p><b>Last Status:</b> <span id="lastStatus">)rawliteral" + lastStatus + R"rawliteral(</span></p>
<div class="zone-block">
<h3>Zone Control</h3>
)rawliteral";
    for (int i = 1; i <= 4; i++) {
      html += "<p>Zone " + String(i) + ": ";
      html += "<button onclick=\"sendCmd('Z" + String(i) + ":' + document.getElementById('slider" + String(i) + "').value)\">Start</button> ";
      html += "<button onclick=\"sendCmd('Z" + String(i) + "C')\">Cancel</button> ";
      html += "Run: <input type='range' min='1' max='30' value='5' id='slider" + String(i) + "' oninput=\"this.nextElementSibling.value=this.value\"> <output>5</output> min ";
      html += "<span class='status' id='zone" + String(i) + "'></span></p>";
    }
    html += R"rawliteral(
</div><script>
function sendCmd(cmd) {
  fetch('/send?msg=' + encodeURIComponent(cmd)).then(r => location.reload());
}
function pollStatus() {
  fetch('/status.json')
    .then(res => res.json())
    .then(data => {
      for (let i = 1; i <= 4; i++) {
        const el = document.getElementById('zone' + i);
        if (el) el.style.backgroundColor = data['zone' + i] ? 'green' : 'red';
      }
      document.getElementById('lastMsg').textContent = data.lastMsg;
      document.getElementById('lastStatus').textContent = data.lastStatus;
    });
}
setInterval(() => {
  fetch('/shouldUpdate').then(res => res.text()).then(flag => {
    if (flag === '1') pollStatus();
  });
}, 1000);
</script></body></html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  server.on("/send", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("msg")) {
      String msg = request->getParam("msg")->value();
      enqueueMessage(msg);
    }
    request->redirect("/");
  });

  server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    bool zoneStates[4] = {false, false, false, false};
    int zonesIdx = lastStatusReply.indexOf("Zones:");
    if (zonesIdx != -1) {
      String zonesPart = lastStatusReply.substring(zonesIdx + 6);
      zonesPart.trim();
      for (int i = 1; i <= 4; i++) {
        if (zonesPart.indexOf(String(i)) != -1) {
          zoneStates[i - 1] = true;
        }
      }
    }
    for (int i = 0; i < 4; i++) {
      json += "\"zone" + String(i + 1) + "\":" + (zoneStates[i] ? "true" : "false") + ",";
    }
    json += "\"lastMsg\":\"" + lastMessage + "\",";
    json += "\"lastStatus\":\"" + lastStatus + "\"";
    json += "}";
    statusUpdated = false;
    request->send(200, "application/json", json);
  });

  server.on("/shouldUpdate", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", statusUpdated ? "1" : "0");
  });

  server.begin();
}
