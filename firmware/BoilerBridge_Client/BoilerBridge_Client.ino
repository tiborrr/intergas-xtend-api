/*
 * BoilerBridge_Client — ESP32 (Arduino core)
 *
 * Libraries: WiFi, HTTPClient (built-in with ESP32 board package)
 *
 * Role: STA on boiler Wi-Fi. Polls the boiler API and sends the body to
 *       BoilerBridge_Server over Serial2, and carries out the server's read-only
 *       passthrough requests against the boiler (frames: see bridge_link.h).
 *
 * UART wiring to server board: this TX -> other RX, this RX <- other TX, GND common.
 * Default pins match README; change SERIAL2_RX / SERIAL2_TX if your board differs.
 */

#include <cstring>

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "bridge_link.h"

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

// Heat pump energy fields, always appended (also when secrets.h overrides the
// list above): COP, electrical/thermal power, kWh counters, compressor state.
#define XTEND_ENERGY_FIELDS \
  "f934,f91f,f926,63b3,63f0,6358,6339,63df,65a7,657e,6a53,6ac5"

// --- Poll interval ---
static const unsigned long POLL_INTERVAL_MS = 15000;
static const uint16_t HTTP_TIMEOUT_MS = 5000;

// Recovery: reboot instead of hanging when the boiler AP or API goes away.
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 60000;
static const unsigned long NO_SUCCESS_RESTART_MS = 10UL * 60UL * 1000UL;

// Passthrough. Polling waits while a request runs, so a request must end well
// within the server's DATA_STALE_MS (120 s).
static const unsigned long PASS_REQUEST_MAX_MS = 90000;
static const unsigned long PASS_ACK_TIMEOUT_MS = 3000;

// A firmware update stops when the server goes quiet.
static const unsigned long OTA_IDLE_TIMEOUT_MS = 30000;
// New firmware must get a stats frame acknowledged by the server within this
// time, or it reverts itself to the previous firmware.
static const unsigned long OTA_PROVE_HEALTHY_MS = 5UL * 60UL * 1000UL;

// --- Serial2 (link to BoilerBridge_Server) ---
#define SERIAL2_RX 16
#define SERIAL2_TX 17
#define SERIAL2_BAUD 115200

// Max bytes read from HTTP body; must match BoilerBridge_Server PAYLOAD_MAX
#define BOILER_BODY_MAX 4096

static unsigned long last_poll_ms = 0;
static unsigned long last_success_ms = 0;
static char body_buf[BOILER_BODY_MAX + 1];

static bridge::FrameParser<2048> s_parser;
// Pulled out of the request frame, because waiting for acks reuses the parser.
static char s_request[2049];
static bool s_request_pending = false;
static uint8_t s_request_id = 0;
static bool s_ota_active = false;
static unsigned long s_ota_last_ms = 0;
static bool s_proven_healthy = false;

#ifndef CONFIG_APP_ROLLBACK_ENABLE
#error "This firmware relies on the bootloader rolling back a bad update."
#endif

// Tells the Arduino core not to accept new firmware at boot: see markHealthy().
extern "C" bool verifyRollbackLater() { return true; }

static bool pendingVerify() {
  esp_ota_img_states_t state;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void markHealthy() {
  s_proven_healthy = true;
  if (pendingVerify()) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("New firmware proved healthy, keeping it");
  }
}

static void keepRequest() {
  s_request_id = s_parser.id();
  memcpy(s_request, s_parser.payload(), s_parser.length() + 1);
  s_request_pending = true;
}

static void sendFrame(uint8_t type, uint8_t id, const void *payload, size_t len) {
  bridge::writeFrame([](const uint8_t *data, size_t n) { Serial2.write(data, n); }, type, id, payload, len);
}

static bool waitForAck(uint8_t id, unsigned long timeout_ms) {
  const unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    while (Serial2.available()) {
      if (!s_parser.feed(static_cast<uint8_t>(Serial2.read()))) {
        continue;
      }
      if (s_parser.type() == bridge::kAck && s_parser.id() == id) {
        return true;
      }
      // The server may ask for a page just as a stats frame goes out.
      if (s_parser.type() == bridge::kRequest) {
        keepRequest();
      }
    }
    delay(1);
  }
  return false;
}

