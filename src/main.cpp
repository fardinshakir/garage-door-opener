#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ESP32Servo.h>
#include <ESPmDNS.h>

// Values injected from .env at build time
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
#define SERVO_PIN       13
#define SERVO_REST_DEG  0    // Resting angle — arm clear of button
#define SERVO_PRESS_DEG 60   // Pressing angle — tune this to your mount
#define PRESS_HOLD_MS   400  // How long to hold the button (ms)

#define HOSTNAME        "garage"   // http://garage.local
// ─────────────────────────────────────────────────────────────────────────────

static bool g_pressing = false;
static bool g_doorOpen = false;   // software-toggled; add a reed switch for real state

AsyncWebServer server(80);
Servo          servo;

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

void pressButton() {
  if (g_pressing) return;
  g_pressing = true;
  servo.write(SERVO_PRESS_DEG);
  delay(PRESS_HOLD_MS);
  servo.write(SERVO_REST_DEG);
  g_doorOpen = !g_doorOpen;
  g_pressing = false;
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed");
    return;
  }

  servo.attach(SERVO_PIN);
  servo.write(SERVO_REST_DEG);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\n[WiFi] Connected: http://%s.local  (%s)\n",
                HOSTNAME, WiFi.localIP().toString().c_str());

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

  // GET /api/status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    String body = g_doorOpen ? "{\"open\":true}" : "{\"open\":false}";
    r->send(200, "application/json", body);
  });

  // POST /api/press — verify PIN and fire servo
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
      if (g_pressing) {
        r->send(429, "application/json", "{\"error\":\"busy\"}");
        return;
      }

      r->send(200, "application/json", "{\"ok\":true}");
      Serial.println("[Press] Activating servo");
      pressButton();
    }
  );

  server.onNotFound([](AsyncWebServerRequest* r) {
    r->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[Server] Ready");
}

void loop() {}
