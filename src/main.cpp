#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

#ifndef WIFI_SSID
#  error "WIFI_SSID not set — copy .env.example to .env and fill it in"
#endif
#ifndef WIFI_PASSWORD
#  error "WIFI_PASSWORD not set"
#endif
#ifndef APP_PIN
#  error "APP_PIN not set"
#endif

// ─── Hardware config ──────────────────────────────────────────────────────────
#define BUTTON_PIN      13
#define PRESS_HOLD_MS   1000
#define COOLDOWN_MS     20000

#define HOSTNAME        "garage"
#define WDT_TIMEOUT_S    10     // reboot if loop() stalls for this long
#define WIFI_CHECK_MS    5000   // how often to check WiFi is still connected

// SSE clients (the PWA's live status feed) can die silently — phone sleeps,
// WiFi roams, app gets backgrounded — without ever sending a TCP FIN. Since
// SSE is server→client only, ESPAsyncWebServer disables RX timeout on these
// sockets, so a dead one sits open forever, slowly eating the small (~10)
// lwIP socket pool until the server can't accept new connections at all.
// TCP keepalive lets lwIP notice and reap them on its own.
#define SSE_KEEPALIVE_MS     15000  // probe every 15s
#define SSE_KEEPALIVE_COUNT  3      // give up (close) after 3 missed probes

// Safety net: periodically reboot when idle, in case something still leaks.
#define UPTIME_RESTART_MS  (3UL * 24 * 60 * 60 * 1000UL)  // 3 days
// ─────────────────────────────────────────────────────────────────────────────

static bool          g_pressing        = false;
static bool          g_doorOpen        = false;
static unsigned long g_cooldownUntil   = 0;
static bool          g_pressPending    = false;
static unsigned long g_lastSensorMs    = 0;
#define SENSOR_TIMEOUT_MS 15000  // sensor considered offline after 15s with no update

AsyncWebServer   server(2582);
AsyncEventSource events("/api/events");

// ─── Helpers ──────────────────────────────────────────────────────────────────

String extractField(const String& json, const char* key) {
  String search = String("\"") + key + "\"";
  int pos = json.indexOf(search);
  if (pos < 0) return "";
  pos = json.indexOf(':', pos) + 1;
  while (pos < (int)json.length() && json[pos] == ' ') pos++;
  if (json[pos] != '"') return "";
  pos++;
  int end = json.indexOf('"', pos);
  if (end < 0) return "";
  return json.substring(pos, end);
}

bool safeEquals(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); i++)
    diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return diff == 0;
}

String stateJson() {
  bool inCooldown = millis() < g_cooldownUntil;
  unsigned long remaining = inCooldown ? (g_cooldownUntil - millis()) : 0;
  bool sensorOnline = g_lastSensorMs > 0 && (millis() - g_lastSensorMs < SENSOR_TIMEOUT_MS);
  return String("{\"open\":") + (g_doorOpen ? "true" : "false") +
         ",\"cooldown\":" + (inCooldown ? "true" : "false") +
         ",\"remaining\":" + remaining +
         ",\"sensorOnline\":" + (sensorOnline ? "true" : "false") + "}";
}

void broadcastState() {
  events.send(stateJson().c_str(), "state", millis());
}

