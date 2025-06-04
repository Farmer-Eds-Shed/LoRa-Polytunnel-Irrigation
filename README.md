# 🌱 LoRa Irrigation Gateway

**A Wi-Fi-connected ESP32-based LoRa gateway for managing a polytunnel or field irrigation system with multiple zones and pump control.**  
Supports both a web-based control interface and MQTT integration for automation via platforms like Node-RED or Home Assistant.

---

## 🚀 Features

- 🛰️ LoRa message encryption (XOR-based) for secure command/control  
- 🌐 Web-based control panel (HTML/JS) for manual operation  
- 🔄 MQTT support for remote automation  
- ✅ Visual feedback of active zones & pump status  
- 📡 Robust queueing and auto-recovery of LoRa communication  
- 🔧 Configurable timer durations per zone  

---


## 🔧 Hardware

- **Heltec WiFi LoRa 32 (V2)**
- 868 MHz LoRa radio
- Relays for zones + pump (on the controller side)
- Internet-connected Wi-Fi
- **Heltec CubeCell**
- 868 MHz LoRa radio
- Soil Moisture Sensor

---

## 📦 MQTT Topics

| Topic               | Direction | Payload Example       | Description                          |
|--------------------|-----------|------------------------|--------------------------------------|
| `irrigation/cmd`   | ⬅️ Sub     | `Z1:10`, `Z1C`, `REBOOT`, `STATUS` | Control commands to be sent over LoRa |
| `irrigation/status`| ➡️ Pub     | `Pump: ON; Zones: 12` | Current zone/pump status from controller |
| `irrigation/sensor/soil`| ➡️ Pub     | `Moist: 12; Wet: 12` | Soil Moisture Sensor |

---

## ✅ Supported Commands

| Command     | Description                              |
|-------------|------------------------------------------|
| `Z1:10`     | Turn on Zone 1 for 10 minutes            |
| `Z2C`       | Cancel Zone 2                            |
| `Z123:5`    | Turn on Zones 1, 2, 3 for 5 minutes      |
| `REBOOT`    | Reboot the remote irrigation controller  |
| `STATUS`    | Request current status from controller   |

---

## 🌍 Web Interface

- Run timers per zone  
- Cancel individual zones  
- Status indicators (green/red) for each zone  
- Auto-status polling and UI updates  
- Works offline on local network  

---

## 📦 Dependencies

- [Heltec ESP32 LoRa Library V1.1.5](https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series)  
- [PubSubClient](https://github.com/knolleary/pubsubclient)  
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)  
- [AsyncTCP](https://github.com/me-no-dev/AsyncTCP)  

---

## 🔐 Configuration

Update your `ssid`, `password`, `mqtt_server`, `mqtt_user`, and `mqtt_pass` at the top of the sketch.

---

## 🧑‍🌾 Created By

[Farmer Ed](https://github.com/Farmer-Eds-Shed)  

