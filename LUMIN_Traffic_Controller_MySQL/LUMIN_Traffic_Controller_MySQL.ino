// ═══════════════════════════════════════════════════════════
// LUMIN Traffic Controller v5.0 — Local MySQL Edition
//
// Wiring:
//   GPIO 23 → RED    LED (+ 220Ω → GND)
//   GPIO 22 → YELLOW LED (+ 220Ω → GND)
//   GPIO 21 → GREEN  LED (+ 220Ω → GND)
//   GPIO 2  → Onboard status LED (built-in)
//
// This version uses a local MySQL database via REST API instead of Firebase.
// The ESP32 makes HTTP requests to your local PHP API.
//
// Required libraries (install via Arduino Library Manager):
//   • ArduinoJson v6.x
//
// Serial commands (115200 baud):
//   ENABLE | DISABLE | AUTO | RED | YELLOW | GREEN | STATUS
// ═══════════════════════════════════════════════════════════

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ──────────────────────────────────────────────────────────
// FORWARD DECLARATIONS
// ──────────────────────────────────────────────────────────
void pushStateToDB(bool force = false);
void logLightChange(const String& light);
void autoStep();
void fetchConfig();
void sendHeartbeat();
void allLightsOff();

// ──────────────────────────────────────────────────────────
// CONFIGURATION — update these!
// ──────────────────────────────────────────────────────────
const char* WIFI_SSID      = "Chichi";
const char* WIFI_PASSWORD  = "12345678";

// MySQL API endpoint (your local XAMPP server)
// Format: "http://YOUR_SERVER_IP/Bunny_Trafficlight/api.php"
// Use your computer's local IP if ESP32 and XAMPP are on same network
const char* API_BASE_URL   = "http://10.26.133.184/Bunny_Trafficlight/api.php";

// ──────────────────────────────────────────────────────────
// PINS
// ──────────────────────────────────────────────────────────
#define PIN_RED     23
#define PIN_YELLOW  22
#define PIN_GREEN   21
#define PIN_STATUS   2   // onboard LED — HIGH = WiFi connected

// ──────────────────────────────────────────────────────────
// LIGHT DURATIONS (seconds) — must match dashboard DUR object
// ──────────────────────────────────────────────────────────
#define DUR_RED     30
#define DUR_YELLOW   5
#define DUR_GREEN   25

// ──────────────────────────────────────────────────────────
// TIMING INTERVALS (milliseconds)
// ──────────────────────────────────────────────────────────
#define INTERVAL_COUNTDOWN   1000    // 1 s tick — DO NOT CHANGE
#define INTERVAL_FETCH       1000    // poll DB for dashboard changes
#define INTERVAL_HEARTBEAT  10000    // keep device "online" in DB
#define INTERVAL_WIFI_RETRY  4000    // reconnect attempt

// ──────────────────────────────────────────────────────────
// LIGHT STATE ENUM
// ──────────────────────────────────────────────────────────
enum LightState { LIGHT_OFF, LIGHT_RED, LIGHT_YELLOW, LIGHT_GREEN };

LightState strToLight(const String& s) {
  if (s == "red")    return LIGHT_RED;
  if (s == "yellow") return LIGHT_YELLOW;
  if (s == "green")  return LIGHT_GREEN;
  return LIGHT_OFF;
}

String lightToStr(LightState l) {
  switch (l) {
    case LIGHT_RED:    return "red";
    case LIGHT_YELLOW: return "yellow";
    case LIGHT_GREEN:  return "green";
    default:           return "off";
  }
}

const int DURATIONS[] = { 0, DUR_RED, DUR_YELLOW, DUR_GREEN };

int durationFor(LightState l) {
  return DURATIONS[(int)l];
}

// ──────────────────────────────────────────────────────────
// RUNTIME STATE
// ──────────────────────────────────────────────────────────
LightState currentLight  = LIGHT_OFF;
int        remaining     = 0;
bool       enabled       = false;
String     mode          = "auto";
String     manualLight   = "red";

bool   prev_enabled     = false;
String prev_mode        = "auto";
String prev_manualLight = "red";

bool wifiOK    = false;
bool httpOK    = false;
bool httpBusy  = false;

unsigned long tCountdown = 0;
unsigned long tFetch     = 0;
unsigned long tHeartbeat = 0;
unsigned long tWifiRetry = 0;

