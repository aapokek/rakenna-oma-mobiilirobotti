// Muokkaa tähän robottisi nimi ja wifi:n salasana:
String robotName = "MunRobotti";
static const char* AP_PASSWORD = "OmaSalasana"; // 8–63 merkkiä

/*
 * ════════════════════════════════════════════════════════
 *  Rakenna oma mobiilirobotti - Pullonkaula ry
 *    Tekijä: Aapo Kekkonen (TG: @aapokek)
 * ════════════════════════════════════════════════════════
 *
 *  Erikseen asennettavat kirjastot:
 *    - ESP8266WiFi
 *    - ESPAsyncTCP
 *    - ESPAsyncWebServer
 *    - DNSServer
 *    - LittleFS
 *
 *  KYTKENNÄT:
 *    - L298N mini
 *      > IN1 → D1 (GPIO5)   Vasen moottori +
 *      > IN2 → D2 (GPIO4)   Vasen moottori -
 *      > IN3 → D6 (GPIO12)  Oikea moottori +
 *      > IN4 → D5 (GPIO14)  Oikea moottori -
 *
 *    - ULTRAÄÄNIANTURIT (HC-SR04):
 *      > Etu Trig → D7 (GPIO13)
 *      > Etu Echo → D8 (GPIO15)
 *      > Taka Trig → D0 (GPIO16)
 *      > Taka Echo → D3 (GPIO0)
 */

#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <algorithm>
#include <math.h>

// ───── Robotin konfiguraatio tallennetaan sisäiseen muistiin ─────
static const char CONFIG_FILE[] = "/robot.cfg";

// ───── WiFi ─────
static constexpr uint16_t DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

DNSServer dnsServer;

// ───── Moottorien ohjauspinnit ─────
const int IN1 = 5;   // D1 – vasen +
const int IN2 = 4;   // D2 – vasen -
const int IN3 = 12;  // D6 – oikea +
const int IN4 = 14;  // D5 – oikea -

// ───── Ultraäänianturit ─────
const int FRONT_TRIG = 13; // D7
const int FRONT_ECHO = 15; // D8
const int BACK_TRIG  = 16; // D0
const int BACK_ECHO  =  0; // D3

static constexpr float SOUND_CM_PER_US = 0.0343f;
static constexpr uint32_t ECHO_TIMEOUT_US = 25000UL;
static constexpr float NO_ECHO_CM = 999.0f;

// Törmäyksenestoetäisyys skaalautuu nopeuden mukaan lineaarisesti
static constexpr float STOP_MIN_CM = 20.0f;  // Etäisyys minimi nopeudella (10 %)
static constexpr float STOP_MAX_CM = 50.0f;  // Etäisyys maksimi nopeudella (100 %)

// ───── Etäisyydet ─────
float frontCm = NO_ECHO_CM;
float backCm  = NO_ECHO_CM;

// ───── Kalibrointi ─────
float leftCalib  = 1.0f;
float rightCalib = 1.0f;

// ───── Nopeus ─────
int speedPercent = 50;
int speedPWM     = 0;

// ───── Painettujen suuntien järjestys ─────
enum Direction : uint8_t {
  DIR_NONE = 0,
  DIR_FORWARD,
  DIR_BACKWARD,
  DIR_LEFT,
  DIR_RIGHT
};

Direction pressedOrder[4] = {DIR_NONE, DIR_NONE, DIR_NONE, DIR_NONE};
uint8_t pressedCount = 0;

// ───── Anturien ajastus ─────
static constexpr uint32_t SENSOR_INTERVAL_MS = 100UL;
unsigned long lastSensorMs = 0;
bool measureFront = true;

// ───── Tukiaseman uudelleenkäynnistys nimen vaihdon jälkeen ─────
bool apRestartPending = false;
unsigned long apRestartAt = 0;
static constexpr uint32_t AP_RESTART_DELAY_MS = 400UL;

AsyncWebServer server(80);

// ═══════════════════════════════════════════════════════
//  APUFUNKTIOT
// ═══════════════════════════════════════════════════════

static inline int cal(int s, float k) {
  return constrain((int)(s * k), 0, 255);
}

