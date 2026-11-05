# intergas-xtend-api

Firmware for **two ESP32** boards: one stays on the **boiler’s Wi‑Fi** and reads its **HTTP API**; the other stays on your **home Wi‑Fi** and exposes **`GET /data`** (formatted Xtend fields), **`GET /data/raw`** (verbatim payload), a **read-only passthrough** to the Xtend's own pages and API, and **firmware updates over the network**. They exchange data over **UART** (Serial2).

## Hardware UART (both sketches use the same pin defines)

| Signal | BoilerBridge_Client | BoilerBridge_Server |
|--------|---------------------|---------------------|
| Client TX → | GPIO **17** (Serial2 TX) | → Server RX GPIO **16** |
| Client RX ← | GPIO **16** (Serial2 RX) | ← Server TX GPIO **17** |
| GND | common ground | common ground |

Baud rate: **115200** (both). Change `SERIAL2_RX` / `SERIAL2_TX` in each `.ino` if your modules use different pins.

## Wi-Fi passwords and privacy (do not commit secrets)

Credentials live only in **`secrets.h`** next to each sketch. Those files are **gitignored** and listed in **`.cursorignore`** so they stay out of normal git history and are less likely to be pulled into AI context.

1. Copy the example once per sketch:

   ```bash
   cp firmware/BoilerBridge_Client/secrets.h.example firmware/BoilerBridge_Client/secrets.h
   cp firmware/BoilerBridge_Server/secrets.h.example firmware/BoilerBridge_Server/secrets.h
   ```

2. Edit each **`secrets.h`** with your real SSIDs and passwords.

3. **Never** paste `secrets.h` into chat, screenshots of it, or commit it. The `.ino` files only `#include "secrets.h"`; if `secrets.h` is missing, the build stops with a clear error.

**BoilerBridge_Client `secrets.h`:** `BOILER_SSID`, `BOILER_PASS`, **`BOILER_API_BASE`** (no trailing slash, e.g. `"http://10.20.30.1"`). The polled field ids are not secret, so they live in the sketch as **`STATS_FIELDS`**, including the heat pump energy fields (COP, electrical/thermal power, kWh counters, compressor frequency). Plain **HTTP** by default; **HTTPS** needs extra `HTTPClient` certificate setup.

If you still have **`BOILER_API_URL`** from an older checkout, replace it with **`BOILER_API_BASE`** using `secrets.h.example` as a template.

Compiled firmware (`.bin`, `.elf`) contains these passwords in plain text; `.gitignore` keeps them out of git — don't share build output either.

**BoilerBridge_Server `secrets.h`:** `HOME_SSID`, `HOME_PASS`, **`OTA_PASSWORD`** (protects firmware updates; generate one with `openssl rand -hex 16`), **`BRIDGE_PASSWORD`** (protects the passthrough, user `xtend`; generate it the same way).

`secrets.h.example` stays in git with **fake placeholders** only.

## Xtend boiler HTTP (reference)

A valid read uses a **field list** (same hex ids as in `known_xtend_codes.md`, without `0x`):

```http
GET http://<boiler-host>/api/stats/values?fields=7940,79b3,7921,7e2c,77c3,7e51,77d2,f9f2,7ed3,629c,6280,621d,62ed,503e,5088,5077,5041,50f2,62d1,620f,6206,8439,47e0,7e7a,7774,77de,6115,61ba,61eb,610b,6101,6117
```

Example response shape (forwarded on UART as one line after stripping `\r`/`\n`):

```json
{
  "stats": {
    "5041": 49,
    "47e0": "V0.90-"
  }
}
```

## Build and flash

Install the **ESP32** Arduino core once (e.g. `arduino-cli core install esp32:esp32` with Espressif’s board index).

**BoilerBridge_Server** also needs **ArduinoJson v6** (v7 is not supported; the build stops with an error if v7 is installed):

```bash
arduino-cli lib install ArduinoJson@6.21.5
```

Regenerate the server field map after editing `known_xtend_codes.md`:

```bash
python3 tools/gen_xtend_field_map.py
```

That writes `firmware/BoilerBridge_Server/xtend_field_map.gen.h` (commit it when the table changes).

### Script (easiest)

From the repo root:

```bash
./scripts/flash.sh server              # home / HTTP server sketch
./scripts/flash.sh client              # boiler / HTTP client sketch
./scripts/flash.sh server --monitor  # upload then serial at 115200 baud
```

With both boards plugged in, auto-detect refuses to guess; pass the serial port as second argument:

```bash
./scripts/flash.sh client /dev/cu.usbserial-210
```

Override defaults:

- **`ARDUINO_PORT`** — fixed port when several USB serial devices exist  
- **`FQBN`** — default is `esp32:esp32:nodemcu-32s` (change if your board needs another profile)

### Over the network (OTA)