// ──────────────────────────────────────────────────────────
// HARDWARE CONTROL
// ──────────────────────────────────────────────────────────
void allLightsOff() {
  digitalWrite(PIN_RED,    LOW);
  digitalWrite(PIN_YELLOW, LOW);
  digitalWrite(PIN_GREEN,  LOW);
}

void applyHardware(LightState l) {
  allLightsOff();
  switch (l) {
    case LIGHT_RED:    digitalWrite(PIN_RED,    HIGH); break;
    case LIGHT_YELLOW: digitalWrite(PIN_YELLOW, HIGH); break;
    case LIGHT_GREEN:  digitalWrite(PIN_GREEN,  HIGH); break;
    default: break;
  }
}

// ──────────────────────────────────────────────────────────
// SET LIGHT — single authority for all light changes
// ──────────────────────────────────────────────────────────
void setLight(LightState l) {
  String prev = lightToStr(currentLight);
  currentLight = l;
  remaining    = durationFor(l);

  applyHardware(l);

  String cur = lightToStr(l);
  String upper = cur; upper.toUpperCase();
  Serial.printf("[LIGHT] %s -> %s (%ds)\n", prev.c_str(), upper.c_str(), remaining);

  pushStateToDB(true);
  logLightChange(cur);
}

// ──────────────────────────────────────────────────────────
// AUTO CYCLE: red → green → yellow → red → …
// ──────────────────────────────────────────────────────────
void autoStep() {
  switch (currentLight) {
    case LIGHT_RED:    setLight(LIGHT_GREEN);  break;
    case LIGHT_GREEN:  setLight(LIGHT_YELLOW); break;
    case LIGHT_YELLOW: setLight(LIGHT_RED);    break;
    default:           setLight(LIGHT_RED);    break;
  }
}

// ──────────────────────────────────────────────────────────
// HTTP HELPERS
// ──────────────────────────────────────────────────────────
String httpGet(const char* url) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  String payload = (code == 200) ? http.getString() : "";
  http.end();
  return payload;
}

String httpPost(const char* url, const String& jsonBody) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(jsonBody);
  String payload = (code == 200) ? http.getString() : "";
  http.end();
  return payload;
}

// ──────────────────────────────────────────────────────────
// PUSH STATE TO MYSQL (via API)
// force=true → write immediately
// force=false → rate-limited to ~1 Hz
// ──────────────────────────────────────────────────────────
void pushStateToDB(bool force) {
  if (!wifiOK || httpBusy) return;

  static unsigned long lastWrite = 0;
  unsigned long now = millis();
  if (!force && (now - lastWrite < 950)) return;
  lastWrite = now;

  httpBusy = true;

  String url = String(API_BASE_URL) + "?action=update";

  StaticJsonDocument<512> doc;
  // Note: enabled is NOT pushed - dashboard controls this
  doc["mode"]           = mode;
  doc["current_light"]  = lightToStr(currentLight);
  doc["remaining_time"] = remaining;
  doc["updated_at"]     = (int)millis();
  doc["updated_by"]     = "device";

  String body;
  serializeJson(doc, body);

  String response = httpPost(url.c_str(), body);

  if (response.length() > 0) {
    StaticJsonDocument<256> resp;
    DeserializationError err = deserializeJson(resp, response);
    if (err || !resp["success"]) {
      Serial.printf("[API UPDATE] Error parsing response\n");
      httpOK = false;
    } else {
      httpOK = true;
    }
  } else {
    Serial.printf("[API UPDATE] No response\n");
    httpOK = false;
  }

  httpBusy = false;
}

// ──────────────────────────────────────────────────────────
// LOG LIGHT CHANGE (via API)
// ──────────────────────────────────────────────────────────
void logLightChange(const String& light) {
  if (!wifiOK || httpBusy) return;

  httpBusy = true;

  String url = String(API_BASE_URL) + "?action=log";

  StaticJsonDocument<256> doc;
  doc["light"]      = light;
  doc["mode"]       = mode;
  doc["source"]     = "device";
  doc["created_at"] = (int)millis();

  String body;
  serializeJson(doc, body);

  httpPost(url.c_str(), body);

  httpBusy = false;
}

