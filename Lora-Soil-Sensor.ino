#include "LoRaWan_APP.h"
#include "Arduino.h"

// === User Configuration ===
#define NODE_ID "soil1"
#define XOR_KEY "LoRaKey"
#define SLEEP_INTERVAL_MS (5 * 60 * 1000)  // 5 minutes

// === Sensor Pins (CubeCell specific) ===
#define SENSOR_POWER_PIN GPIO2
#define SOIL_ANALOG_PIN ADC
#define SOIL_DIGITAL_PIN GPIO3

// === LoRa Parameters ===
#define RF_FREQUENCY 868000000
#define TX_OUTPUT_POWER 14
#define LORA_BANDWIDTH 0
#define LORA_SPREADING_FACTOR 7
#define LORA_CODINGRATE 1
#define LORA_PREAMBLE_LENGTH 8
#define LORA_SYMBOL_TIMEOUT 0
#define LORA_FIX_LENGTH_PAYLOAD_ON false
#define LORA_IQ_INVERSION_ON false

// === LoRa Events and Sleep ===
static RadioEvents_t RadioEvents;
static TimerEvent_t wakeUpTimer;
bool lowpower = false;

// === XOR Encryption ===
String xorEncrypt(const String& input) {
  String result = "";
  for (size_t i = 0; i < input.length(); i++) {
    result += (char)(input[i] ^ XOR_KEY[i % strlen(XOR_KEY)]);
  }
  return result;
}

// === Radio Callbacks ===
void OnTxDone(void) {
  Serial.println("✅ TX done");
  Serial.printf("😴 Going into low power mode. Will wake up in %lu ms\n", SLEEP_INTERVAL_MS);
  TimerSetValue(&wakeUpTimer, SLEEP_INTERVAL_MS);
  TimerStart(&wakeUpTimer);
  lowpower = true;
}

void OnTxTimeout(void) {
  Serial.println("❌ TX timeout");
  lowpower = false;
}

void onWakeUp() {
  Serial.println("⏰ Wakeup triggered");
  lowpower = false;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("✅ Boot");

  // Init LoRa
  RadioEvents.TxDone = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;
  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                    LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                    LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
                    true, 0, 0, LORA_IQ_INVERSION_ON, 3000);
  Serial.println("✅ Radio Init OK");

  TimerInit(&wakeUpTimer, onWakeUp);

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  pinMode(SOIL_DIGITAL_PIN, INPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);  // Default OFF
}

void loop() {
  if (lowpower) {
    lowPowerHandler();  // Provided by LoRaWan_APP
    return;
  }

  // === Sensor Power ON ===
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  delay(100);  // Let sensor stabilize

  int analogVal = analogRead(SOIL_ANALOG_PIN);
  bool isWet = digitalRead(SOIL_DIGITAL_PIN) == LOW;

  // === Power OFF ===
  digitalWrite(SENSOR_POWER_PIN, LOW);

  // === Map & Constrain Moisture ===
  int moistPct = map(analogVal, 2900, 1800, 0, 100);
  moistPct = constrain(moistPct, 0, 100);

  Serial.printf("🌱 Raw analog: %d → %d%% | Wet: %s\n", analogVal, moistPct, isWet ? "true" : "false");

  // === Create Payload ===
  String json = "{\"moist\":" + String(moistPct) + ",\"wet\":" + String(isWet ? "true" : "false") + "}";

  String payload = "SENSOR|" NODE_ID "|" + json;
  String encrypted = xorEncrypt(payload);

  Serial.println("📡 Sending: " + payload);
  Serial.print("🔐 Encrypted HEX: ");
  for (size_t i = 0; i < encrypted.length(); i++) {
    Serial.printf("%02X ", encrypted[i]);
  }
  Serial.println();

  // === Send Encrypted Payload ===
  Radio.Send((uint8_t *)encrypted.c_str(), encrypted.length());
}