static inline String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    switch (c) {
      case '&':  out += F("&amp;");  break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default:   out += c; break;
    }
  }
  return out;
}

static String utf8SafeTruncate(const String& in, size_t maxBytes) {
  if (in.length() <= maxBytes) return in;

  const char* data = in.c_str();
  size_t cut = maxBytes;
  while (cut > 0 && ((uint8_t)data[cut] & 0xC0) == 0x80) {
    --cut;
  }
  return in.substring(0, cut);
}

static String buildSsidFromName(const String& name) {
  String ssid = name;
  ssid.trim();
  if (ssid.isEmpty()) ssid = F("Robotti");
  return utf8SafeTruncate(ssid, 32);
}

static const char* dirName(Direction d) {
  switch (d) {
    case DIR_FORWARD:  return "forward";
    case DIR_BACKWARD: return "backward";
    case DIR_LEFT:     return "left";
    case DIR_RIGHT:    return "right";
    default:           return "stopped";
  }
}

static void setPressed(Direction d, bool isPressed) {
  if (isPressed) {
    for (uint8_t i = 0; i < pressedCount; ++i) {
      if (pressedOrder[i] == d) return;
    }
    if (pressedCount < 4) {
      pressedOrder[pressedCount++] = d;
    } else {
      pressedOrder[0] = d;
      pressedCount = 1;
    }
  } else {
    uint8_t out = 0;
    for (uint8_t i = 0; i < pressedCount; ++i) {
      if (pressedOrder[i] != d) {
        pressedOrder[out++] = pressedOrder[i];
      }
    }
    for (uint8_t i = out; i < 4; ++i) {
      pressedOrder[i] = DIR_NONE;
    }
    pressedCount = out;
  }
}

static Direction activeDirection() {
  if (pressedCount == 0) return DIR_NONE;
  return pressedOrder[pressedCount - 1];
}

// ═══════════════════════════════════════════════════════
//  MOOTTORINOHJAUS
// ═══════════════════════════════════════════════════════

void motorStop() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void motorForward(int s) {
  analogWrite(IN2, cal(s, leftCalib));  digitalWrite(IN1, LOW);
  analogWrite(IN4, cal(s, rightCalib)); digitalWrite(IN3, LOW);
}

void motorBackward(int s) {
  analogWrite(IN1, cal(s, leftCalib));  digitalWrite(IN2, LOW);
  analogWrite(IN3, cal(s, rightCalib)); digitalWrite(IN4, LOW);
}

void motorLeft(int s) {
  analogWrite(IN1, cal(s, leftCalib));  digitalWrite(IN2, LOW);
  analogWrite(IN4, cal(s, rightCalib)); digitalWrite(IN3, LOW);
}

void motorRight(int s) {
  analogWrite(IN2, cal(s, leftCalib));  digitalWrite(IN1, LOW);
  analogWrite(IN3, cal(s, rightCalib)); digitalWrite(IN4, LOW);
}

// ═══════════════════════════════════════════════════════
//  ULTRAÄÄNIMITTAUS
// ═══════════════════════════════════════════════════════

float readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, ECHO_TIMEOUT_US);
  if (duration == 0) return NO_ECHO_CM;
  return duration * SOUND_CM_PER_US / 2.0f;
}

// ═══════════════════════════════════════════════════════
//  LIIKKEENHALLINTA
// ═══════════════════════════════════════════════════════

void applyMotion() {
  Direction dir = activeDirection();

  // Törmäyksenestoetäisyys lasketaan lineaarisesti nopeuden mukaan
  float stopCm = STOP_MIN_CM + (STOP_MAX_CM - STOP_MIN_CM) * (speedPercent - 10) / 90.0f;

  if (dir == DIR_FORWARD  && frontCm < stopCm) dir = DIR_NONE;
  if (dir == DIR_BACKWARD && backCm  < stopCm) dir = DIR_NONE;

  switch (dir) {
    case DIR_FORWARD:  motorForward(speedPWM);  break;
    case DIR_BACKWARD: motorBackward(speedPWM); break;
    case DIR_LEFT:     motorLeft(speedPWM);     break;
    case DIR_RIGHT:    motorRight(speedPWM);    break;
    default:           motorStop();             break;
  }
}