void pressButton() {
  if (g_pressing) return;
  g_pressing = true;
  digitalWrite(BUTTON_PIN, HIGH);
  delay(PRESS_HOLD_MS);
  digitalWrite(BUTTON_PIN, LOW);
  g_pressing = false;
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed");
    return;
  }

  pinMode(BUTTON_PIN, OUTPUT);
  digitalWrite(BUTTON_PIN, LOW);

  WiFi.setSleep(false);  // keep radio awake — avoids modem-sleep wake latency on first request
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\n[WiFi] Connected: http://%s.local  (%s)\n",
                HOSTNAME, WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());

  if (MDNS.begin(HOSTNAME))
    Serial.printf("[mDNS] http://%s.local\n", HOSTNAME);

  // Static assets
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(SPIFFS, "/index.html", "text/html");
  });
  server.on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(SPIFFS, "/manifest.json", "application/json");
  });
  server.on("/sw.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* resp = r->beginResponse(SPIFFS, "/sw.js", "application/javascript");
    resp->addHeader("Service-Worker-Allowed", "/");
    r->send(resp);
  });
  server.on("/icon.svg", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(SPIFFS, "/icon.svg", "image/svg+xml");
  });

  // GET /api/status — kept for fallback
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", stateJson());
  });

  // SSE — send current state to each new client on connect
  events.onConnect([](AsyncEventSourceClient* client) {
    client->client()->setKeepAlive(SSE_KEEPALIVE_MS, SSE_KEEPALIVE_COUNT);
    client->send(stateJson().c_str(), "state", millis(), 1000);
  });
  server.addHandler(&events);

  // POST /api/sensor — called by sensor ESP32 with {"open":bool,"distance_cm":float}
  server.on("/api/sensor", HTTP_POST,
    [](AsyncWebServerRequest*) {},
    nullptr,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
      String body = String((char*)data, len);
      bool sensorOpen = body.indexOf("\"open\":true") >= 0;
      g_lastSensorMs = millis();
      if (sensorOpen != g_doorOpen) {
        g_doorOpen = sensorOpen;
        Serial.printf("[Sensor] State updated → %s\n", g_doorOpen ? "open" : "closed");
      }
      broadcastState();
      r->send(200, "application/json", "{\"ok\":true}");
    }
  );

  // POST /api/press
  server.on("/api/press", HTTP_POST,
    [](AsyncWebServerRequest*) {},
    nullptr,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
      String body = String((char*)data, len);
      String pin  = extractField(body, "password");

      if (!safeEquals(pin, String(APP_PIN))) {
        r->send(401, "application/json", "{\"error\":\"wrong pin\"}");
        Serial.println("[Press] Wrong PIN");
        return;
      }
      if (g_pressing || millis() < g_cooldownUntil) {
        r->send(429, "application/json", "{\"error\":\"busy\"}");
        return;
      }

      g_pressPending = true;
      r->send(200, "application/json", "{\"ok\":true}");
    }
  );

  server.onNotFound([](AsyncWebServerRequest* r) {
    r->send(404, "text/plain", "Not found");
  });

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready");

  server.begin();
  Serial.println("[Server] Ready");

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdtConfig);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
  Serial.println("[WDT] Watchdog armed");
}

void ensureWifiConnected() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < WIFI_CHECK_MS) return;
  lastCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected — reconnecting");
    WiFi.disconnect();
    WiFi.reconnect();

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      esp_task_wdt_reset();
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WiFi] Reconnected: %s\n", WiFi.localIP().toString().c_str());
      MDNS.end();
      if (MDNS.begin(HOSTNAME))
        Serial.printf("[mDNS] http://%s.local\n", HOSTNAME);
    } else {
      Serial.println("[WiFi] Reconnect attempt failed, will retry");
    }
  }
}

void loop() {
  esp_task_wdt_reset();
  ArduinoOTA.handle();
  ensureWifiConnected();

  static bool          wasCooling    = false;
  static unsigned long lastBroadcast = 0;

  if (g_pressPending) {
    g_pressPending = false;
    Serial.println("[Press] Activating button");
    g_cooldownUntil = millis() + COOLDOWN_MS;
    broadcastState();
    pressButton();
  }

  bool isCooling = millis() < g_cooldownUntil;
  if (isCooling && millis() - lastBroadcast >= 1000) {
    broadcastState();
    lastBroadcast = millis();
  }
  if (wasCooling && !isCooling) {
    broadcastState();
    lastBroadcast = millis();
  }
  wasCooling = isCooling;

  // Self-heal: reboot after long uptime, but only when idle so we never
  // interrupt a press/cooldown in progress.
  if (millis() > UPTIME_RESTART_MS && !g_pressing && !isCooling) {
    Serial.println("[Self-heal] Scheduled restart after long uptime");
    delay(100);
    ESP.restart();
  }
}
