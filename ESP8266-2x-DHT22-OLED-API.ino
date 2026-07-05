/*
 * ESP8266 – 1-2x DHT22 + OLED SSD1306 + REST-API + Offset + WLAN-Setup
 * --------------------------------------------------------------------
 * NEU in dieser Version:
 *  - Geraetename ueber die Web-Oberflaeche einstellbar (persistent)
 *  - WLAN-Provisioning: Ohne gespeicherte Zugangsdaten (oder wenn die
 *    Verbindung fehlschlaegt) oeffnet der ESP einen Hotspot
 *    "Klima-Setup-xxxxxx" mit Captive Portal. Dort kann man nach
 *    Netzwerken scannen, eines auswaehlen und das Passwort eingeben.
 *    SSID + Passwort werden persistent im EEPROM gespeichert.
 *  - Offsets aus der vorherigen Sketch-Version werden automatisch
 *    migriert (alte EEPROM-Struktur wird erkannt).
 *
 * Endpunkte:
 *    http://<IP>/            -> Live-Webseite (Sensorwerte + Offsets)
 *    http://<IP>/setup       -> Einstellungen (Name, WLAN)
 *    http://<IP>/api         -> JSON fuer eigene Anwendungen
 *    http://<IP>/api/offset  -> Offset setzen (sensor=1|2, delta|set)
 *    http://<IP>/api/scan    -> WLAN-Scan als JSON
 *    http://<IP>/api/config  -> Name/WLAN speichern (POST) -> Neustart
 *    http://<name>.local     -> per mDNS
 *
 * Benoetigte Bibliotheken (Library Manager):
 *   - "DHT sensor library" (Adafruit)
 *   - "Adafruit Unified Sensor"
 *   - "Adafruit GFX Library"
 *   - "Adafruit SSD1306"
 * (ESP8266WiFi, WebServer, mDNS, DNSServer, EEPROM sind im Core enthalten)
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

// ---------------- Setup-Hotspot ----------------
// Leer = offener Hotspot. Wenn gesetzt, mindestens 8 Zeichen!
const char* AP_PASSWORD = "";
const char* DEFAULT_NAME = "Klima-Sensor";

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

// ---------------- Webserver / DNS ----------------
ESP8266WebServer server(80);
DNSServer dnsServer;
bool apMode = false;
String apSsid;

// ---------------- Persistente Konfiguration ----------------
// Version 2: Name + WLAN + Offsets. Offsets in ZEHNTELGRAD (int16),
// damit es keine Float-Rundungsprobleme gibt. 3 == +0.3 C
struct PersistData {
  uint32_t magic;
  char     name[32];      // Geraetename (UTF-8, wird ge-slugified fuer mDNS)
  char     ssid[33];      // WLAN-SSID (max. 32 Zeichen + \0)
  char     pass[64];      // WLAN-Passwort (max. 63 Zeichen + \0)
  int16_t  offT1;         // Offset Sensor 1 in 0.1 C
  int16_t  offT2;         // Offset Sensor 2 in 0.1 C
};
// Alte Struktur aus der vorherigen Sketch-Version (nur Offsets):
struct PersistDataV1 {
  uint32_t magic;
  int16_t  offT1;
  int16_t  offT2;
};
const uint32_t EE_MAGIC_V1 = 0x44485432;   // "DHT2" (alte Version)
const uint32_t EE_MAGIC_V2 = 0x44485433;   // "DHT3" (aktuelle Version)
const int16_t  OFFSET_MIN  = -100;         // -10.0 C
const int16_t  OFFSET_MAX  =  100;         // +10.0 C

PersistData cfg;

void saveConfig() {
  cfg.magic = EE_MAGIC_V2;
  EEPROM.put(0, cfg);
  EEPROM.commit();   // beim ESP8266 zwingend noetig!
  Serial.println("Konfiguration gespeichert.");
}

void loadConfig() {
  EEPROM.begin(256);
  EEPROM.get(0, cfg);

  if (cfg.magic == EE_MAGIC_V2) {
    // Strings sicherheitshalber terminieren
    cfg.name[sizeof(cfg.name) - 1] = '\0';
    cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    cfg.pass[sizeof(cfg.pass) - 1] = '\0';
    cfg.offT1 = constrain(cfg.offT1, OFFSET_MIN, OFFSET_MAX);
    cfg.offT2 = constrain(cfg.offT2, OFFSET_MIN, OFFSET_MAX);
    Serial.printf("Konfig geladen: Name='%s' SSID='%s' Offsets S1=%+.1f S2=%+.1f\n",
                  cfg.name, cfg.ssid, cfg.offT1 / 10.0f, cfg.offT2 / 10.0f);
    return;
  }

  if (cfg.magic == EE_MAGIC_V1) {
    // Migration: Offsets aus alter Version uebernehmen
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
const unsigned long MEASURE_INTERVAL = 5000;        // alle 5 s messen
const unsigned long AP_RETRY_INTERVAL = 5UL * 60UL * 1000UL; // 5 min
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
  apSsid = "Klima-Setup-" + String(ESP.getChipId(), HEX);

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
  char buf[700];
  buf[0] = '\0';

  String nameEsc = jsonEscape(cfg.name);
  char head[120];
  snprintf(head, sizeof(head), "{\"name\":\"%s\",\"setup_mode\":%s,",
           nameEsc.c_str(), apMode ? "true" : "false");
  strncat(buf, head, sizeof(buf) - strlen(buf) - 1);

  appendSensor(buf, sizeof(buf), "sensor1", r1, cfg.offT1);
  appendSensor(buf, sizeof(buf), "sensor2", r2, cfg.offT2);

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
    // Duplikate (Mesh/Repeater) ueberspringen
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

// ---------------- Konfigurations-API (Name + WLAN) ----------------
// POST /api/config  mit  name=...&ssid=...&pass=...
// ssid/pass sind optional (dann bleibt das WLAN unveraendert).
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
<a class="cfg" href="/setup">&#9881; Einstellungen (Name / WLAN)</a>
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
    document.getElementById('meta').textContent = 'RSSI '+d.rssi+' dBm \u00B7 Uptime '+d.uptime_s+' s';
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

<button class="save" onclick="save()">Speichern &amp; Neustart</button>
<div id="msg"></div>
<p><a href="/">&larr; zur&uuml;ck zur Anzeige</a></p>
</div>
<script>
async function load(){
  try{
    const d = await (await fetch('/api')).json();
    document.getElementById('name').value = d.name;
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

  loadConfig();                    // Name, WLAN, Offsets aus EEPROM
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
  } else if (MDNS.begin(hostName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS aktiv: http://%s.local\n", hostName.c_str());
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
  }

  if (millis() - lastMeasure > MEASURE_INTERVAL) {
    lastMeasure = millis();
    readSensors();
    updateDisplay();
  }
}