void handleButton(Direction direction, bool isPressed) {
  setPressed(direction, isPressed);
  applyMotion();
}

// ═══════════════════════════════════════════════════════
//  NOPEUS
// ═══════════════════════════════════════════════════════

static inline int percentToPWM(int pct) {
  // 10 % → PWM 130
  // 100 % → PWM 255
  return map(constrain(pct, 10, 100), 10, 100, 130, 255);
}

// ═══════════════════════════════════════════════════════
//  TALLENNUS
// ═══════════════════════════════════════════════════════

bool saveConfig() {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return false;

  f.println("name=" + robotName);
  f.printf("speed=%d\n", speedPercent);
  f.printf("lcal=%.2f\n", leftCalib);
  f.printf("rcal=%.2f\n", rightCalib);
  f.close();
  return true;
}

bool loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) return false;

  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("name=")) {
      String v = line.substring(5);
      v.trim();
      if (!v.isEmpty()) robotName = v;
    } else if (line.startsWith("speed=")) {
      int v = line.substring(6).toInt();
      if (v >= 10 && v <= 100) speedPercent = v;
    } else if (line.startsWith("lcal=")) {
      float v = line.substring(5).toFloat();
      if (isfinite(v) && v >= 0.5f && v <= 1.0f) leftCalib = v;
    } else if (line.startsWith("rcal=")) {
      float v = line.substring(5).toFloat();
      if (isfinite(v) && v >= 0.5f && v <= 1.0f) rightCalib = v;
    }
  }
  f.close();

  speedPercent = constrain(speedPercent, 10, 100);
  leftCalib = constrain(leftCalib, 0.5f, 1.0f);
  rightCalib = constrain(rightCalib, 0.5f, 1.0f);
  speedPWM = percentToPWM(speedPercent);
  return true;
}

// ═══════════════════════════════════════════════════════
//  WIFI TUKIASEMA
// ═══════════════════════════════════════════════════════

void startAccessPoint() {
  String ssid = buildSsidFromName(robotName);

  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(50);
  WiFi.mode(WIFI_AP);
  delay(50);

  WiFi.softAPdisconnect(true);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(ssid.c_str(), AP_PASSWORD, 1, false, 4);
  if (!ok) {
    WiFi.softAPdisconnect(true);
    WiFi.softAP("Robotti", AP_PASSWORD, 1, false, 4);
  }

  dnsServer.stop();
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
}

// ═══════════════════════════════════════════════════════
//  WEB-SIVU
// ═══════════════════════════════════════════════════════

