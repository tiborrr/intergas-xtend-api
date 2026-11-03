/*
 * BoilerBridge_Client — ESP32 (Arduino core)
 *
 * Libraries: WiFi, HTTPClient (built-in with ESP32 board package)
 *
 * Role: STA on boiler Wi-Fi, HTTP GET boiler API, forward response body on Serial2
 *       to BoilerBridge_Server (newline-terminated lines, max BOILER_BODY_MAX bytes).
 *
 * UART wiring to server board: this TX -> other RX, this RX <- other TX, GND common.
 * Default pins match README; change SERIAL2_RX / SERIAL2_TX if your board differs.
 */

#include <cstring>

#include <HTTPClient.h>
#include <WiFi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Create secrets.h from secrets.h.example (see README). Never commit secrets.h."
#endif

#if defined(BOILER_API_URL) && !defined(BOILER_API_BASE)
#error "BoilerBridge_Client: replace BOILER_API_URL with BOILER_API_BASE and BOILER_STATS_FIELDS (see secrets.h.example)."
#endif
#if !defined(BOILER_API_BASE)
#error "BoilerBridge_Client: secrets.h must define BOILER_API_BASE (see secrets.h.example)."
#endif

// Field ids are not secret; secrets.h may still override them.
#ifndef BOILER_STATS_FIELDS
#define BOILER_STATS_FIELDS                                                                                            \
  "7940,79b3,7921,7e2c,77c3,7e51,77d2,f9f2,7ed3,629c,6280,621d,62ed,503e,5088,5077,5041,50f2,62d1,620f,6206,8439,47e0," \
  "7e7a,7774,77de,6115,61ba,61eb,610b,6101,6117"
#endif

// --- Poll interval ---
static const unsigned long POLL_INTERVAL_MS = 15000;
static const uint16_t HTTP_TIMEOUT_MS = 5000;

// Recovery: reboot instead of hanging when the boiler AP or API goes away.
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 60000;
static const unsigned long NO_SUCCESS_RESTART_MS = 10UL * 60UL * 1000UL;

// --- Serial2 (link to BoilerBridge_Server) ---
#define SERIAL2_RX 16
#define SERIAL2_TX 17
#define SERIAL2_BAUD 115200

// Max bytes read from HTTP body; must match BoilerBridge_Server PAYLOAD_MAX
#define BOILER_BODY_MAX 4096

static unsigned long last_poll_ms = 0;
static unsigned long last_success_ms = 0;
static char body_buf[BOILER_BODY_MAX + 1];

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  Serial2.begin(SERIAL2_BAUD, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(BOILER_SSID, BOILER_PASS);

  Serial.print("Connecting to boiler Wi-Fi ");
  Serial.print(BOILER_SSID);
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nBoiler Wi-Fi not reachable, restarting");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("OK, IP: ");
  Serial.println(WiFi.localIP());

  // First HTTP poll on next loop() iteration
  last_poll_ms = millis() - POLL_INTERVAL_MS;
  last_success_ms = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - last_success_ms > NO_SUCCESS_RESTART_MS) {
    Serial.println("No successful poll for too long, restarting");
    ESP.restart();
  }
  if (now - last_poll_ms < POLL_INTERVAL_MS) {
    return;
  }
  last_poll_ms = now;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi lost, reconnecting...");
    WiFi.reconnect();
    return;
  }

  String url = String(BOILER_API_BASE) + "/api/stats/values?fields=" + BOILER_STATS_FIELDS;

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(url);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed: %d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  // A truncated body is invalid JSON the server would reject anyway; drop it.
  size_t len = payload.length();
  if (len > BOILER_BODY_MAX) {
    Serial.printf("Body too large (%u > %u bytes), dropped\n", (unsigned)len, BOILER_BODY_MAX);
    return;
  }
  memcpy(body_buf, payload.c_str(), len);
  body_buf[len] = '\0';

  // One UART line per message: drop CR/LF so pretty-printed JSON stays valid JSON.
  {
    char *w = body_buf;
    for (size_t i = 0; i < len; ++i) {
      if (body_buf[i] != '\r' && body_buf[i] != '\n') {
        *w++ = body_buf[i];
      }
    }
    *w = '\0';
    len = static_cast<size_t>(w - body_buf);
  }

  Serial2.print(body_buf);
  Serial2.print('\n');
  last_success_ms = millis();
  Serial.printf("Forwarded %u bytes on Serial2\n", (unsigned)len);
}
