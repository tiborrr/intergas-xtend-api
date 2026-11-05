/*
 * BoilerBridge_Server — ESP32 (Arduino core)
 *
 * Libraries: WiFi, WebServer, ESPmDNS (ESP32 Arduino core), ArduinoJson v6
 *
 * Role: STA on home Wi-Fi, read frames from Serial2 (from BoilerBridge_Client,
 *       see bridge_link.h), serve formatted GET /data, raw GET /data/raw and
 *       bridge health GET /status. Reachable as http://boilerbridge.local
 *       Every other GET or HEAD goes to the boiler through the client board:
 *       /xtend/ is the boiler's own /, other paths stay as they are.
 *       POST /ota/server and /ota/client install new firmware (password needed).
 *
 * Valid JSON + stats is parsed once per complete frame; HTTP handlers serve
 * pre-built snapshots only (no parse-on-read).
 *
 * UART wiring to client board: this RX <- other TX, this TX -> other RX, GND common.
 */

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#if ARDUINOJSON_VERSION_MAJOR != 6
#error "BoilerBridge_Server needs ArduinoJson v6: arduino-cli lib install ArduinoJson@6.21.5"
#endif

#include <cstdlib>
#include <cstring>

#include "bridge_link.h"
#include "xtend_field_map.gen.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Create secrets.h from secrets.h.example (see README). Never commit secrets.h."
#endif

#ifndef OTA_PASSWORD
#error "secrets.h must define OTA_PASSWORD (see secrets.h.example): the boards can't be reached over USB later."
#endif
#ifndef CONFIG_APP_ROLLBACK_ENABLE
#error "This firmware relies on the bootloader rolling back a bad update."
#endif

// --- Serial2 (link from BoilerBridge_Client) ---
#define SERIAL2_RX 16
#define SERIAL2_TX 17
#define SERIAL2_BAUD 115200

// Must match BoilerBridge_Client BOILER_BODY_MAX (a stats frame is usually ~hundreds of bytes).
#define PAYLOAD_MAX 4096

// A stats frame is compact boiler JSON. GET /data expands stats (larger JSON + JsonDocument pool).
// Bad or oversized frames are dropped; prior published_* kept until a good line arrives.
// Tune: JSON_OUT_CAPACITY / JSON_IN_CAPACITY / malloc ladder if drops persist.
static const size_t JSON_IN_CAPACITY = 8192;
// Slightly above the ~45k pool that overflowed with ~32 expanded stats in testing.
static const size_t JSON_OUT_CAPACITY = 49152;

// Wi-Fi: avoid infinite boot hang; surface status for flaky routers / 2.4 GHz issues
static const uint32_t WIFI_STARTUP_ROUND_TIMEOUT_MS = 45000;
static const int WIFI_STARTUP_MAX_ROUNDS = 5;
static const unsigned long WIFI_LOOP_RECONNECT_INTERVAL_MS = 30000;
// Offline this long in loop() -> reboot (clears stuck Wi-Fi driver states).
static const unsigned long WIFI_OFFLINE_RESTART_MS = 10UL * 60UL * 1000UL;

// Client polls every 15 s; older data is served as 503 so consumers never log
// a frozen value as if it were live.
static const unsigned long DATA_STALE_MS = 120000;

// Passthrough. The client may first finish its 5 s stats poll and then needs up
// to 5 s to connect; its own limit for a whole request is 90 s.
static const unsigned long PASS_HEAD_TIMEOUT_MS = 15000;
// Longer than the client's PASS_ACK_TIMEOUT_MS.
static const unsigned long PASS_CHUNK_TIMEOUT_MS = 30000;
static const unsigned long PASS_REQUEST_MAX_MS = 100000;

static const char *OTA_USER = "ota";
// Writing a chunk can include erasing flash on the client board.
static const unsigned long OTA_CHUNK_TIMEOUT_MS = 20000;
static const unsigned long OTA_FINISH_TIMEOUT_MS = 30000;
// New firmware must stay on the home network this long before it is kept, and
// reverts itself when it doesn't get there at all.
static const unsigned long OTA_HEALTHY_AFTER_MS = 60000;
static const unsigned long OTA_PROVE_HEALTHY_MS = 5UL * 60UL * 1000UL;