static const char PAGE_TMPL[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html lang="fi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>{{NAME}}</title>
<style>
*{-webkit-user-select:none;user-select:none;-webkit-touch-callout:none;
  -webkit-tap-highlight-color:transparent;touch-action:manipulation;box-sizing:border-box;}
body{font-family:Arial,sans-serif;background:#111;color:#fff;
     margin:0;padding:16px;text-align:center;}
h1{font-size:clamp(26px,7vw,40px);margin:12px 0 16px;word-break:break-word;}
.pad{display:grid;grid-template-columns:repeat(3,90px);
     grid-template-rows:repeat(3,90px);gap:12px;
     margin:0 auto;width:294px;}
.btn{background:#0f9;border:none;border-radius:16px;
     width:90px;height:90px;font-size:36px;
     color:#000;cursor:pointer;
     box-shadow:0 6px 0 #00000044;}
.btn:active{transform:translateY(5px);box-shadow:0 1px 0 #00000044;}
.section{margin:28px auto;width:min(320px,90vw);}
.section-label{font-size:14px;color:#aaa;margin-bottom:8px;}
.speed-row{display:flex;align-items:center;justify-content:center;gap:16px;}
.sb{width:72px;height:56px;font-size:32px;background:#0f9;color:#000;
    border:none;border-radius:12px;cursor:pointer;}
#pct{font-size:52px;font-weight:bold;color:#0f9;min-width:80px;}
.control-row{display:flex;justify-content:center;gap:10px;flex-wrap:wrap;}
.menu-btn{cursor:pointer;font-size:14px;color:#ccc;
          background:#222;border:1px solid #444;
          padding:8px 16px;border-radius:8px;}
.menu{display:none;margin-top:16px;}
.name-section input{
  width:100%;padding:10px 14px;font-size:18px;border-radius:10px;
  border:2px solid #333;background:#222;color:#fff;text-align:center;}
.name-section input:focus{outline:none;border-color:#0f9;}
.name-btn{
  margin-top:10px;width:100%;padding:10px;font-size:16px;
  background:#0f9;color:#000;border:none;border-radius:10px;cursor:pointer;}
.name-btn:active{opacity:.75;}
.name-ok{color:#0f9;font-size:13px;min-height:18px;margin-top:6px;}
#calibMenu{display:none;}
.calrow{display:flex;justify-content:space-around;margin-top:12px;}
.calgroup{display:flex;flex-direction:column;align-items:center;gap:6px;}
.calgroup span{font-size:12px;color:#aaa;}
.csb{width:54px;height:42px;font-size:24px;background:#f90;
     color:#000;border:none;border-radius:8px;cursor:pointer;}
#cal{font-size:14px;color:#f90;margin-top:6px;}
</style>
</head>
<body>

<h1 id="robotTitle">{{NAME}}</h1>

<div class="pad">
  <div></div>
  <button class="btn" id="btnF">▲</button>
  <div></div>

  <button class="btn" id="btnL">◀</button>
  <div></div>
  <button class="btn" id="btnR">▶</button>

  <div></div>
  <button class="btn" id="btnB">▼</button>
  <div></div>
</div>

<div class="section">
  <div class="section-label">Nopeus</div>
  <div class="speed-row">
    <button class="sb" onclick="sendCmd('sd')">–</button>
    <span id="pct">50</span><span style="font-size:28px;color:#0f9;">%</span>
    <button class="sb" onclick="sendCmd('su')">+</button>
  </div>
</div>

<div class="section">
  <div class="control-row">
    <button class="menu-btn" onclick="toggleCalib()">⚙ Kalibrointi</button>
    <button class="menu-btn" onclick="toggleName()">✎ Nimi</button>
  </div>

  <div id="calibMenu" class="menu">
    <div id="cal">V:1.00 O:1.00</div>
    <div class="calrow">
      <div class="calgroup">
        <span>Vasen</span>
        <div>
          <button class="csb" onclick="sendCmd('cld')">–</button>
          <button class="csb" onclick="sendCmd('clu')">+</button>
        </div>
      </div>
      <div class="calgroup">
        <span>Oikea</span>
        <div>
          <button class="csb" onclick="sendCmd('crd')">–</button>
          <button class="csb" onclick="sendCmd('cru')">+</button>
        </div>
      </div>
    </div>
  </div>

  <div id="nameMenu" class="menu name-section">
    <div class="section-label">Robotin nimi</div>
    <input type="text" id="nameInput" maxlength="24" placeholder="Kirjoita nimi tähän…" value="{{NAME}}">
    <button class="name-btn" onclick="saveName()">Tallenna nimi</button>
    <div class="name-ok" id="nameOk"></div>
  </div>
</div>

<script>
function sendCmd(c) {
  return fetch('/' + c, { cache: 'no-store' }).catch(() => {});
}

function bindHold(btnId, pressCmd, releaseCmd) {
  const b = document.getElementById(btnId);
  const down = (e) => {
    e.preventDefault();
    sendCmd(pressCmd);
  };
  const up = (e) => {
    e.preventDefault();
    sendCmd(releaseCmd);
  };
  b.addEventListener('pointerdown', down, { passive: false });
  b.addEventListener('pointerup', up, { passive: false });
  b.addEventListener('pointercancel', up, { passive: false });
  b.addEventListener('pointerleave', up, { passive: false });
  b.addEventListener('contextmenu', e => e.preventDefault());
}

function updateUI() {
  fetch('/g', { cache: 'no-store' }).then(r => r.text()).then(t => {
    document.getElementById('pct').textContent = t;
  }).catch(() => {});
  fetch('/gc', { cache: 'no-store' }).then(r => r.text()).then(t => {
    document.getElementById('cal').textContent = t;
  }).catch(() => {});
  fetch('/gn', { cache: 'no-store' }).then(r => r.text()).then(t => {
    if (t) {
      document.getElementById('robotTitle').textContent = t;
      document.title = t;

      const input = document.getElementById('nameInput');

      if (document.activeElement !== input) {
        input.value = t;
      }
    }
  }).catch(() => {});
}

function saveName() {
  const nameInput = document.getElementById('nameInput');
  const name = nameInput.value.trim();
  if (!name) return;

  fetch('/setname?v=' + encodeURIComponent(name), { cache: 'no-store' })
    .then(r => r.text())
    .then(t => {
      document.getElementById('robotTitle').textContent = t;
      document.title = t;
      document.getElementById('nameInput').value = t;
      const ok = document.getElementById('nameOk');
      ok.textContent = 'Nimi tallennettu! Yhdistä uudelleen uuteen verkkoon.';
      setTimeout(() => ok.textContent = '', 7000);
    }).catch(() => {});
}

function toggleCalib() {
  const c = document.getElementById('calibMenu');
  const n = document.getElementById('nameMenu');

  const open = c.style.display === 'block';
  c.style.display = open ? 'none' : 'block';
  n.style.display = 'none';
}

function toggleName() {
  const c = document.getElementById('calibMenu');
  const n = document.getElementById('nameMenu');

  const open = n.style.display === 'block';
  n.style.display = open ? 'none' : 'block';
  c.style.display = 'none';
}

window.addEventListener('load', () => {
  bindHold('btnF', 'f1', 'f0');
  bindHold('btnB', 'b1', 'b0');
  bindHold('btnL', 'l1', 'l0');
  bindHold('btnR', 'r1', 'r0');
  updateUI();
  setInterval(updateUI, 1000);
});

window.addEventListener('pagehide', () => { sendCmd('stopall'); });
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'hidden') sendCmd('stopall');
});
</script>
</body>
</html>
)rawliteral";

String buildPage() {
  String page = FPSTR(PAGE_TMPL);
  String safe = htmlEscape(robotName);
  page.replace("{{NAME}}", safe);
  return page;
}

// ═══════════════════════════════════════════════════════
//  REITIT
// ═══════════════════════════════════════════════════════

void setupRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", buildPage());
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });

  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* r) { r->redirect("/"); });
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* r) { r->redirect("/"); });
  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "Microsoft NCSI"); });
  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest* r) { r->redirect("/"); });
  server.on("/fwlink", HTTP_GET, [](AsyncWebServerRequest* r) { r->redirect("/"); });
  server.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest* r) { r->redirect("/"); });

  server.on("/f1", HTTP_GET, [](AsyncWebServerRequest* r){ handleButton(DIR_FORWARD,  true);  r->send(200, "text/plain", "OK"); });
  server.on("/f0", HTTP_GET, [](AsyncWebServerRequest* r){ handleButton(DIR_FORWARD,  false); r->send(200, "text/plain", "OK"); });
  server.on("/b1", HTTP_GET, [](AsyncWebServerRequest* r){ handleButton(DIR_BACKWARD, true);  r->send(200, "text/plain", "OK"); });
  server.on("/b0", HTTP_GET, [](AsyncWebServerRequest* r){ handleButton(DIR_BACKWARD, false); r->send(200, "text/plain", "OK"); });
  server.on("/l1", HTTP_GET, [](AsyncWebServerRequest* r){ handleButton(DIR_LEFT,     true);  r->send(200, "text/plain", "OK"); });
  server.on("/l0", HTTP_GET, [](AsyncWebServerRequest* r){ handleButton(DIR_LEFT,     false); r->send(200, "text/plain", "OK"); });
  server.on("/r1", HTTP_GET, [](AsyncWebServerRequest* r){ handleButton(DIR_RIGHT,    true);  r->send(200, "text/plain", "OK"); });
  server.on("/r0", HTTP_GET, [](AsyncWebServerRequest* r){ handleButton(DIR_RIGHT,    false); r->send(200, "text/plain", "OK"); });

  server.on("/su", HTTP_GET, [](AsyncWebServerRequest* r) {
    speedPercent = min(100, speedPercent + 10);
    speedPWM = percentToPWM(speedPercent);
    applyMotion();
    saveConfig();
    r->send(200, "text/plain", String(speedPercent));
  });

  server.on("/sd", HTTP_GET, [](AsyncWebServerRequest* r) {
    speedPercent = max(10, speedPercent - 10);
    speedPWM = percentToPWM(speedPercent);
    applyMotion();
    saveConfig();
    r->send(200, "text/plain", String(speedPercent));
  });

  server.on("/g", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", String(speedPercent));
  });

  server.on("/clu", HTTP_GET, [](AsyncWebServerRequest* r) {
    leftCalib = constrain(leftCalib + 0.01f, 0.5f, 1.0f);
    applyMotion();
    saveConfig();
    r->send(200, "text/plain", "OK");
  });

  server.on("/cld", HTTP_GET, [](AsyncWebServerRequest* r) {
    leftCalib = constrain(leftCalib - 0.01f, 0.5f, 1.0f);
    applyMotion();
    saveConfig();
    r->send(200, "text/plain", "OK");
  });

  server.on("/cru", HTTP_GET, [](AsyncWebServerRequest* r) {
    rightCalib = constrain(rightCalib + 0.01f, 0.5f, 1.0f);
    applyMotion();
    saveConfig();
    r->send(200, "text/plain", "OK");
  });

  server.on("/crd", HTTP_GET, [](AsyncWebServerRequest* r) {
    rightCalib = constrain(rightCalib - 0.01f, 0.5f, 1.0f);
    applyMotion();
    saveConfig();
    r->send(200, "text/plain", "OK");
  });

  server.on("/gc", HTTP_GET, [](AsyncWebServerRequest* r) {
    String s = "V:" + String(leftCalib, 2) + "  O:" + String(rightCalib, 2);
    r->send(200, "text/plain", s);
  });

  server.on("/setname", HTTP_GET, [](AsyncWebServerRequest* r) {
    String reply = robotName;

    if (r->hasParam("v")) {
      String newName = r->getParam("v")->value();
      newName.trim();
      if (!newName.isEmpty()) {
        robotName = newName;
        saveConfig();
        reply = robotName;
        apRestartPending = true;
        apRestartAt = millis();
      }
    }

    r->send(200, "text/plain", reply);
  });

  server.on("/gn", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", robotName);
  });

  server.on("/stopall", HTTP_GET, [](AsyncWebServerRequest* r) {
    pressedCount = 0;
    for (uint8_t i = 0; i < 4; ++i) pressedOrder[i] = DIR_NONE;
    motorStop();
    r->send(200, "text/plain", "OK");
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("/");
  });
}

