# ESP8266 Room Climate Sensor

A WiFi room climate sensor based on the ESP8266 (NodeMCU / Wemos D1 Mini) and
one or two DHT22 sensors, featuring an SSD1306 OLED display, a live web UI,
and a JSON REST API.

![ESP8266 room climate sensor with DHT22 sensor and OLED display in 3D-printed enclosures](https://www.byteyourlife.com/wp-content/uploads/2026/07/ESP8266_room_climate_sensor_DHT22_sensor_OLED_display_01-1024x771.jpg)

**Features**

- Automatic detection of one or two connected DHT22 sensors
- OLED display showing device name, temperature, humidity and IP address
- Live web interface with dark mode, auto-refreshing every 3 seconds
- Per-sensor temperature offset calibration in 0.1 °C steps, stored
  persistently in EEPROM
- REST API (`/api`) with corrected and raw values, offset, RSSI and uptime
- Captive portal WiFi setup: the sensor opens its own hotspot, lets you scan
  for networks and stores the credentials – no hardcoded WiFi credentials or
  device names in the source code
- Device name configurable via the web UI (used as hostname and mDNS address,
  e.g. `http://livingroom.local`)
- Self-healing: automatically reconnects if WiFi becomes available again

This sensor is the first building block of a DIY smart floor heating system:
one sensor per room provides the actual temperatures for a central controller
that will drive the heating valve actuators (coming in a future project).

**Full build guide** (in German) with photos, wiring, 3D-printed enclosure and
setup instructions on my blog:

👉 https://www.byteyourlife.com/do-it-yourself/esp8266-raumklima-sensor-mit-dht22-fussbodenheizung-smart-machen/10413
