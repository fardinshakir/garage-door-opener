#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>

#ifndef WIFI_SSID
#  error "WIFI_SSID not set — copy .env.example to .env and fill it in"
#endif
#ifndef WIFI_PASSWORD
#  error "WIFI_PASSWORD not set"
#endif

// ─── Hardware config ──────────────────────────────────────────────────────────
#define TRIG_PIN             13
#define ECHO_PIN             12

// Ceiling-mounted sensor: door panels roll up close to ceiling when open.
// Tune this after mounting. Anything below this threshold → door is open.
#define DOOR_OPEN_MAX_CM     40

#define MEASURE_INTERVAL_MS  500
#define DEBOUNCE_COUNT       3     // consecutive agreeing readings before state change
#define HEARTBEAT_MS         5000  // re-report even when state unchanged

#define MAIN_ESP_URL         "http://garage.local:2582/api/sensor"
// ─────────────────────────────────────────────────────────────────────────────

static bool          g_doorOpen  = false;
static int           g_sameCount = 0;
static unsigned long g_lastReport = 0;

float measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  // 30 000 µs timeout ≈ 5 m max range
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1.0f;
  return duration * 0.034f / 2.0f;
}

static int g_wifiFailCount = 0;

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) { g_wifiFailCount = 0; return; }
  Serial.print("[WiFi] Reconnecting");
  // After 3 failed attempts, do a full reconnect instead of just reconnect()
  if (g_wifiFailCount >= 3) {
    Serial.print(" (full restart)");
    WiFi.disconnect();
    delay(1000);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
    WiFi.reconnect();
  }
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    g_wifiFailCount = 0;
    Serial.println(" OK");
  } else {
    g_wifiFailCount++;
    Serial.printf(" Failed (%d)\n", g_wifiFailCount);
  }
}

void reportState(bool open, float dist) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(MAIN_ESP_URL);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"open\":") + (open ? "true" : "false") +
                ",\"distance_cm\":" + String(dist, 1) + "}";
  int code = http.POST(body);
  http.end();
  Serial.printf("[Sensor] POST %s → HTTP %d\n", body.c_str(), code);
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\n[WiFi] Connected: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  ArduinoOTA.setHostname("garage-sensor");
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready");

  // Take initial reading and report so main ESP has state from boot
  delay(100);
  float dist = measureDistance();
  if (dist > 0) {
    g_doorOpen = (dist > DOOR_OPEN_MAX_CM);
    reportState(g_doorOpen, dist);
    g_lastReport = millis();
    Serial.printf("[Sensor] Initial: %.1f cm → %s\n", dist, g_doorOpen ? "open" : "closed");
  }
}

void loop() {
  ArduinoOTA.handle();

  static unsigned long lastRssi = 0;
  if (millis() - lastRssi >= 30000) {
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
    lastRssi = millis();
  }

  static unsigned long lastMeasure = 0;
  if (millis() - lastMeasure < MEASURE_INTERVAL_MS) return;
  lastMeasure = millis();

  float dist = measureDistance();
  if (dist < 0) {
    Serial.println("[Sensor] No echo (out of range or wiring issue)");
    return;
  }

  bool reading = (dist > DOOR_OPEN_MAX_CM);
  Serial.printf("[Sensor] %.1f cm → %s\n", dist, reading ? "open" : "closed");

  if (reading != g_doorOpen) {
    // State appears to have changed — debounce before committing
    g_sameCount++;
    if (g_sameCount >= DEBOUNCE_COUNT) {
      g_doorOpen  = reading;
      g_sameCount = 0;
      reportState(g_doorOpen, dist);
      g_lastReport = millis();
    }
  } else {
    g_sameCount = 0;
    // Periodic heartbeat so main ESP can detect sensor going offline
    if (millis() - g_lastReport >= HEARTBEAT_MS) {
      reportState(g_doorOpen, dist);
      g_lastReport = millis();
    }
  }
}