Once both boards run this firmware, no USB is needed:

```bash
./scripts/ota.sh server
./scripts/ota.sh client
```

The script builds the sketch and uploads it to the server board (user `ota`, password `OTA_PASSWORD`). The client board sits on the boiler's Wi-Fi and can't be reached from home, so the server sends its firmware over the UART; expect 2 to 3 minutes for the client, during which `/data` returns no fresh data.

A bad update reverts itself. New firmware only stays when it proves healthy: the server after 60 s on the home network, the client once the server acknowledges a stats frame. Without that within 5 minutes, or after a crash, the bootloader returns to the previous firmware. The healthy check can't catch everything, so update one board at a time and check **`/status`** in between.

Both boards must speak the same UART protocol (`firmware/shared/bridge_link.h`). When changing the framing of existing frame types, keep the OTA frames working, or a client can no longer be updated over the network.

### Manual (`arduino-cli`)

```bash
arduino-cli compile -b esp32:esp32:nodemcu-32s firmware/BoilerBridge_Server
arduino-cli upload -p /dev/cu.usbserial-210 -b esp32:esp32:nodemcu-32s firmware/BoilerBridge_Server
arduino-cli monitor -p /dev/cu.usbserial-210 -c baudrate=115200
```

## Use

When both are running and UART is wired:

| URL | Response |
|-----|----------|
| **`GET /data`** | Formatted JSON: `{ "fields": [ { "id", "raw", optional "name", "type", "unit", "scaled" }, ... ] }` built from `stats` in the latest payload. String-valued stats omit `scaled`. Unknown ids still appear with `id` and `raw`. |
| **`GET /data/raw`** | Unchanged body from the boiler (last UART line), for debugging. |
| **`GET /status`** | Bridge health: `build`, `uptime_s`, `frames_ok`, `frames_bad`, `data_age_ms`, `link_errors`, `pass_requests`, `pass_failed`, `wifi_rssi_dbm`, `free_heap`. |
| **`GET /bridge`** | Short text hint. |
| **`GET /`** | The Xtend's own dashboard, which only runs at `/`. Needs the password (user `xtend`). The first load takes about half a minute on a good signal, because it fetches over 30 files one at a time. `/xtend/` redirects here. |
| **any other `GET` or `HEAD`** | Passed to the Xtend unchanged, for example `/api/stats/values?fields=7940`. Needs the password. |
| **`POST /api/auth`** | The dashboard's service login, passed to the Xtend. Needs the password. Every other `POST` gets 405, so settings can't be written through the bridge. The password travels unencrypted: keep the bridge off the internet. |
| **`POST /ota/server`, `/ota/client`** | Firmware upload, see OTA above. |

The server announces itself over mDNS, so `http://boilerbridge.local/data` works on most networks.

**`/data`** and **`/data/raw`** return **503** when nothing arrived yet, or when the last good frame is older than 2 minutes (client offline). Successful responses carry an **`X-Data-Age-Ms`** header.

Lines that are not JSON, or have no **`stats`** object, are dropped (counted in `frames_bad`); the previous good frame stays published until it goes stale.

Fields of type `int16` with raw value `32767` are the Xtend's "not available" marker; their `scaled` is `null`.

Both boards reboot themselves when stuck: the client after 60 s without boiler Wi-Fi at boot or 10 min without a successful poll, the server after 10 min without home Wi-Fi.

## Protocol

Client **HTTP GET**s `BOILER_API_BASE + "/api/stats/values?fields=" + STATS_FIELDS`, then sends the response body to the server as a frame. All UART traffic is in CRC-checked frames, see `firmware/shared/bridge_link.h`; test them without hardware:

```bash
c++ -std=c++17 -Ifirmware/shared tests/test_bridge_link.cpp -o /tmp/test_bridge_link && /tmp/test_bridge_link
```

**Passthrough:** the server handles one request at a time and sends it to the client, which sends the response back in acknowledged 1 kB chunks. At 115200 baud that is roughly 10 kB/s, and while a request runs `/data` and `/status` wait and the client doesn't poll, so don't leave a dashboard open while collecting data. On a weak Wi-Fi signal a transfer can pause for many seconds while TCP retries; check `wifi_rssi_dbm` in `/status`. The Xtend only serves its pages gzipped: send `Accept-Encoding: gzip` (browsers do; for curl use `--compressed`). There is **no password** on the passthrough: anyone on the home network can read the Xtend through it, and a boiler that changes state on a GET would be exposed too. Add authentication before ever allowing POST.

**Tuning:** If **`/data`** fails to parse or serialize, increase **`JSON_IN_CAPACITY`** / **`JSON_OUT_CAPACITY`** in `BoilerBridge_Server.ino` (and ensure **`PAYLOAD_MAX`** / client **`BOILER_BODY_MAX`** are large enough for your boiler response).