// Stream that HTTPClient writes the response body to: sends it in acknowledged
// chunks, and refuses more data once the server stops answering.
class ChunkStream : public Stream {
 public:
  ChunkStream(uint8_t id, unsigned long deadline_ms) : id_(id), deadline_ms_(deadline_ms) {}

  size_t write(uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t *data, size_t size) override {
    size_t done = 0;
    while (done < size) {
      if (failed_) {
        return done;
      }
      size_t n = bridge::kChunkSize - used_;
      if (n > size - done) {
        n = size - done;
      }
      memcpy(buf_ + used_, data + done, n);
      used_ += n;
      done += n;
      if (used_ == bridge::kChunkSize) {
        sendChunk();
      }
    }
    return done;
  }

  void finish() {
    if (used_ > 0) {
      sendChunk();
    }
  }

  bool failed() const { return failed_; }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  void sendChunk() {
    if (static_cast<long>(millis() - deadline_ms_) > 0) {
      failed_ = true;
      return;
    }
    sendFrame(bridge::kRespData, id_, buf_, used_);
    used_ = 0;
    if (!waitForAck(id_, PASS_ACK_TIMEOUT_MS)) {
      failed_ = true;
    }
  }

  uint8_t id_;
  unsigned long deadline_ms_;
  uint8_t buf_[bridge::kChunkSize];
  size_t used_ = 0;
  bool failed_ = false;
};

static void respondError(uint8_t id, const char *head) {
  sendFrame(bridge::kRespHead, id, head, strlen(head));
  waitForAck(id, PASS_ACK_TIMEOUT_MS);
  const uint8_t aborted = 0;
  sendFrame(bridge::kRespEnd, id, &aborted, 1);
}

// s_request: "METHOD /path\nName: value\n..."
static void handlePassthrough(uint8_t id) {
  char *line_end = strchr(s_request, '\n');
  if (line_end != nullptr) {
    *line_end = '\0';
  }
  char *space = strchr(s_request, ' ');
  if (space == nullptr) {
    respondError(id, "400\n");
    return;
  }
  *space = '\0';
  const char *method = s_request;
  const char *path = space + 1;
  const bool is_head = strcmp(method, "HEAD") == 0;
  // The server already filters this; this board is what talks to the boiler.
  if (strcmp(method, "GET") != 0 && !is_head) {
    respondError(id, "405\n");
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(String(BOILER_API_BASE) + path)) {
    respondError(id, "502\n");
    return;
  }

  for (char *line = line_end != nullptr ? line_end + 1 : nullptr; line != nullptr && *line != '\0';) {
    char *next = strchr(line, '\n');
    if (next != nullptr) {
      *next = '\0';
    }
    char *colon = strstr(line, ": ");
    if (colon != nullptr) {
      *colon = '\0';
      if (strcmp(line, "Accept-Encoding") == 0) {
        // HTTPClient ignores this one in addHeader(). The boiler's pages only
        // exist compressed; they pass through as they are, the browser unpacks.
        if (strstr(colon + 2, "gzip") != nullptr) {
          http.setAcceptEncoding("gzip");
        }
      } else {
        http.addHeader(line, colon + 2);
      }
    }
    line = next != nullptr ? next + 1 : nullptr;
  }

  static const char *kResponseHeaders[] = {"Content-Type",  "Content-Encoding", "Cache-Control",
                                           "ETag",          "Last-Modified",    "Set-Cookie",
                                           "Location",      "Content-Range",    "Accept-Ranges"};
  const size_t count = sizeof(kResponseHeaders) / sizeof(kResponseHeaders[0]);
  http.collectHeaders(kResponseHeaders, count);

  const int code = http.sendRequest(method);
  if (code <= 0) {
    Serial.printf("Passthrough %s %s failed: %d %s\n", method, path, code, http.errorToString(code).c_str());
    http.end();
    respondError(id, "502\n");
    return;
  }

  String head = String(code) + "\n";
  for (size_t i = 0; i < count; ++i) {
    String value = http.header(kResponseHeaders[i]);
    if (value.length() == 0) {
      continue;
    }
    // A redirect to the boiler's own address must stay on the bridge.
    if (strcmp(kResponseHeaders[i], "Location") == 0 && value.startsWith(BOILER_API_BASE)) {
      value = value.substring(strlen(BOILER_API_BASE));
    }
    head += String(kResponseHeaders[i]) + ": " + value + "\n";
  }
  if (head.length() > 2048) {
    http.end();
    respondError(id, "502\n");
    return;
  }
  sendFrame(bridge::kRespHead, id, head.c_str(), head.length());
  bool ok = waitForAck(id, PASS_ACK_TIMEOUT_MS);

  if (ok && !is_head && code != 204 && code != 304) {
    ChunkStream body(id, millis() + PASS_REQUEST_MAX_MS);
    const int written = http.writeToStream(&body);
    body.finish();
    ok = written >= 0 && !body.failed();
    Serial.printf("Passthrough %s %s: %d, %d bytes%s\n", method, path, code, written, ok ? "" : ", aborted");
  }
  http.end();

  const uint8_t status = ok ? 0 : 1;
  sendFrame(bridge::kRespEnd, id, &status, 1);
}

