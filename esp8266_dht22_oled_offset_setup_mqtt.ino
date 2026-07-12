/*
 * ESP8266 – 1-2x DHT22 + OLED + REST-API + Offset + WLAN-Setup + MQTT/HA
 * ----------------------------------------------------------------------
 * NEU in dieser Version:
 *  - Optionale MQTT-Anbindung mit Home Assistant Discovery:
 *    Broker, Port, Benutzer und Passwort werden ueber die Setup-Seite
 *    konfiguriert und persistent im EEPROM gespeichert.
 *    Broker-Feld leer = MQTT komplett deaktiviert (alles laeuft wie bisher).
 *  - Der Sensor erscheint in Home Assistant automatisch als Geraet mit
 *    Entitaeten fuer Temperatur, Luftfeuchtigkeit (je Sensor) und RSSI.
 *  - Availability via Last Will: HA zeigt "nicht verfuegbar", wenn der
 *    Sensor ausfaellt.
 *  - Web-Oberflaeche, REST-API, Offset-Kalibrierung und WLAN-Setup
 *    bleiben vollstaendig unveraendert erhalten.
 *
 * Endpunkte:
 *    http://<IP>/            -> Live-Webseite (Sensorwerte + Offsets)
 *    http://<IP>/setup       -> Einstellungen (Name, WLAN, MQTT)
 *    http://<IP>/api         -> JSON fuer eigene Anwendungen
 *    http://<IP>/api/offset  -> Offset setzen (sensor=1|2, delta|set)
 *    http://<IP>/api/scan    -> WLAN-Scan als JSON
 *    http://<IP>/api/config  -> Name/WLAN/MQTT speichern (POST) -> Neustart
 *    http://<name>.local     -> per mDNS
 *
 * Benoetigte Bibliotheken (Library Manager):
 *   - "DHT sensor library" (Adafruit)
 *   - "Adafruit Unified Sensor"
 *   - "Adafruit GFX Library"
 *   - "Adafruit SSD1306"
 *   - "PubSubClient" (Nick O'Leary)          <- NEU fuer MQTT
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <PubSubClient.h>

// ---------------- Setup-Hotspot ----------------
// Leer = offener Hotspot. Wenn gesetzt, mindestens 8 Zeichen!
const char* AP_PASSWORD  = "";
const char* DEFAULT_NAME = "Klima-Sensor";
const char* SW_VERSION   = "1.1";

// ---------------- MQTT ----------------
const char* HA_DISCOVERY_PREFIX = "homeassistant";   // HA-Standard
const unsigned long MQTT_PUBLISH_INTERVAL   = 30000; // Werte alle 30 s
const unsigned long MQTT_RECONNECT_INTERVAL = 15000; // Reconnect alle 15 s

// ---------------- DHT22 ----------------
#define DHTTYPE DHT22
#define DHTPIN1 14   // D5  -> DHT22 Sensor 1
#define DHTPIN2 12   // D6  -> DHT22 Sensor 2
DHT dht1(DHTPIN1, DHTTYPE);
DHT dht2(DHTPIN2, DHTTYPE);

// ---------------- OLED SSD1306 ----------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64     // bei 128x32-Display hier auf 32 aendern
#define OLED_RESET    -1
#define OLED_ADDR     0x3C   // ggf. 0x3D
#define I2C_SDA       4      // D2
#define I2C_SCL       5      // D1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- Webserver / DNS / MQTT ----------------
ESP8266WebServer server(80);
DNSServer dnsServer;
WiFiClient espClient;
PubSubClient mqtt(espClient);
bool apMode = false;
String apSsid;
String chipIdStr;          // eindeutige ID aus der Chip-ID (hex)
unsigned long lastMqttPublish = 0;
unsigned long lastMqttAttempt = 0;
bool discoveryPublished = false;

// ---------------- Persistente Konfiguration ----------------
// Version 3: Name + WLAN + Offsets + MQTT.
// Offsets in ZEHNTELGRAD (int16). 3 == +0.3 C
struct PersistData {
  uint32_t magic;
  char     name[32];      // Geraetename
  char     ssid[33];      // WLAN-SSID
  char     pass[64];      // WLAN-Passwort
  int16_t  offT1;         // Offset Sensor 1 in 0.1 C
  int16_t  offT2;         // Offset Sensor 2 in 0.1 C
  char     mqttHost[41];  // MQTT-Broker (leer = MQTT deaktiviert)
  uint16_t mqttPort;      // MQTT-Port (0 -> 1883)
  char     mqttUser[33];  // MQTT-Benutzer (optional)
  char     mqttPass[41];  // MQTT-Passwort (optional)
};
// Alte Strukturen fuer die Migration:
struct PersistDataV2 {
  uint32_t magic;
  char     name[32];
  char     ssid[33];
  char     pass[64];
  int16_t  offT1;
  int16_t  offT2;
};
struct PersistDataV1 {
  uint32_t magic;
  int16_t  offT1;
  int16_t  offT2;
};
const uint32_t EE_MAGIC_V1 = 0x44485432;   // "DHT2"
const uint32_t EE_MAGIC_V2 = 0x44485433;   // "DHT3"
const uint32_t EE_MAGIC_V3 = 0x44485434;   // "DHT4" (aktuell)
const int16_t  OFFSET_MIN  = -100;         // -10.0 C
const int16_t  OFFSET_MAX  =  100;         // +10.0 C

PersistData cfg;

void saveConfig() {
  cfg.magic = EE_MAGIC_V3;
  EEPROM.put(0, cfg);
  EEPROM.commit();   // beim ESP8266 zwingend noetig!
  Serial.println("Konfiguration gespeichert.");
}

void loadConfig() {
  EEPROM.begin(512);
  EEPROM.get(0, cfg);

  if (cfg.magic == EE_MAGIC_V3) {
    cfg.name[sizeof(cfg.name) - 1]         = '\0';
    cfg.ssid[sizeof(cfg.ssid) - 1]         = '\0';
    cfg.pass[sizeof(cfg.pass) - 1]         = '\0';
    cfg.mqttHost[sizeof(cfg.mqttHost) - 1] = '\0';
    cfg.mqttUser[sizeof(cfg.mqttUser) - 1] = '\0';
    cfg.mqttPass[sizeof(cfg.mqttPass) - 1] = '\0';
    cfg.offT1 = constrain(cfg.offT1, OFFSET_MIN, OFFSET_MAX);
    cfg.offT2 = constrain(cfg.offT2, OFFSET_MIN, OFFSET_MAX);
    Serial.printf("Konfig geladen: Name='%s' SSID='%s' MQTT='%s' Offsets S1=%+.1f S2=%+.1f\n",
                  cfg.name, cfg.ssid, cfg.mqttHost,
                  cfg.offT1 / 10.0f, cfg.offT2 / 10.0f);
    return;
  }

  if (cfg.magic == EE_MAGIC_V2) {
    // Migration V2 -> V3: alles uebernehmen, MQTT-Felder leer
    PersistDataV2 old;
    EEPROM.get(0, old);
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, old.name, sizeof(cfg.name));
    memcpy(cfg.ssid, old.ssid, sizeof(cfg.ssid));
    memcpy(cfg.pass, old.pass, sizeof(cfg.pass));
    cfg.name[sizeof(cfg.name) - 1] = '\0';
    cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    cfg.pass[sizeof(cfg.pass) - 1] = '\0';
    cfg.offT1 = constrain(old.offT1, OFFSET_MIN, OFFSET_MAX);
    cfg.offT2 = constrain(old.offT2, OFFSET_MIN, OFFSET_MAX);
    saveConfig();
    Serial.println("EEPROM von V2 migriert (Name/WLAN/Offsets uebernommen).");
    return;
  }

  if (cfg.magic == EE_MAGIC_V1) {
    // Migration V1 -> V3: nur Offsets vorhanden
    PersistDataV1 old;
    EEPROM.get(0, old);
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.name, DEFAULT_NAME, sizeof(cfg.name));
    cfg.offT1 = constrain(old.offT1, OFFSET_MIN, OFFSET_MAX);
    cfg.offT2 = constrain(old.offT2, OFFSET_MIN, OFFSET_MAX);
    saveConfig();
    Serial.println("EEPROM von V1 migriert (Offsets uebernommen).");
    return;
  }

  // Erststart: Defaults
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.name, DEFAULT_NAME, sizeof(cfg.name));
  saveConfig();
  Serial.println("EEPROM initialisiert (Defaults).");
}

bool mqttConfigured() { return strlen(cfg.mqttHost) > 0; }
uint16_t getMqttPort() { return cfg.mqttPort ? cfg.mqttPort : 1883; }

// ---------------- Messwerte ----------------
struct Reading {
  float temperature = NAN;   // korrigierter Wert (roh + Offset)
  float rawTemp     = NAN;   // Rohwert vom Sensor
  float humidity    = NAN;
  bool  valid       = false; // letzte Messung erfolgreich?
  bool  present     = false; // Sensor ueberhaupt angeschlossen?
};
Reading r1, r2;

// ---------------- Explizite Prototypen ----------------
// Noetig, weil die Arduino IDE ihre Auto-Prototypen VOR der
// Struct-Definition einfuegt -> "Reading has not been declared".
void readOne(DHT& dht, Reading& r, int16_t offsetTenths);
void appendSensor(char* buf, size_t n, const char* key,
                  const Reading& r, int16_t offsetTenths);

String hostName;  // aus cfg.name erzeugter, DNS-tauglicher Name

unsigned long lastMeasure = 0;
const unsigned long MEASURE_INTERVAL   = 5000;                // alle 5 s messen
const unsigned long AP_RETRY_INTERVAL  = 5UL * 60UL * 1000UL; // 5 min
unsigned long apStarted = 0;

// ---------------- Hilfsfunktion: Name -> Hostname ----------------
String slugify(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); ) {
    uint8_t c = (uint8_t)in[i];
    if (c == 0xC3 && i + 1 < in.length()) {        // UTF-8 Umlaut-Praefix
      uint8_t c2 = (uint8_t)in[i + 1];
      switch (c2) {
        case 0xA4: case 0x84: out += "ae"; break;  // ä Ä
        case 0xB6: case 0x96: out += "oe"; break;  // ö Ö
        case 0xBC: case 0x9C: out += "ue"; break;  // ü Ü
        case 0x9F:            out += "ss"; break;  // ß
        default: break;
      }
      i += 2; continue;
    }
    if      (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += (char)c;
    else if (c == ' ' || c == '_' || c == '-') out += '-';
    i++;
  }
  while (out.length() && out[0] == '-') out.remove(0, 1);
  while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1);
  if (out.length() == 0) out = "esp-sensor";
  return out;
}

// ---------------- Hilfsfunktion: String fuer JSON escapen ----------------
String jsonEscape(const char* s) {
  String out;
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
    else if ((uint8_t)*p < 0x20) { out += ' '; }
    else out += *p;
  }
  return out;
}

// ---------------- OLED: einfache Textseite ----------------
void showBootScreen(const char* line1, const char* line2, const char* line3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.println(cfg.name);
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);
  display.setCursor(0, 18); display.println(line1);
  display.setCursor(0, 32); display.println(line2);
  display.setCursor(0, 46); display.println(line3);
  display.display();
}

// ---------------- WLAN: Verbindung versuchen ----------------
bool connectSTA() {
  if (strlen(cfg.ssid) == 0) return false;

  Serial.printf("Verbinde mit '%s'", cfg.ssid);
  showBootScreen("Verbinde mit WLAN:", cfg.ssid, "");

  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostName.c_str());
  WiFi.begin(cfg.ssid, cfg.pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WLAN verbunden - Name: "); Serial.println(hostName);
    Serial.print("WLAN verbunden - IP:   "); Serial.println(WiFi.localIP());
    Serial.print("WLAN verbunden - MAC:  "); Serial.println(WiFi.macAddress());
    return true;
  }
  Serial.println("WLAN-Verbindung fehlgeschlagen.");
  return false;
}

// ---------------- WLAN: Setup-Hotspot starten ----------------
void startAP() {
  apMode = true;
  apStarted = millis();
  apSsid = "Klima-Setup-" + chipIdStr;

  // AP_STA, damit WLAN-Scan auch im Hotspot-Modus funktioniert
  WiFi.mode(WIFI_AP_STA);
  if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  else                          WiFi.softAP(apSsid.c_str());

  // Captive Portal: alle DNS-Anfragen auf uns umleiten
  dnsServer.start(53, "*", WiFi.softAPIP());

  Serial.printf("Setup-Hotspot aktiv: %s  ->  http://%s/\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  showBootScreen("Setup-Modus! WLAN:", apSsid.c_str(), "http://192.168.4.1");
}

// ---------------- einen Sensor lesen ----------------
void readOne(DHT& dht, Reading& r, int16_t offsetTenths) {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    r.rawTemp     = t;
    r.temperature = t + offsetTenths / 10.0f;   // Offset anwenden
    r.humidity    = h;
    r.valid       = true;
    r.present     = true;          // einmal gelesen = vorhanden
  } else {
    r.valid = false;               // present bleibt erhalten
  }
}

void readSensors() {
  readOne(dht1, r1, cfg.offT1);
  readOne(dht2, r2, cfg.offT2);
  Serial.printf("[%s]  S1 present=%d %.1fC/%.0f%% (off %+.1f)   S2 present=%d %.1fC/%.0f%% (off %+.1f)\n",
                cfg.name,
                r1.present, r1.temperature, r1.humidity, cfg.offT1 / 10.0f,
                r2.present, r2.temperature, r2.humidity, cfg.offT2 / 10.0f);
}

// ---------------- Sensoren beim Start erkennen ----------------
void detectSensors() {
  for (int i = 0; i < 3 && !(r1.present && r2.present); i++) {
    readOne(dht1, r1, cfg.offT1);
    readOne(dht2, r2, cfg.offT2);
    if (r1.present && r2.present) break;
    delay(2000);
  }
  int n = (r1.present ? 1 : 0) + (r2.present ? 1 : 0);
  Serial.printf("Erkannte Sensoren: %d\n", n);
}

// ---------------- OLED aktualisieren ----------------
void updateDisplay() {
  if (apMode) {
    showBootScreen("Setup-Modus! WLAN:", apSsid.c_str(), "http://192.168.4.1");
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(cfg.name);
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  int count = (r1.present ? 1 : 0) + (r2.present ? 1 : 0);

  if (count == 0) {
    display.setCursor(0, 26);
    display.print("Kein Sensor gefunden");
  } else if (count == 1) {
    Reading& r = r1.present ? r1 : r2;
    display.setTextSize(2);
    display.setCursor(0, 18);
    if (r.valid) display.printf("%.1f C", r.temperature);
    else         display.print("--.- C");
    display.setTextSize(1);
    display.setCursor(0, 42);
    if (r.valid) display.printf("Luftf.: %.0f %%", r.humidity);
    else         display.print("Luftf.: -- %");
  } else {
    display.setTextSize(1);
    display.setCursor(0, 16);
    if (r1.valid) display.printf("S1: %.1fC  %.0f%%", r1.temperature, r1.humidity);
    else          display.print("S1: ---");
    display.setCursor(0, 28);
    if (r2.valid) display.printf("S2: %.1fC  %.0f%%", r2.temperature, r2.humidity);
    else          display.print("S2: ---");
  }

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print(WiFi.localIP().toString());
  display.display();
}

// ================================================================
//                     MQTT / Home Assistant
// ================================================================

String mqttBaseTopic()  { return "klima/" + chipIdStr; }          // z.B. klima/a1b2c3
String mqttStateTopic() { return mqttBaseTopic() + "/state"; }
String mqttAvailTopic() { return mqttBaseTopic() + "/status"; }

// Gemeinsamer "device"-Block, damit HA alle Entitaeten gruppiert
String haDeviceJson() {
  String d = "\"dev\":{\"ids\":[\"klima_" + chipIdStr + "\"],";
  d += "\"name\":\"" + jsonEscape(cfg.name) + "\",";
  d += "\"mf\":\"DIY\",\"mdl\":\"ESP8266 Klima-Sensor\",";
  d += "\"sw\":\"" + String(SW_VERSION) + "\",";
  d += "\"cu\":\"http://" + WiFi.localIP().toString() + "/\"}";
  return d;
}

// Eine Discovery-Config fuer eine Entitaet publizieren (retained)
void publishOneDiscovery(const char* objId, const char* entName,
                         const char* devClass, const char* unit,
                         const char* valueKey, bool diagnostic) {
  String topic = String(HA_DISCOVERY_PREFIX) + "/sensor/klima_" + chipIdStr +
                 "_" + objId + "/config";

  String p = "{";
  p += "\"name\":\"" + String(entName) + "\",";
  p += "\"uniq_id\":\"klima_" + chipIdStr + "_" + objId + "\",";
  p += "\"stat_t\":\"" + mqttStateTopic() + "\",";
  p += "\"avty_t\":\"" + mqttAvailTopic() + "\",";
  p += "\"dev_cla\":\"" + String(devClass) + "\",";
  p += "\"unit_of_meas\":\"" + String(unit) + "\",";
  p += "\"stat_cla\":\"measurement\",";
  p += "\"val_tpl\":\"{{ value_json." + String(valueKey) + " }}\",";
  if (diagnostic) p += "\"ent_cat\":\"diagnostic\",";
  p += haDeviceJson();
  p += "}";

  bool ok = mqtt.publish(topic.c_str(), p.c_str(), true);   // retained!
  Serial.printf("HA-Discovery %s: %s\n", objId, ok ? "ok" : "FEHLER");
}

void publishDiscovery() {
  bool two = r1.present && r2.present;
  if (r1.present) {
    publishOneDiscovery("t1", two ? "Temperatur 1" : "Temperatur",
                        "temperature", "\u00b0C", "t1", false);
    publishOneDiscovery("h1", two ? "Luftfeuchtigkeit 1" : "Luftfeuchtigkeit",
                        "humidity", "%", "h1", false);
  }
  if (r2.present) {
    publishOneDiscovery("t2", two ? "Temperatur 2" : "Temperatur",
                        "temperature", "\u00b0C", "t2", false);
    publishOneDiscovery("h2", two ? "Luftfeuchtigkeit 2" : "Luftfeuchtigkeit",
                        "humidity", "%", "h2", false);
  }
  publishOneDiscovery("rssi", "WLAN-Signal", "signal_strength", "dBm",
                      "rssi", true);
  discoveryPublished = true;
}

// Aktuelle Messwerte publizieren
void publishState() {
  char t1[12], h1[12], t2[12], h2[12];
  dtostrf(r1.temperature, 0, 1, t1);
  dtostrf(r1.humidity,    0, 1, h1);
  dtostrf(r2.temperature, 0, 1, t2);
  dtostrf(r2.humidity,    0, 1, h2);

  String json = "{";
  if (r1.present && r1.valid) {
    json += "\"t1\":" + String(t1) + ",\"h1\":" + String(h1) + ",";
  }
  if (r2.present && r2.valid) {
    json += "\"t2\":" + String(t2) + ",\"h2\":" + String(h2) + ",";
  }
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime_s\":" + String(millis() / 1000UL);
  json += "}";

  mqtt.publish(mqttStateTopic().c_str(), json.c_str(), false);
}

// Verbindung zum Broker aufbauen (nicht-blockierend im Loop aufgerufen)
void mqttEnsureConnected() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttAttempt < MQTT_RECONNECT_INTERVAL &&
      lastMqttAttempt != 0) return;
  lastMqttAttempt = millis();

  String clientId = "klima-" + chipIdStr;
  Serial.printf("MQTT: verbinde mit %s:%u ... ", cfg.mqttHost, getMqttPort());

  // Last Will: bei Verbindungsabbruch setzt der Broker "offline" (retained)
  bool ok;
  if (strlen(cfg.mqttUser) > 0) {
    ok = mqtt.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPass,
                      mqttAvailTopic().c_str(), 0, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(),
                      mqttAvailTopic().c_str(), 0, true, "offline");
  }

  if (ok) {
    Serial.println("verbunden.");
    mqtt.publish(mqttAvailTopic().c_str(), "online", true);   // retained
    publishDiscovery();
    publishState();
    lastMqttPublish = millis();
  } else {
    Serial.printf("fehlgeschlagen (rc=%d), naechster Versuch in %lus.\n",
                  mqtt.state(), MQTT_RECONNECT_INTERVAL / 1000UL);
  }
}

void mqttSetup() {
  if (!mqttConfigured()) {
    Serial.println("MQTT: kein Broker konfiguriert -> deaktiviert.");
    return;
  }
  mqtt.setServer(cfg.mqttHost, getMqttPort());
  // Discovery-Payloads sind groesser als der PubSubClient-Default (256 B)
  mqtt.setBufferSize(768);
  Serial.printf("MQTT: konfiguriert fuer %s:%u\n", cfg.mqttHost, getMqttPort());
}

// ================================================================
//                          HTTP-Handler
// ================================================================

// ---------------- JSON-API ----------------
void appendSensor(char* buf, size_t n, const char* key,
                  const Reading& r, int16_t offsetTenths) {
  char t[12], traw[12], h[12], off[12];
  dtostrf(r.temperature, 0, 1, t);
  dtostrf(r.rawTemp,     0, 1, traw);
  dtostrf(r.humidity,    0, 1, h);
  dtostrf(offsetTenths / 10.0f, 0, 1, off);
  char part[220];
  snprintf(part, sizeof(part),
    "\"%s\":{\"present\":%s,\"temperature\":%s,\"temperature_raw\":%s,"
    "\"offset\":%s,\"humidity\":%s,\"valid\":%s},",
    key,
    r.present ? "true" : "false",
    r.valid   ? t    : "null",
    r.valid   ? traw : "null",
    off,
    r.valid   ? h : "null",
    r.valid   ? "true" : "false");
  strncat(buf, part, n - strlen(buf) - 1);
}

void handleApi() {
  char buf[900];
  buf[0] = '\0';

  String nameEsc = jsonEscape(cfg.name);
  char head[120];
  snprintf(head, sizeof(head), "{\"name\":\"%s\",\"setup_mode\":%s,",
           nameEsc.c_str(), apMode ? "true" : "false");
  strncat(buf, head, sizeof(buf) - strlen(buf) - 1);

  appendSensor(buf, sizeof(buf), "sensor1", r1, cfg.offT1);
  appendSensor(buf, sizeof(buf), "sensor2", r2, cfg.offT2);

  // MQTT-Status (Passwort wird nie ausgegeben)
  String mq = "\"mqtt\":{\"enabled\":" +
              String(mqttConfigured() ? "true" : "false") +
              ",\"connected\":" + String(mqtt.connected() ? "true" : "false") +
              ",\"host\":\"" + jsonEscape(cfg.mqttHost) + "\"" +
              ",\"port\":" + String(getMqttPort()) +
              ",\"user\":\"" + jsonEscape(cfg.mqttUser) + "\"},";
  strncat(buf, mq.c_str(), sizeof(buf) - strlen(buf) - 1);

  char tail[80];
  snprintf(tail, sizeof(tail), "\"rssi\":%d,\"uptime_s\":%lu}",
           WiFi.RSSI(), millis() / 1000UL);
  strncat(buf, tail, sizeof(buf) - strlen(buf) - 1);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", buf);
}

// ---------------- Offset-API ----------------
// /api/offset?sensor=1&delta=1    -> +0.1 C
// /api/offset?sensor=2&delta=-1   -> -0.1 C
// /api/offset?sensor=1&set=-0.3   -> absolut setzen (0 = Reset)
void handleOffset() {
  if (!server.hasArg("sensor")) {
    server.send(400, "application/json", "{\"error\":\"sensor fehlt (1 oder 2)\"}");
    return;
  }
  int s = server.arg("sensor").toInt();
  if (s != 1 && s != 2) {
    server.send(400, "application/json", "{\"error\":\"sensor muss 1 oder 2 sein\"}");
    return;
  }

  int16_t& off = (s == 1) ? cfg.offT1 : cfg.offT2;
  Reading& r   = (s == 1) ? r1        : r2;
  int16_t oldOff = off;

  if (server.hasArg("delta")) {
    off = constrain((int16_t)(off + server.arg("delta").toInt()),
                    OFFSET_MIN, OFFSET_MAX);
  } else if (server.hasArg("set")) {
    float v = server.arg("set").toFloat();
    off = constrain((int16_t)lroundf(v * 10.0f), OFFSET_MIN, OFFSET_MAX);
  } else {
    server.send(400, "application/json", "{\"error\":\"delta oder set fehlt\"}");
    return;
  }

  // Anzeige sofort anpassen, ohne auf naechste Messung zu warten
  if (r.valid) {
    r.temperature = r.rawTemp + off / 10.0f;
  }

  if (off != oldOff) {
    saveConfig();      // persistent speichern
    updateDisplay();   // OLED direkt aktualisieren
    // Neuen Wert auch sofort per MQTT melden
    if (mqtt.connected()) publishState();
  }

  char resp[80];
  char offStr[12];
  dtostrf(off / 10.0f, 0, 1, offStr);
  snprintf(resp, sizeof(resp), "{\"sensor\":%d,\"offset\":%s}", s, offStr);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", resp);
}

// ---------------- WLAN-Scan-API ----------------
void handleScan() {
  Serial.println("Starte WLAN-Scan...");
  int n = WiFi.scanNetworks();   // blockiert ~2-3 s, fuer Setup ok
  String json = "[";
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;              // versteckte Netze ueberspringen
    bool dup = false;
    for (int j = 0; j < i; j++) {
      if (WiFi.SSID(j) == s) { dup = true; break; }
    }
    if (dup) continue;
    if (json.length() > 1) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(s.c_str()) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"enc\":" + String(WiFi.encryptionType(i) == ENC_TYPE_NONE ? "false" : "true") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ---------------- Konfigurations-API (Name + WLAN + MQTT) ----------------
// POST /api/config  mit  name=...&ssid=...&pass=...
//                        &mqtt_host=...&mqtt_port=...&mqtt_user=...&mqtt_pass=...
// ssid/pass optional (dann bleibt das WLAN unveraendert).
// mqtt_host leer = MQTT deaktivieren. mqtt_pass leer = Passwort unveraendert.
// Nach dem Speichern startet der ESP neu.
void handleConfigSave() {
  bool changed = false;

  if (server.hasArg("name")) {
    String n = server.arg("name");
    n.trim();
    if (n.length() > 0) {
      strlcpy(cfg.name, n.c_str(), sizeof(cfg.name));
      changed = true;
    }
  }

  if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
    strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
    // Passwort gehoert zur SSID; leer = offenes Netzwerk
    strlcpy(cfg.pass, server.hasArg("pass") ? server.arg("pass").c_str() : "",
            sizeof(cfg.pass));
    changed = true;
  }

  if (server.hasArg("mqtt_host")) {
    String h = server.arg("mqtt_host");
    h.trim();
    strlcpy(cfg.mqttHost, h.c_str(), sizeof(cfg.mqttHost));   // leer = aus
    cfg.mqttPort = (uint16_t)server.arg("mqtt_port").toInt(); // 0 -> 1883
    strlcpy(cfg.mqttUser, server.arg("mqtt_user").c_str(), sizeof(cfg.mqttUser));
    // Passwort nur ueberschreiben, wenn eines eingegeben wurde
    if (server.arg("mqtt_pass").length() > 0) {
      strlcpy(cfg.mqttPass, server.arg("mqtt_pass").c_str(), sizeof(cfg.mqttPass));
    }
    changed = true;
  }

  if (!changed) {
    server.send(400, "application/json", "{\"error\":\"keine Daten\"}");
    return;
  }

  saveConfig();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"ok\":true,\"restart\":true}");

  Serial.println("Neue Konfiguration -> Neustart in 1 s");
  delay(1000);        // Antwort noch rausschicken
  ESP.restart();
}

// ---------------- Live-Webseite ----------------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Klima-Monitor</title>
<style>
 body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:2rem;text-align:center}
 h1{font-weight:600;font-size:1.5rem}
 .grid{display:flex;gap:1rem;justify-content:center;flex-wrap:wrap;margin-top:1.5rem}
 .card{background:#1c1c1c;border:1px solid #333;border-radius:14px;padding:1.5rem 2rem;min-width:180px}
 .t{font-size:2.2rem;font-weight:700}
 .h{font-size:1.2rem;color:#9ad}
 .off{margin-top:.9rem;padding-top:.7rem;border-top:1px solid #333;font-size:.85rem;color:#aaa}
 .off .val{display:inline-block;min-width:3.6em;font-variant-numeric:tabular-nums;color:#eee}
 .off button{background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:8px;
   width:2.2em;height:2.2em;font-size:1rem;cursor:pointer;margin:0 .25em}
 .off button:active{background:#3a3a3a}
 .off .reset{width:auto;padding:0 .6em;font-size:.75rem;height:1.9em;margin-left:.5em}
 a.cfg{color:#9ad;text-decoration:none;font-size:.9rem;display:inline-block;margin-top:1.5rem}
 small{color:#888}
</style></head><body>
<h1 id="title">Klima-Monitor</h1>
<div class="grid">
  <div class="card" id="card1"><div>Sensor 1</div>
    <div class="t" id="t1">--</div><div class="h" id="h1">--</div>
    <div class="off">Offset:
      <button onclick="adj(1,-1)">&minus;</button><span class="val" id="o1">+0.0</span><button onclick="adj(1,1)">+</button>
      <button class="reset" onclick="rst(1)">Reset</button>
    </div>
  </div>
  <div class="card" id="card2"><div>Sensor 2</div>
    <div class="t" id="t2">--</div><div class="h" id="h2">--</div>
    <div class="off">Offset:
      <button onclick="adj(2,-1)">&minus;</button><span class="val" id="o2">+0.0</span><button onclick="adj(2,1)">+</button>
      <button class="reset" onclick="rst(2)">Reset</button>
    </div>
  </div>
</div>
<a class="cfg" href="/setup">&#9881; Einstellungen (Name / WLAN / MQTT)</a>
<p><small id="meta"></small></p>
<script>
function fmtOff(v){ return (v>=0?'+':'')+v.toFixed(1)+' \u00B0C'; }
function setCard(n,s){
  var card=document.getElementById('card'+n);
  if(!s.present){card.style.display='none';return;}
  card.style.display='';
  document.getElementById('t'+n).textContent = s.valid ? s.temperature.toFixed(1)+' \u00B0C' : '\u2014';
  document.getElementById('h'+n).textContent = s.valid ? s.humidity.toFixed(0)+' %' : '\u2014';
  document.getElementById('o'+n).textContent = fmtOff(s.offset);
}
async function poll(){
  try{
    const d = await (await fetch('/api')).json();
    document.getElementById('title').textContent = d.name;
    document.title = d.name;
    setCard('1', d.sensor1);
    setCard('2', d.sensor2);
    var m = 'RSSI '+d.rssi+' dBm \u00B7 Uptime '+d.uptime_s+' s';
    if(d.mqtt && d.mqtt.enabled){
      m += ' \u00B7 MQTT: '+(d.mqtt.connected?'verbunden':'getrennt');
    }
    document.getElementById('meta').textContent = m;
  }catch(e){ document.getElementById('meta').textContent='Fehler beim Abrufen'; }
}
async function adj(s,d){
  try{
    const r = await (await fetch('/api/offset?sensor='+s+'&delta='+d)).json();
    document.getElementById('o'+s).textContent = fmtOff(r.offset);
    poll();
  }catch(e){}
}
async function rst(s){
  try{
    const r = await (await fetch('/api/offset?sensor='+s+'&set=0')).json();
    document.getElementById('o'+s).textContent = fmtOff(r.offset);
    poll();
  }catch(e){}
}
poll(); setInterval(poll, 3000);
</script></body></html>
)HTML";

// ---------------- Setup-/Einstellungsseite ----------------
const char SETUP_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Einstellungen</title>
<style>
 body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:2rem;
   display:flex;justify-content:center}
 .box{background:#1c1c1c;border:1px solid #333;border-radius:14px;padding:1.5rem 2rem;
   width:100%;max-width:420px}
 h1{font-weight:600;font-size:1.3rem;margin-top:0}
 h2{font-size:.95rem;color:#9ad;margin:1.4rem 0 .5rem;border-top:1px solid #333;padding-top:1rem}
 label{display:block;font-size:.85rem;color:#aaa;margin:.8rem 0 .25rem}
 input{width:100%;box-sizing:border-box;background:#111;color:#eee;border:1px solid #444;
   border-radius:8px;padding:.55em .7em;font-size:1rem}
 button{background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:8px;
   padding:.55em 1em;font-size:.95rem;cursor:pointer;margin-top:.6rem}
 button:active{background:#3a3a3a}
 .save{background:#1e4620;border-color:#2e7d32;width:100%;margin-top:1.4rem;font-weight:600}
 #nets{display:flex;flex-direction:column;gap:.35rem;margin-top:.5rem}
 #nets button{text-align:left;margin:0}
 #msg{margin-top:1rem;color:#9ad;font-size:.9rem;min-height:1.2em}
 .hint{font-size:.75rem;color:#777;margin-top:.3rem}
 a{color:#9ad;text-decoration:none;font-size:.85rem}
</style></head><body>
<div class="box">
<h1>Einstellungen</h1>

<h2>Ger&auml;t</h2>
<label for="name">Ger&auml;tename (z.B. Kinderzimmer)</label>
<input id="name" maxlength="31" placeholder="Klima-Sensor">

<h2>WLAN</h2>
<button onclick="scan()">Netzwerke suchen</button>
<div id="nets"></div>
<label for="ssid">SSID</label>
<input id="ssid" maxlength="32" placeholder="Netzwerk ausw&auml;hlen oder eintippen">
<label for="pass">Passwort</label>
<input id="pass" type="password" maxlength="63" placeholder="leer lassen = WLAN unver&auml;ndert / offenes Netz">

<h2>MQTT / Home Assistant (optional)</h2>
<label for="mhost">Broker-Adresse</label>
<input id="mhost" maxlength="40" placeholder="z.B. 192.168.1.10 (leer = MQTT aus)">
<label for="mport">Port</label>
<input id="mport" type="number" min="1" max="65535" placeholder="1883">
<label for="muser">Benutzer</label>
<input id="muser" maxlength="32" placeholder="optional">
<label for="mpass">Passwort</label>
<input id="mpass" type="password" maxlength="40" placeholder="leer = unver&auml;ndert">
<div class="hint">Mit gesetztem Broker meldet sich der Sensor per
Home Assistant Discovery automatisch als Ger&auml;t an.</div>

<button class="save" onclick="save()">Speichern &amp; Neustart</button>
<div id="msg"></div>
<p><a href="/">&larr; zur&uuml;ck zur Anzeige</a></p>
</div>
<script>
async function load(){
  try{
    const d = await (await fetch('/api')).json();
    document.getElementById('name').value = d.name;
    if(d.mqtt){
      document.getElementById('mhost').value = d.mqtt.host || '';
      document.getElementById('mport').value = d.mqtt.port || '';
      document.getElementById('muser').value = d.mqtt.user || '';
    }
  }catch(e){}
}
async function scan(){
  const div=document.getElementById('nets');
  div.textContent='Suche l\u00E4uft...';
  try{
    const l = await (await fetch('/api/scan')).json();
    div.innerHTML='';
    if(l.length===0){div.textContent='Keine Netzwerke gefunden';return;}
    l.sort((a,b)=>b.rssi-a.rssi);
    l.forEach(n=>{
      const b=document.createElement('button');
      b.textContent=n.ssid+'  ('+n.rssi+' dBm)'+(n.enc?' \uD83D\uDD12':'');
      b.onclick=()=>{document.getElementById('ssid').value=n.ssid;
                     document.getElementById('pass').focus();};
      div.appendChild(b);
    });
  }catch(e){ div.textContent='Fehler beim Scannen'; }
}
async function save(){
  const msg=document.getElementById('msg');
  const p=new URLSearchParams();
  p.append('name', document.getElementById('name').value);
  const s=document.getElementById('ssid').value.trim();
  if(s){ p.append('ssid', s); p.append('pass', document.getElementById('pass').value); }
  p.append('mqtt_host', document.getElementById('mhost').value.trim());
  p.append('mqtt_port', document.getElementById('mport').value.trim());
  p.append('mqtt_user', document.getElementById('muser').value.trim());
  const mp=document.getElementById('mpass').value;
  if(mp){ p.append('mqtt_pass', mp); }
  msg.textContent='Speichere...';
  try{
    await fetch('/api/config',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:p.toString()});
    msg.textContent='Gespeichert \u2013 Ger\u00E4t startet neu. Diese Seite kann geschlossen werden.';
  }catch(e){
    msg.textContent='Gespeichert \u2013 Ger\u00E4t startet neu.';
  }
}
load();
</script></body></html>
)HTML";

void handleRoot() {
  // Im Setup-Modus direkt die Einstellungsseite zeigen
  if (apMode) server.send_P(200, "text/html", SETUP_HTML);
  else        server.send_P(200, "text/html", INDEX_HTML);
}

void handleSetupPage() { server.send_P(200, "text/html", SETUP_HTML); }

void handleNotFound() {
  if (apMode) {
    // Captive Portal: alles Unbekannte auf die Setup-Seite umleiten
    server.sendHeader("Location",
                      String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "404: Not found");
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  dht1.begin();
  dht2.begin();

  chipIdStr = String(ESP.getChipId(), HEX);

  loadConfig();                    // Name, WLAN, Offsets, MQTT aus EEPROM
  hostName = slugify(cfg.name);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 nicht gefunden (Adresse/Verkabelung pruefen)");
  }
  showBootScreen("Suche Sensoren...", "", "");

  detectSensors();

  // WLAN: erst gespeicherte Daten versuchen, sonst Setup-Hotspot
  if (!connectSTA()) {
    startAP();
  } else {
    if (MDNS.begin(hostName.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS aktiv: http://%s.local\n", hostName.c_str());
    }
    mqttSetup();                   // MQTT nur im normalen Betrieb
  }

  server.on("/",           handleRoot);
  server.on("/setup",      handleSetupPage);
  server.on("/api",        handleApi);
  server.on("/api/offset", handleOffset);
  server.on("/api/scan",   handleScan);
  server.on("/api/config", handleConfigSave);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP-Server gestartet");

  readSensors();
  updateDisplay();
}

// ---------------- Loop ----------------
void loop() {
  server.handleClient();

  if (apMode) {
    dnsServer.processNextRequest();

    // Wenn WLAN-Daten gespeichert sind und gerade niemand am Hotspot
    // haengt: alle 5 min neu versuchen (z.B. Router war kurz offline)
    if (strlen(cfg.ssid) > 0 &&
        millis() - apStarted > AP_RETRY_INTERVAL &&
        WiFi.softAPgetStationNum() == 0) {
      Serial.println("Erneuter WLAN-Versuch -> Neustart");
      ESP.restart();
    }
  } else {
    MDNS.update();

    // MQTT nur, wenn ein Broker konfiguriert ist
    if (mqttConfigured()) {
      mqttEnsureConnected();
      if (mqtt.connected()) {
        mqtt.loop();
        if (millis() - lastMqttPublish > MQTT_PUBLISH_INTERVAL) {
          lastMqttPublish = millis();
          publishState();
        }
      }
    }
  }

  if (millis() - lastMeasure > MEASURE_INTERVAL) {
    lastMeasure = millis();
    readSensors();
    updateDisplay();
  }
}