// ──────────────────────────────────────────────────────────
// HEARTBEAT — marks device as online (via API)
// ──────────────────────────────────────────────────────────
void sendHeartbeat() {
  if (!wifiOK || httpBusy) return;

  httpBusy = true;

  String url = String(API_BASE_URL) + "?action=heartbeat";

  StaticJsonDocument<256> doc;
  doc["online"]    = true;
  doc["last_seen"] = (int)millis();

  String body;
  serializeJson(doc, body);

  String response = httpPost(url.c_str(), body);

  if (response.length() > 0) {
    StaticJsonDocument<256> resp;
    DeserializationError err = deserializeJson(resp, response);
    if (!err && resp["success"]) {
      Serial.println("[HEARTBEAT] OK");
      httpOK = true;
    } else {
      httpOK = false;
    }
  } else {
    httpOK = false;
  }

  httpBusy = false;
}

// ──────────────────────────────────────────────────────────
// FETCH CONFIG (via API)
// Reads device_config from MySQL and reacts only on changes.
// ESP32 NEVER reads remaining_time — it owns timing.
// ──────────────────────────────────────────────────────────
void fetchConfig() {
  if (!wifiOK || httpBusy) return;

  httpBusy = true;

  String url = String(API_BASE_URL) + "?action=config";
  String response = httpGet(url.c_str());

  Serial.printf("[FETCH] RAW response: %s\n", response.substring(0, 200).c_str());

  if (response.length() == 0) {
    Serial.printf("[FETCH] No response\n");
    httpBusy = false;
    httpOK = false;
    return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, response);

  if (err) {
    Serial.printf("[FETCH] JSON parse error\n");
    httpBusy = false;
    httpOK = false;
    return;
  }

  if (!doc["success"] || !doc["data"]) {
    Serial.printf("[FETCH] API error\n");
    httpBusy = false;
    httpOK = false;
    return;
  }

  JsonObject data = doc["data"];

  // 26a0Fe0f MySQL BOOLEAN = TINYINT (0/1). ArduinoJson pipe op does NOT cast int2192bool.
  // data["enabled"] | false ALWAYS returns false when value is 1.
  // Use as<int>() != 0 to correctly read 0/1 as false/true.
  bool   new_enabled     = data["enabled"].as<int>() != 0;
  String new_mode        = data["mode"]        | "auto";
  String new_manualLight = data["manual_light"]| "red";

  // DEBUG — print raw enabled value from JSON (int) and parsed bool
  Serial.printf("[FETCH] RAW enabled=%d (int from JSON)\n", data["enabled"].as<int>());
  // DEBUG output
  Serial.printf("[FETCH] enabled=%d mode=%s manual=%s | prev: enabled=%d mode=%s manual=%s\n",
    new_enabled, new_mode.c_str(), new_manualLight.c_str(),
    prev_enabled, prev_mode.c_str(), prev_manualLight.c_str());

  httpBusy = false;
  httpOK = true;

  // ── 1. enabled changed ────────────────────────────────
  if (new_enabled != prev_enabled) {
    enabled          = new_enabled;
    prev_enabled     = new_enabled;  // UPDATE prev first to prevent race
    prev_mode        = new_mode;
    prev_manualLight = new_manualLight;
    mode             = new_mode;
    manualLight      = new_manualLight;

    Serial.printf("[CONFIG] enabled -> %s\n", enabled ? "true" : "false");

    if (!enabled) {
      allLightsOff();
      currentLight = LIGHT_OFF;
      remaining    = 0;
      pushStateToDB(true);
    } else {
      if (mode == "manual") {
        setLight(strToLight(manualLight));
      } else {
        setLight(LIGHT_RED);
      }
    }
    return;
  }

  if (!enabled) return;

  // ── 2. mode changed ───────────────────────────────────
  if (new_mode != prev_mode) {
    prev_mode        = new_mode;
    prev_manualLight = new_manualLight;
    mode             = new_mode;
    manualLight      = new_manualLight;
    Serial.printf("[CONFIG] mode -> %s\n", mode.c_str());

    if (mode == "manual") {
      setLight(strToLight(manualLight));
    } else {
      setLight(LIGHT_RED);
    }
    return;
  }

  // ── 3. manual_light changed (while in manual mode) ────
  if (mode == "manual" && new_manualLight != prev_manualLight) {
    prev_manualLight = new_manualLight;
    manualLight      = new_manualLight;
    Serial.printf("[CONFIG] manual_light -> %s\n", manualLight.c_str());
    setLight(strToLight(manualLight));
    return;
  }
}