static const char *MDNS_HOSTNAME = "boilerbridge";

// int16 0x7FFF is the Xtend's "not available" marker (e.g. setpoints when idle).
static const long XTEND_INT16_NOT_AVAILABLE = 32767;

WebServer server(80);

static unsigned long s_lastFrameMs = 0;
static uint32_t s_framesOk = 0;
static uint32_t s_framesBad = 0;
static uint32_t s_passRequests = 0;
static uint32_t s_passFailed = 0;
static bool s_otaOk = false;
static bool s_proven_healthy = false;
static String s_otaError;

// Tells the Arduino core not to accept new firmware at boot: see checkFirmwareHealth().
extern "C" bool verifyRollbackLater() { return true; }

static bool pendingVerify() {
  esp_ota_img_states_t state;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}
static unsigned long s_wifiDownSinceMs = 0;

static const char *wifiStatusStr(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:
      return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:
      return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:
      return "WL_CONNECTED";
    case WL_CONNECT_FAILED:
      return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "WL_DISCONNECTED";
    case WL_NO_SHIELD:
      return "WL_NO_SHIELD";
    default:
      return "WL_UNKNOWN";
  }
}

static bool waitWifiConnected(uint32_t timeout_ms) {
  const uint32_t start = millis();
  uint32_t lastReport = 0;
  while (WiFi.status() != WL_CONNECTED) {
    const uint32_t elapsed = millis() - start;
    if (elapsed > timeout_ms) {
      Serial.printf(
          "\n[WiFi] timeout after %u ms, status=%s (%d)\n", timeout_ms,
          wifiStatusStr(WiFi.status()), static_cast<int>(WiFi.status()));
      if (WiFi.status() == WL_NO_SSID_AVAIL) {
        Serial.println(
            "[WiFi] hint: SSID not seen — typo, AP off, hidden SSID, or not "
            "2.4GHz (ESP32 has no 5GHz Wi-Fi).");
      } else if (WiFi.status() == WL_DISCONNECTED ||
                 WiFi.status() == WL_CONNECT_FAILED) {
        Serial.println(
            "[WiFi] hint: auth failed — check HOME_PASS; or router blocked "
            "client / full DHCP pool.");
      }
      return false;
    }
    if (elapsed - lastReport >= 10000u) {
      lastReport = elapsed;
      Serial.printf(
          "\n[WiFi] still connecting… status=%s (%d) elapsed=%u ms\n",
          wifiStatusStr(WiFi.status()), static_cast<int>(WiFi.status()), elapsed);
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  return true;
}

static bool connectHomeWifiStartup() {
  WiFi.setHostname(MDNS_HOSTNAME);  // the core applies it when WiFi.mode() enables the station
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  for (int round = 0; round < WIFI_STARTUP_MAX_ROUNDS; ++round) {
    if (round > 0) {
      Serial.printf("\n[WiFi] startup retry %d/%d\n", round + 1,
                    WIFI_STARTUP_MAX_ROUNDS);
    }
    WiFi.disconnect(true, true);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(HOME_SSID, HOME_PASS);
    Serial.print("Connecting to home Wi-Fi ");
    Serial.print(HOME_SSID);
    if (waitWifiConnected(WIFI_STARTUP_ROUND_TIMEOUT_MS)) {
      return true;
    }
    delay(1500);
  }
  return false;
}

static unsigned long s_lastWifiLoopReconnectMs = 0;

// Auto-reconnect does most of the work; this only nudges it now and then.
// (A full disconnect/begin every few seconds aborts handshakes in progress.)
static void tryWifiReconnectFromLoop() {
  const unsigned long now = millis();
  if (s_wifiDownSinceMs == 0) {
    s_wifiDownSinceMs = now;
  }
  if (now - s_wifiDownSinceMs > WIFI_OFFLINE_RESTART_MS) {
    Serial.println("[WiFi] offline too long, restarting");
    ESP.restart();
  }
  if (s_lastWifiLoopReconnectMs != 0 &&
      now - s_lastWifiLoopReconnectMs < WIFI_LOOP_RECONNECT_INTERVAL_MS) {
    return;
  }
  s_lastWifiLoopReconnectMs = now;
  Serial.printf("[WiFi] loop reconnect (status=%s %d)\n",
                wifiStatusStr(WiFi.status()),
                static_cast<int>(WiFi.status()));
  WiFi.reconnect();
}

static DynamicJsonDocument docInWork(JSON_IN_CAPACITY);
static DynamicJsonDocument docOutWork(JSON_OUT_CAPACITY);

static bridge::FrameParser<PAYLOAD_MAX> s_parser;

static char published_raw[PAYLOAD_MAX + 1];
static size_t published_raw_len = 0;

static char *published_formatted = nullptr;
static size_t published_formatted_len = 0;
static size_t published_formatted_cap = 0;

// UART may prefix garbage before '{' — skip to first JSON object/array start.
static size_t jsonPayloadOffset(const char *data, size_t n) {
  size_t i = 0;
  while (i < n && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r')) {
    ++i;
  }
  if (i < n && (data[i] == '{' || data[i] == '[')) {
    return i;
  }
  for (; i < n; ++i) {
    if (data[i] == '{' || data[i] == '[') {
      return i;
    }
  }
  return n;
}

static bool tryBuildFormattedFromStats(JsonObject stats) {
  if (published_formatted == nullptr) {
    return false;
  }
  docOutWork.clear();
  JsonArray fields = docOutWork.createNestedArray("fields");

  for (JsonPair kv : stats) {
    const char *keyStr = kv.key().c_str();
    char *end = nullptr;
    unsigned long k = strtoul(keyStr, &end, 16);
    const bool key_ok = (end != keyStr && *end == '\0' && k <= 0xFFFFu);
    const uint16_t key16 = static_cast<uint16_t>(k);
    const XtendField *fd = key_ok ? xtendFieldFind(key16) : nullptr;

    JsonObject o = fields.createNestedObject();
    o["id"] = keyStr;

    JsonVariant v = kv.value();
    if (v.is<const char *>()) {
      o["raw"] = v.as<const char *>();
      if (fd != nullptr) {
        o["name"] = fd->name;
        o["type"] = fd->type;
        o["unit"] = fd->unit;
      }
    } else if (v.is<bool>()) {
      const bool b = v.as<bool>();
      o["raw"] = b;
      if (fd != nullptr) {
        o["name"] = fd->name;
        o["type"] = fd->type;
        o["unit"] = fd->unit;
        o["scaled"] = (b ? 1.0 : 0.0) * fd->factor;
      }
    } else {
      const double rawNum = v.as<double>();
      o["raw"] = rawNum;
      if (fd != nullptr) {
        o["name"] = fd->name;
        o["type"] = fd->type;
        o["unit"] = fd->unit;
        if (strcmp(fd->type, "int16") == 0 &&
            v.as<long>() == XTEND_INT16_NOT_AVAILABLE) {
          o["scaled"] = nullptr;
        } else {
          o["scaled"] = rawNum * fd->factor;
        }
      }
    }
  }

  if (docOutWork.overflowed()) {
    return false;
  }

  const size_t need = measureJson(docOutWork);
  if (need == 0 || published_formatted_cap == 0 ||
      need >= published_formatted_cap) {
    return false;
  }
  const size_t w = serializeJson(docOutWork, published_formatted,
                                 published_formatted_cap);
  if (w != need) {
    return false;
  }
  published_formatted_len = w;
  return true;
}

static void tryPublishCompleteLine(char *frame, size_t n) {
  if (n == 0 || n > PAYLOAD_MAX) {
    return;
  }
  frame[n] = '\0';

  const size_t joff = jsonPayloadOffset(frame, n);
  if (joff >= n) {
    return;
  }
  if (joff > 0) {
    memmove(frame, frame + joff, n - joff + 1);
    n -= joff;
  }

  // Parse from const char* (copy mode). With a plain char* ArduinoJson uses
  // zero-copy mode and rewrites the buffer in place, which used to corrupt
  // the /data/raw copy taken from it below.
  docInWork.clear();
  if (deserializeJson(docInWork, static_cast<const char *>(frame), n)) {
    ++s_framesBad;
    return;
  }

  JsonVariant statsVar = docInWork["stats"];
  if (!statsVar.is<JsonObject>()) {
    ++s_framesBad;
    return;
  }

  JsonObject stats = statsVar.as<JsonObject>();
  if (!tryBuildFormattedFromStats(stats)) {
    ++s_framesBad;
    return;
  }

  memcpy(published_raw, frame, n);
  published_raw[n] = '\0';
  published_raw_len = n;
  s_lastFrameMs = millis();
  ++s_framesOk;
}

static void sendFrame(uint8_t type, uint8_t id, const void *payload, size_t len) {
  bridge::writeFrame([](const uint8_t *data, size_t n) { Serial2.write(data, n); }, type, id, payload, len);
}

// Handles every waiting byte. Returns true when a frame other than stats is
// ready in s_parser, and leaves the rest of the bytes for the next call.
static bool pumpSerial2() {
  while (Serial2.available()) {
    const int r = Serial2.read();
    if (r < 0) {
      break;
    }
    if (!s_parser.feed(static_cast<uint8_t>(r))) {
      continue;
    }
    if (s_parser.type() != bridge::kStats) {
      return true;
    }
    const uint8_t id = s_parser.id();
    tryPublishCompleteLine(s_parser.payload(), s_parser.length());
    sendFrame(bridge::kAck, id, nullptr, 0);
  }
  return false;
}

void ingest_serial2() {
  // Frames left over from a request that already ended are simply dropped.
  while (pumpSerial2()) {
  }
}

// Keeps ',' ':' and '/' readable: the boiler's field lists are comma separated.
static String encodeQueryPart(const String &part) {
  static const char *kHex = "0123456789ABCDEF";
  String out;
  out.reserve(part.length());
  for (size_t i = 0; i < part.length(); ++i) {
    const char c = part[i];
    if (isalnum(static_cast<unsigned char>(c)) || strchr("-_.~,:/", c) != nullptr) {
      out += c;
    } else {
      out += '%';
      out += kHex[(static_cast<uint8_t>(c) >> 4) & 0xF];
      out += kHex[static_cast<uint8_t>(c) & 0xF];
    }
  }
  return out;
}

static String boilerPath() {
  String path = server.uri();
  if (path == "/xtend") {
    path = "/";
  } else if (path.startsWith("/xtend/")) {
    path = path.substring(6);
  }
  for (int i = 0; i < server.args(); ++i) {
    path += i == 0 ? '?' : '&';
    path += encodeQueryPart(server.argName(i));
    path += '=';
    path += encodeQueryPart(server.arg(i));
  }
  return path;
}

static const char *kForwardedRequestHeaders[] = {"Accept",          "Accept-Encoding", "Accept-Language",
                                                 "Cookie",          "If-None-Match",   "If-Modified-Since",
                                                 "Range"};
static const char *kCollectedHeaders[] = {"Accept",        "Accept-Encoding",   "Accept-Language", "Cookie",
                                          "If-None-Match", "If-Modified-Since", "Range",           "Content-Type"};

// Waits for the next frame of this request; false when none arrives in time.
static bool nextPassFrame(uint8_t id, unsigned long timeout_ms, unsigned long deadline_ms) {
  const unsigned long start = millis();
  while (millis() - start < timeout_ms && static_cast<long>(millis() - deadline_ms) < 0) {
    if (pumpSerial2() && s_parser.id() == id) {
      return true;
    }
    delay(1);
  }
  return false;
}

// Reads "code\nName: value\n..." and sends it to the browser. Bodies go out
// chunked because a gzip or chunked boiler response has no usable length here.
static bool forwardHead(bool has_body) {
  char *text = s_parser.payload();
  const int code = atoi(text);
  String content_type = "application/octet-stream";
  char *line = strchr(text, '\n');
  while (line != nullptr) {
    line += 1;
    char *next = strchr(line, '\n');
    if (next != nullptr) {
      *next = '\0';
    }
    char *colon = strstr(line, ": ");
    if (colon != nullptr) {
      *colon = '\0';
      if (strcmp(line, "Content-Type") == 0) {
        content_type = colon + 2;
      } else {
        server.sendHeader(line, colon + 2);
      }
    }
    line = next;
  }
  if (code < 100 || code > 599) {
    return false;
  }
  if (has_body && code != 204 && code != 304) {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  } else {
    server.setContentLength(0);
  }
  server.send(code, content_type, "");
  return true;
}

void handlePassthrough() {
  const HTTPMethod method = server.method();
  if (method != HTTP_GET && method != HTTP_HEAD) {
    server.send(405, "text/plain", "passthrough is read-only: GET and HEAD only\n");
    return;
  }
  ++s_passRequests;

  static uint8_t s_nextId = 0;
  const uint8_t id = ++s_nextId;
  String request = String(method == HTTP_HEAD ? "HEAD " : "GET ") + boilerPath() + "\n";
  for (const char *name : kForwardedRequestHeaders) {
    if (server.hasHeader(name)) {
      request += String(name) + ": " + server.header(name) + "\n";
    }
  }
  if (request.length() > 2048) {
    server.send(414, "text/plain", "request too long for the passthrough\n");
    return;
  }

  const unsigned long deadline = millis() + PASS_REQUEST_MAX_MS;
  sendFrame(bridge::kRequest, id, request.c_str(), request.length());

  if (!nextPassFrame(id, PASS_HEAD_TIMEOUT_MS, deadline) || s_parser.type() != bridge::kRespHead ||
      !forwardHead(method == HTTP_GET)) {
    ++s_passFailed;
    server.send(504, "text/plain", "no answer from the boiler (see /status)\n");
    return;
  }
  const bool has_body = method == HTTP_GET;
  sendFrame(bridge::kAck, id, nullptr, 0);

  bool complete = false;
  while (nextPassFrame(id, PASS_CHUNK_TIMEOUT_MS, deadline)) {
    if (s_parser.type() == bridge::kRespEnd) {
      complete = s_parser.length() == 1 && s_parser.payload()[0] == 0;
      break;
    }
    if (s_parser.type() != bridge::kRespData || !server.client().connected()) {
      break;  // no ack: the client board stops by itself
    }
    server.sendContent(s_parser.payload(), s_parser.length());
    sendFrame(bridge::kAck, id, nullptr, 0);
  }

  if (!complete) {
    ++s_passFailed;
    server.client().stop();  // an unterminated response tells the browser it failed
    return;
  }
  if (has_body) {
    server.sendContent("");
  }
}

static bool otaWaitClient(uint8_t id, unsigned long timeout_ms, uint8_t want_type) {
  if (!nextPassFrame(id, timeout_ms, millis() + timeout_ms)) {
    s_otaError = "no answer from the client board";
    return false;
  }
  if (s_parser.type() != want_type) {
    s_otaError = "the client board refused the firmware";
    return false;
  }
  return true;
}

static void otaFail() {
  s_otaOk = false;
  if (s_otaError.length() == 0) {
    s_otaError = Update.errorString();
  }
}

// Streams one uploaded file straight into flash here or on the client board.
static void otaUpload(bool to_client) {
  static const uint8_t kOtaId = 0xEE;
  // A POST that isn't a file upload ends up here too, without an upload object.
  if (!server.header("Content-Type").startsWith("multipart/form-data")) {
    return;
  }
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    s_otaError = "";
    s_otaOk = server.authenticate(OTA_USER, OTA_PASSWORD);
    if (!s_otaOk) {
      return;
    }
    if (to_client) {
      sendFrame(bridge::kOtaBegin, kOtaId, nullptr, 0);
      s_otaOk = otaWaitClient(kOtaId, OTA_CHUNK_TIMEOUT_MS, bridge::kAck);
    } else if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaFail();
    }
  } else if (!s_otaOk) {
    return;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (to_client) {
      sendFrame(bridge::kOtaData, kOtaId, upload.buf, upload.currentSize);
      s_otaOk = otaWaitClient(kOtaId, OTA_CHUNK_TIMEOUT_MS, bridge::kAck);
    } else if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaFail();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (to_client) {
      sendFrame(bridge::kOtaEnd, kOtaId, nullptr, 0);
      s_otaOk = otaWaitClient(kOtaId, OTA_FINISH_TIMEOUT_MS, bridge::kOtaResult) && s_parser.length() == 1 &&
                s_parser.payload()[0] == 0;
      if (!s_otaOk && s_otaError.length() == 0) {
        s_otaError = "the client board rejected the firmware";
      }
    } else if (!Update.end(true)) {
      otaFail();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (!to_client) {
      Update.abort();
    }
    s_otaOk = false;
    s_otaError = "upload aborted";
  }
}

static void otaFinish(bool restart_self) {
  if (!server.authenticate(OTA_USER, OTA_PASSWORD)) {
    server.requestAuthentication();
    return;
  }
  if (!server.header("Content-Type").startsWith("multipart/form-data")) {
    server.send(400, "text/plain", "send the firmware as a file: curl -F firmware=@file.bin\n");
    return;
  }
  if (!s_otaOk) {
    server.send(500, "text/plain", "update failed: " + s_otaError + "\n");
    return;
  }
  server.send(200, "text/plain", "installed, restarting; it reverts itself if it doesn't come up healthy\n");
  if (restart_self) {
    delay(500);
    ESP.restart();
  }
}

static void checkFirmwareHealth() {
  if (s_proven_healthy) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED && millis() > OTA_HEALTHY_AFTER_MS) {
    s_proven_healthy = true;
    if (pendingVerify()) {
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("New firmware proved healthy, keeping it");
    }
  } else if (millis() > OTA_PROVE_HEALTHY_MS && pendingVerify()) {
    Serial.println("New firmware never proved healthy, rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

void handleRoot() {
  server.send(200, "text/plain",
               "BoilerBridge_Server — GET /data formatted fields, GET /data/raw verbatim, "
               "GET /status bridge health, GET /xtend/ the boiler's own pages (read-only)\n");
}

static unsigned long dataAgeMs() { return millis() - s_lastFrameMs; }

// Sends 503 and returns false when there is no fresh frame to serve.
static bool requireFreshData(bool have_data) {
  if (!have_data) {
    server.send(503, "text/plain", "no valid data yet\n");
    return false;
  }
  if (dataAgeMs() > DATA_STALE_MS) {
    server.send(503, "text/plain", "data stale: no frame from client recently (see /status)\n");
    return false;
  }
  server.sendHeader("X-Data-Age-Ms", String(dataAgeMs()));
  return true;
}

void handleDataRaw() {
  if (!requireFreshData(published_raw_len > 0)) {
    return;
  }
  server.send(200, "application/json", published_raw);
}

void handleData() {
  if (!requireFreshData(published_formatted_len > 0 && published_formatted != nullptr)) {
    return;
  }
  server.send(200, "application/json", published_formatted);
}

void handleStatus() {
  StaticJsonDocument<512> doc;
  doc["build"] = __DATE__ " " __TIME__;
  doc["uptime_s"] = millis() / 1000;
  doc["frames_ok"] = s_framesOk;
  doc["frames_bad"] = s_framesBad;
  if (s_framesOk > 0) {
    doc["data_age_ms"] = dataAgeMs();
  } else {
    doc["data_age_ms"] = nullptr;
  }
  doc["data_stale_after_ms"] = DATA_STALE_MS;
  doc["link_errors"] = s_parser.errors();
  doc["pass_requests"] = s_passRequests;
  doc["pass_failed"] = s_passFailed;
  doc["wifi_rssi_dbm"] = WiFi.RSSI();
  doc["free_heap"] = ESP.getFreeHeap();
  char out[512];
  serializeJson(doc, out, sizeof(out));
  server.send(200, "application/json", out);
}

static bool allocFormattedResponseBuffer() {
  // Serialized /data is usually a few–tens of kB; try small first to save heap.
  static const size_t kTrySizes[] = {16384u, 20480u, 24576u, 32768u, 40960u,
                                     49152u};
  published_formatted = nullptr;
  published_formatted_cap = 0;
  for (size_t i = 0; i < sizeof(kTrySizes) / sizeof(kTrySizes[0]); ++i) {
    const size_t sz = kTrySizes[i];
    void *p = malloc(sz);
    if (p != nullptr) {
      published_formatted = static_cast<char *>(p);
      published_formatted_cap = sz;
      memset(published_formatted, 0, published_formatted_cap);
      Serial.printf(
          "[cfg] published_formatted allocated %u bytes (free_heap=%u)\n",
          static_cast<unsigned>(published_formatted_cap), ESP.getFreeHeap());
      return true;
    }
    Serial.printf(
        "[cfg] malloc formatted buffer %u failed (free_heap=%u), trying "
        "smaller\n",
        static_cast<unsigned>(sz), ESP.getFreeHeap());
  }
  Serial.printf(
      "[cfg] FATAL: no heap for formatted /data buffer (free_heap=%u). "
      "Use ESP32 with PSRAM, reduce JSON_IN/JSON_OUT, or disable other "
      "features.\n",
      ESP.getFreeHeap());
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  memset(published_raw, 0, sizeof(published_raw));
  published_raw_len = 0;
  published_formatted_len = 0;

  if (!allocFormattedResponseBuffer()) {
    for (;;) {
      delay(1000);
    }
  }

  Serial2.setRxBufferSize(8192);
  Serial2.begin(SERIAL2_BAUD, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);

  if (!connectHomeWifiStartup()) {
    Serial.println(
        "[WiFi] startup: could not connect after all rounds; UART ingest "
        "runs; HTTP when link is up.");
  } else {
    Serial.print("OK, IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("[WiFi] RSSI %d dBm, channel %u\n", WiFi.RSSI(),
                  static_cast<unsigned>(WiFi.channel()));
  }

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", MDNS_HOSTNAME);
  } else {
    Serial.println("[mDNS] start failed");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/data/raw", handleDataRaw);
  server.on("/status", handleStatus);
  server.on("/ota/server", HTTP_POST, [] { otaFinish(true); }, [] { otaUpload(false); });
  server.on("/ota/client", HTTP_POST, [] { otaFinish(false); }, [] { otaUpload(true); });
  server.onNotFound(handlePassthrough);
  server.collectHeaders(kCollectedHeaders, sizeof(kCollectedHeaders) / sizeof(kCollectedHeaders[0]));
  server.begin();
  Serial.println("HTTP server on port 80");
  Serial.printf(
      "[cfg] PAYLOAD_MAX=%u JSON_IN_CAPACITY=%u JSON_OUT_CAPACITY=%u "
      "formatted_cap=%u Serial2_RX=%d TX=%d\n",
      static_cast<unsigned>(PAYLOAD_MAX),
      static_cast<unsigned>(JSON_IN_CAPACITY),
      static_cast<unsigned>(JSON_OUT_CAPACITY),
      static_cast<unsigned>(published_formatted_cap), SERIAL2_RX, SERIAL2_TX);
}

void loop() {
  checkFirmwareHealth();
  ingest_serial2();

  if (WiFi.status() != WL_CONNECTED) {
    tryWifiReconnectFromLoop();
    delay(200);
    return;
  }
  s_wifiDownSinceMs = 0;

  server.handleClient();
}