// ═══════════════════════════════════════════════════════
//  SETUP - Tämä suoritetaan vain kerran käynnistyessä
// ═══════════════════════════════════════════════════════

void setup() {
  pinMode(IN1, OUTPUT); digitalWrite(IN1, LOW);
  pinMode(IN2, OUTPUT); digitalWrite(IN2, LOW);
  pinMode(IN3, OUTPUT); digitalWrite(IN3, LOW);
  pinMode(IN4, OUTPUT); digitalWrite(IN4, LOW);

  motorStop();

  pinMode(FRONT_TRIG, OUTPUT);
  pinMode(FRONT_ECHO, INPUT);
  pinMode(BACK_TRIG, OUTPUT);
  pinMode(BACK_ECHO, INPUT);

  analogWriteRange(255);
  analogWriteFreq(1000);

  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }

  loadConfig();
  speedPWM = percentToPWM(speedPercent);

  startAccessPoint();
  setupRoutes();
  server.begin();
}

// ═══════════════════════════════════════════════════════
//  LOOP - Tätä suoritetaan jatkuvasti
// ═══════════════════════════════════════════════════════

void loop() {
  unsigned long now = millis();

  dnsServer.processNextRequest();

  if (apRestartPending && (now - apRestartAt >= AP_RESTART_DELAY_MS)) {
    apRestartPending = false;
    startAccessPoint();
  }

  if (now - lastSensorMs >= SENSOR_INTERVAL_MS) {
    lastSensorMs = now;

    if (measureFront) {
      frontCm = readDistance(FRONT_TRIG, FRONT_ECHO);
    } else {
      backCm = readDistance(BACK_TRIG, BACK_ECHO);
    }
    measureFront = !measureFront;
    applyMotion();
  }

  yield();
}
