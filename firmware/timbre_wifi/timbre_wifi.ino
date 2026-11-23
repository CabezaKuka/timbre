/*
  TIMBRE WiFi - Wemos D1 mini (ESP8266)
  ----------------------------------------------------------
  - Al arrancar levanta el AP "Timbre-XXXX" con portal de configuración
    (http://192.168.4.1, aparece solo como "Iniciar sesión en la red").
  - Si en 60 s no hay nadie conectado al AP (contados desde que se va
    el último cliente), lo cierra y se conecta con los datos guardados.
  - Sin datos guardados, el AP queda abierto indefinidamente.
  - Conectado a la red: escucha UDP en el puerto 4210.
      "PING" -> responde "TIMBRE|<nombre>"   (descubrimiento por broadcast)
      "RING" -> activa el relé y responde "OK|<nombre>"
                (o "BUSY|<nombre>" si ya está sonando / en cooldown)
  - También responde HTTP: http://<ip>/  (página con botón) y /ring
  - Para cambiar de red: apagar y encender, entrar al AP en los primeros 60 s.

  Placa: "LOLIN(WEMOS) D1 R2 & mini"  (core ESP8266 3.x)
  Sin librerías externas.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiUdp.h>
#include <EEPROM.h>

// ====================== CONFIGURACIÓN ======================
#define RELAY_PIN         D1      // GPIO5
#define RELAY_ACTIVE_LOW  true    // la mayoría de módulos chinos: true
#define LED_PIN           LED_BUILTIN  // GPIO2, activo en LOW

const uint16_t PULSE_ON_MS   = 1000;  // tiempo de relé activado por pulso
const uint16_t PULSE_OFF_MS  = 300;   // pausa entre pulsos (si PULSE_COUNT > 1)
const uint8_t  PULSE_COUNT   = 1;     // cantidad de pulsos por toque
const uint32_t COOLDOWN_MS   = 3000;  // ignora toques repetidos en este lapso
const uint32_t AP_WINDOW_MS  = 60000; // ventana del AP sin clientes
const uint16_t UDP_PORT      = 4210;
const char*    AP_PASSWORD   = "";    // "" = AP abierto; o mínimo 8 caracteres
const char*    HOSTNAME      = "timbre";
// ===========================================================

#define CFG_MAGIC 0x54494D42UL  // "TIMB"

struct Config {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  char name[24];
};

Config cfg;
bool cfgValid = false;

enum Mode { MODE_PORTAL, MODE_STA };
Mode mode = MODE_PORTAL;

ESP8266WebServer server(80);
DNSServer dns;
WiFiUDP udp;
IPAddress apIP(192, 168, 4, 1);

uint32_t portalT0 = 0;          // último instante con clientes en el AP
uint32_t switchToStaAt = 0;     // !=0 -> pasar a STA en ese instante (tras guardar)
bool wasConnected = false;

// ---- relé / patrón ----
bool     relayOn = false;
uint8_t  pulsesLeft = 0;
uint32_t tNext = 0;
uint32_t lastRing = 0;
bool     everRang = false;

// ===================== EEPROM =====================
void loadConfig() {
  EEPROM.begin(sizeof(Config));
  EEPROM.get(0, cfg);
  cfgValid = (cfg.magic == CFG_MAGIC && cfg.ssid[0] != 0);
  if (!cfgValid) {
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.name, "Timbre");
  }
  cfg.ssid[32] = 0; cfg.pass[64] = 0; cfg.name[23] = 0;
}

void saveConfig() {
  cfg.magic = CFG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
  cfgValid = true;
}

// ===================== RELÉ =====================
void setRelay(bool on) {
  relayOn = on;
  digitalWrite(RELAY_PIN, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

bool isBusy() {
  return relayOn || pulsesLeft > 0 ||
         (everRang && (millis() - lastRing) < COOLDOWN_MS);
}

// true si arrancó el patrón, false si estaba ocupado
bool triggerRing() {
  if (isBusy()) return false;
  everRang = true;
  lastRing = millis();
  pulsesLeft = PULSE_COUNT - 1;
  setRelay(true);
  tNext = millis() + PULSE_ON_MS;
  Serial.println(F("[RING]"));
  return true;
}

void updateRing() {
  if (!relayOn && pulsesLeft == 0) return;
  if ((int32_t)(millis() - tNext) < 0) return;
  if (relayOn) {
    setRelay(false);
    tNext = millis() + PULSE_OFF_MS;
  } else if (pulsesLeft > 0) {
    pulsesLeft--;
    setRelay(true);
    tNext = millis() + PULSE_ON_MS;
  }
}

// ===================== LED =====================
void updateLed() {
  static uint32_t t = 0;
  static bool st = false;
  uint32_t period;
  if (mode == MODE_PORTAL) period = 200;                    // rápido: AP activo
  else if (WiFi.status() != WL_CONNECTED) period = 1000;    // lento: conectando
  else { digitalWrite(LED_PIN, relayOn ? LOW : HIGH); return; } // apagado, prende al sonar
  if (millis() - t >= period) {
    t = millis(); st = !st;
    digitalWrite(LED_PIN, st ? LOW : HIGH);
  }
}

// ===================== HTML =====================
String htmlEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) {
      case '&': o += F("&amp;"); break;
      case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;"); break;
      case '"': o += F("&quot;"); break;
      case '\'': o += F("&#39;"); break;
      default: o += c;
    }
  }
  return o;
}

const char HTML_HEAD[] PROGMEM = R"(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Timbre</title>
<style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 14px;background:#f4f4f4}
h2{margin:8px 0}input{width:100%;padding:10px;margin:4px 0 12px;box-sizing:border-box;font-size:16px}
button{width:100%;padding:12px;font-size:17px;border:0;border-radius:6px;background:#1565c0;color:#fff}
.n{display:block;padding:8px;margin:3px 0;background:#fff;border-radius:4px;text-decoration:none;color:#000}
.s{color:#666;font-size:13px}.big{height:180px;font-size:26px;border-radius:50%;width:180px;display:block;margin:30px auto}
</style></head><body>)";

void sendPortalPage(const String& msg = "") {
  String h = FPSTR(HTML_HEAD);
  h += F("<h2>Configurar timbre</h2>");
  if (msg.length()) h += "<p><b>" + msg + "</b></p>";

  int n = WiFi.scanComplete();
  h += F("<p class='s'>Redes encontradas (toque para elegir) · <a href='/scan'>actualizar</a></p>");
  if (n == WIFI_SCAN_RUNNING) h += F("<p class='s'>Buscando redes...</p>");
  else if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); h += F("<p class='s'>Buscando redes...</p>"); }
  else {
    for (int i = 0; i < n && i < 15; i++) {
      String s = htmlEscape(WiFi.SSID(i));
      if (!s.length()) continue;
      h += "<a class='n' href='#' onclick=\"document.getElementById('s').value=this.dataset.s;return false\" data-s=\"" + s + "\">" +
           s + " <span class='s'>(" + String(WiFi.RSSI(i)) + " dBm" +
           (WiFi.encryptionType(i) == ENC_TYPE_NONE ? ", abierta" : "") + ")</span></a>";
    }
  }

  h += F("<form method='POST' action='/save'>Red WiFi (SSID)<input id='s' name='ssid' maxlength='32' value=\"");
  h += htmlEscape(cfg.ssid);
  h += F("\">Contraseña<input name='pass' type='password' maxlength='64' placeholder='");
  h += cfgValid ? F("(vacío = mantener la actual)") : F("");
  h += F("'>Nombre del timbre<input name='name' maxlength='23' value=\"");
  h += htmlEscape(cfg.name);
  h += F("\"><button type='submit'>Guardar y conectar</button></form>");
  h += F("<p class='s'>El AP se cierra 60 s después de que no quede nadie conectado.</p></body></html>");
  server.send(200, "text/html", h);
}

void sendStatusPage() {
  String h = FPSTR(HTML_HEAD);
  h += "<h2>" + htmlEscape(cfg.name) + "</h2><p class='s'>Red: " + htmlEscape(cfg.ssid) +
       " · IP: " + WiFi.localIP().toString() + " · " + String(WiFi.RSSI()) + " dBm</p>";
  h += F("<button class='big' onclick=\"fetch('/ring').then(r=>r.text()).then(t=>document.getElementById('r').innerText=t).catch(e=>document.getElementById('r').innerText='Error')\">TOCAR</button>"
         "<p id='r' style='text-align:center'></p></body></html>");
  server.send(200, "text/html", h);
}

// ===================== HANDLERS =====================
void handleRoot() {
  if (mode == MODE_PORTAL) sendPortalPage();
  else sendStatusPage();
}

void handleScan() {
  if (mode != MODE_PORTAL) { server.send(404, "text/plain", "No"); return; }
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSave() {
  if (mode != MODE_PORTAL) { server.send(404, "text/plain", "No"); return; }
  String ssid = server.arg("ssid"); ssid.trim();
  String pass = server.arg("pass");
  String name = server.arg("name"); name.trim();
  if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) {
    sendPortalPage("SSID inválido");
    return;
  }
  bool keepPass = (pass.length() == 0 && cfgValid && ssid == String(cfg.ssid));
  name.replace("|", " ");
  if (!name.length()) name = "Timbre";

  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  if (!keepPass) strlcpy(cfg.pass, pass.c_str(), sizeof(cfg.pass));
  strlcpy(cfg.name, name.c_str(), sizeof(cfg.name));
  saveConfig();

  String h = FPSTR(HTML_HEAD);
  h += "<h2>Guardado</h2><p>Conectando a <b>" + htmlEscape(cfg.ssid) +
       "</b>. El AP se va a cerrar.</p><p class='s'>Si los datos son incorrectos, apague y encienda el equipo para volver a configurarlo.</p></body></html>";
  server.send(200, "text/html", h);
  switchToStaAt = millis() + 1500;  // dar tiempo a que salga la respuesta
}

void handleRing() {
  if (mode != MODE_STA) { server.send(404, "text/plain", "No"); return; }
  bool ok = triggerRing();
  server.send(ok ? 200 : 409, "text/plain", ok ? "OK" : "BUSY");
}

void handleNotFound() {
  if (mode == MODE_PORTAL) {  // captive portal: todo redirige al formulario
    server.sendHeader("Location", String("http://") + apIP.toString() + "/");
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// ===================== MODOS =====================
void startPortal() {
  mode = MODE_PORTAL;
  WiFi.persistent(false);
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);  // STA solo para escanear redes
  char apName[20];
  snprintf(apName, sizeof(apName), "Timbre-%04X", (uint16_t)(ESP.getChipId() & 0xFFFF));
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(apName, AP_PASSWORD);
  else WiFi.softAP(apName);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", apIP);
  WiFi.scanNetworks(true);
  portalT0 = millis();
  Serial.printf("[AP] %s activo en %s\n", apName, apIP.toString().c_str());
}

void startSTA() {
  dns.stop();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid, cfg.pass);
  udp.begin(UDP_PORT);
  mode = MODE_STA;
  wasConnected = false;
  Serial.printf("[STA] Conectando a '%s'...\n", cfg.ssid);
}

// ===================== UDP =====================
void handleUdp() {
  int len = udp.parsePacket();
  if (len <= 0) return;
  char buf[32];
  int n = udp.read(buf, sizeof(buf) - 1);
  if (n < 0) n = 0;
  buf[n] = 0;
  while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;

  String reply;
  if (strcmp(buf, "RING") == 0) {
    reply = triggerRing() ? "OK|" : "BUSY|";
    reply += cfg.name;
  } else if (strcmp(buf, "PING") == 0) {
    reply = "TIMBRE|";
    reply += cfg.name;
  } else {
    return;
  }
  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.print(reply);
  udp.endPacket();
}

// ===================== SETUP / LOOP =====================
void setup() {
  // Relé en reposo ANTES de configurarlo como salida (evita "click" al arrancar)
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(115200);
  delay(50);
  Serial.println(F("\n\nTimbre WiFi"));

  loadConfig();

  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/ring", handleRing);
  server.onNotFound(handleNotFound);
  server.begin();

  startPortal();
}

void loop() {
  server.handleClient();
  updateRing();
  updateLed();

  if (mode == MODE_PORTAL) {
    dns.processNextRequest();
    if (WiFi.softAPgetStationNum() > 0) portalT0 = millis();

    if (switchToStaAt && (int32_t)(millis() - switchToStaAt) >= 0) {
      switchToStaAt = 0;
      startSTA();
    } else if (cfgValid && !switchToStaAt && millis() - portalT0 > AP_WINDOW_MS) {
      Serial.println(F("[AP] Sin clientes, cerrando AP"));
      startSTA();
    }
  } else {
    handleUdp();
    bool c = (WiFi.status() == WL_CONNECTED);
    if (c && !wasConnected) {
      Serial.printf("[STA] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
    } else if (!c && wasConnected) {
      Serial.println(F("[STA] Conexión perdida, reintentando..."));
    }
    wasConnected = c;
  }
}
