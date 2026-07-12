# ESP8266 Room Climate Sensor

A WiFi room climate sensor based on the ESP8266 (NodeMCU / Wemos D1 Mini) and
one or two DHT22 sensors, featuring an SSD1306 OLED display, a live web UI,
a JSON REST API and optional MQTT support with Home Assistant discovery.

![ESP8266 room climate sensor with DHT22 sensor and OLED display in 3D-printed enclosures](https://www.byteyourlife.com/wp-content/uploads/2026/07/ESP8266_room_climate_sensor_DHT22_sensor_OLED_display_01-1024x771.jpg)

**Features**

- Automatic detection of one or two connected DHT22 sensors
- OLED display showing device name, temperature, humidity and IP address
- Live web interface with dark mode, auto-refreshing every 3 seconds
- Per-sensor temperature offset calibration in 0.1 °C steps, stored
  persistently in EEPROM
- REST API (`/api`) with corrected and raw values, offset, RSSI and uptime
- Captive portal WiFi setup: the sensor opens its own hotspot, lets you scan
  for networks and stores the credentials. No hardcoded WiFi credentials or
  device names in the source code
- Device name configurable via the web UI (used as hostname and mDNS address,
  e.g. `http://livingroom.local`)
- Optional MQTT support with Home Assistant discovery: configure the broker
  via the web UI and the sensor automatically appears as a device in Home
  Assistant, including availability status (last will). Leave the broker
  field empty to disable MQTT completely
- Self-healing: automatically reconnects if WiFi or the MQTT broker become
  available again

This sensor is the first building block of a DIY smart floor heating system:
one sensor per room provides the actual temperatures for a central controller
that will drive the heating valve actuators (coming in a future project).

## Repository structure

- `esp8266_dht22_oled_offset_setup_mqtt.ino` is the current full version of
  the firmware (web UI, REST API, offset calibration, WiFi setup portal and
  MQTT with Home Assistant discovery). Use this file if you build the sensor
- The other `.ino` files are earlier development stages and are kept for
  reference
- `3d_print/` contains the STL files for the 3D-printed enclosure shown in
  the photo above

## Required libraries

Install via the Arduino Library Manager:

- DHT sensor library (Adafruit)
- Adafruit Unified Sensor
- Adafruit GFX Library
- Adafruit SSD1306
- PubSubClient (Nick O'Leary), only needed for MQTT

## Build guide

**Full build guide** (in German) with photos, wiring, 3D-printed enclosure and
setup instructions on my blog:

👉 https://www.byteyourlife.com/do-it-yourself/esp8266-raumklima-sensor-mit-dht22-fussbodenheizung-smart-machen/10413