static void sendOtaResult(uint8_t id, uint8_t status) {
  sendFrame(bridge::kOtaResult, id, &status, 1);
  Serial2.flush();
}

static void handleOtaFrame() {
  const uint8_t id = s_parser.id();
  s_ota_last_ms = millis();
  switch (s_parser.type()) {
    case bridge::kOtaBegin:
      if (s_ota_active) {
        Update.abort();
      }
      s_ota_active = Update.begin(UPDATE_SIZE_UNKNOWN);
      Serial.printf("Firmware update %s\n", s_ota_active ? "started" : "refused");
      if (s_ota_active) {
        sendFrame(bridge::kAck, id, nullptr, 0);
      } else {
        sendOtaResult(id, 1);
      }
      break;
    case bridge::kOtaData:
      if (s_ota_active && Update.write(reinterpret_cast<uint8_t *>(s_parser.payload()), s_parser.length()) ==
                              s_parser.length()) {
        sendFrame(bridge::kAck, id, nullptr, 0);
      } else {
        if (s_ota_active) {
          Update.abort();
          s_ota_active = false;
        }
        sendOtaResult(id, 1);
      }
      break;
    case bridge::kOtaEnd: {
      // Update.end() checks the image's own hash before making it bootable.
      const bool ok = s_ota_active && Update.end(true);
      s_ota_active = false;
      Serial.printf("Firmware update %s\n", ok ? "installed, restarting" : "failed");
      sendOtaResult(id, ok ? 0 : 1);
      if (ok) {
        delay(500);
        ESP.restart();
      }
      break;
    }
    default:
      break;
  }
}

static void pumpRequests() {
  while (Serial2.available()) {
    if (!s_parser.feed(static_cast<uint8_t>(Serial2.read()))) {
      continue;
    }
    const uint8_t type = s_parser.type();
    if (type == bridge::kRequest) {
      keepRequest();
    } else if (type == bridge::kOtaBegin || type == bridge::kOtaData || type == bridge::kOtaEnd) {
      handleOtaFrame();
    }
    if (s_request_pending) {
      break;
    }
  }
  if (s_request_pending) {
    s_request_pending = false;
    handlePassthrough(s_request_id);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  Serial2.setRxBufferSize(4096);
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
  pumpRequests();

  unsigned long now = millis();
  if (s_ota_active) {
    if (now - s_ota_last_ms > OTA_IDLE_TIMEOUT_MS) {
      Serial.println("Firmware update stalled, aborting");
      Update.abort();
      s_ota_active = false;
    }
    return;  // the server is sending: no polling and no restarts now
  }
  if (!s_proven_healthy && now > OTA_PROVE_HEALTHY_MS && pendingVerify()) {
    Serial.println("New firmware never proved healthy, rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
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

  String url = String(BOILER_API_BASE) + "/api/stats/values?fields=" + BOILER_STATS_FIELDS + "," +
               XTEND_ENERGY_FIELDS;

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

  sendFrame(bridge::kStats, 0, body_buf, len);
  last_success_ms = millis();
  // Only an answer proves that the whole chain works with this firmware.
  if (!s_proven_healthy && waitForAck(0, PASS_ACK_TIMEOUT_MS)) {
    markHealthy();
  }
  Serial.printf("Forwarded %u bytes on Serial2\n", (unsigned)len);
}