// ──────────────────────────────────────────────────────────
// COUNTDOWN TICK — 1 s clock, ESP32 is sole authority
// ──────────────────────────────────────────────────────────
void tickCountdown() {
  if (!enabled || currentLight == LIGHT_OFF) return;

  if (remaining > 0) remaining--;

  Serial.printf("[TICK] light=%s remaining=%d mode=%s\n",
    lightToStr(currentLight).c_str(), remaining, mode.c_str());

  pushStateToDB(false);

  if (mode == "auto" && remaining <= 0) {
    autoStep();
  }
}

// ──────────────────────────────────────────────────────────
// SERIAL COMMAND HANDLER — DISABLED (dashboard-only control)
// ──────────────────────────────────────────────────────────
void handleSerial(const String& raw) {
  // Serial commands disabled - control via dashboard only
  // Available for debug: STATUS command
  String cmd = raw;
  cmd.trim();
  cmd.toUpperCase();
  
  if (cmd == "STATUS") {
    Serial.printf(
      "[STATUS] light=%-6s rem=%ds mode=%s wifi=%s http=%s enabled=%s\n",
      lightToStr(currentLight).c_str(),
      remaining,
      mode.c_str(),
      wifiOK ? "OK" : "NO",
      httpOK ? "OK" : "NO",
      enabled ? "YES" : "NO"
    );
  }
  // All other commands ignored - use dashboard
}

// ──────────────────────────────────────────────────────────
// WIFI
// ──────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true;
    digitalWrite(PIN_STATUS, HIGH);
    Serial.printf(" OK — IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    wifiOK = false;
    Serial.println(" FAILED (will retry in loop)");
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiOK) {
      wifiOK = true;
      digitalWrite(PIN_STATUS, HIGH);
      Serial.println("[WIFI] Reconnected");
      sendHeartbeat();
    }
    return;
  }

  if (wifiOK) {
    wifiOK = false;
    httpOK = false;
    digitalWrite(PIN_STATUS, LOW);
    Serial.println("[WIFI] Lost connection");
  }

  unsigned long now = millis();
  if (now - tWifiRetry >= INTERVAL_WIFI_RETRY) {
    tWifiRetry = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("[WIFI] Retrying...");
  }
}

// ──────────────────────────────────────────────────────────
// SETUP
// ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  Serial.println();
  Serial.println("+===========================================+");
  Serial.println("|   LUMIN Traffic Controller v5.0           |");
  Serial.println("|   ESP32 + Local MySQL + Dashboard         |");
  Serial.println("+===========================================+");
  Serial.printf("[CONFIG] API: %s\n", API_BASE_URL);
  Serial.printf("[CONFIG] R=%ds  Y=%ds  G=%ds\n", DUR_RED, DUR_YELLOW, DUR_GREEN);

  pinMode(PIN_RED,    OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_GREEN,  OUTPUT);
  pinMode(PIN_STATUS, OUTPUT);
  allLightsOff();
  digitalWrite(PIN_STATUS, LOW);

  connectWiFi();

  if (wifiOK) {
    sendHeartbeat();
    fetchConfig();
  }

  unsigned long now = millis();
  tCountdown = now + 500;
  tFetch     = now;
  tHeartbeat = now + 3000;

  Serial.println("[BOOT] Ready — entering loop");
  Serial.println("[BOOT] Control via Dashboard only (Serial: STATUS for debug)");
}

// ──────────────────────────────────────────────────────────
// LOOP — millis()-based, no blocking delay()
// ──────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // 1. Serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleSerial(cmd);
  }

  // 2. WiFi watchdog
  maintainWiFi();

  // 3. 1-second countdown tick
  if (now - tCountdown >= INTERVAL_COUNTDOWN) {
    tCountdown = now;
    tickCountdown();
  }

  // 4. Config fetch
  if (now - tFetch >= INTERVAL_FETCH) {
    tFetch = now;
    if (!httpBusy) fetchConfig();
  }

  // 5. Heartbeat
  if (now - tHeartbeat >= INTERVAL_HEARTBEAT) {
    tHeartbeat = now;
    if (!httpBusy) sendHeartbeat();
  }
}
