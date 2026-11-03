# intergas-xtend-api

Firmware for **two ESP32** boards: one stays on the **boiler’s Wi‑Fi** and reads its **HTTP API**; the other stays on your **home Wi‑Fi** and exposes **`GET /data`** (formatted Xtend fields) and **`GET /data/raw`** (verbatim UART payload). They exchange data over **UART** (Serial2).

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

**BoilerBridge_Client `secrets.h`:** `BOILER_SSID`, `BOILER_PASS`, **`BOILER_API_BASE`** (no trailing slash, e.g. `"http://10.20.30.1"`). Optional **`BOILER_STATS_FIELDS`** overrides the default field list in `BoilerBridge_Client.ino` (field ids are not secret, so they live in the sketch). The heat pump energy fields in **`XTEND_ENERGY_FIELDS`** (COP, electrical/thermal power, kWh counters, compressor frequency) are always appended. Plain **HTTP** by default; **HTTPS** needs extra `HTTPClient` certificate setup.

If you still have **`BOILER_API_URL`** from an older checkout, replace it with **`BOILER_API_BASE`** using `secrets.h.example` as a template.

Compiled firmware (`.bin`, `.elf`) contains these passwords in plain text; `.gitignore` keeps them out of git — don't share build output either.

**BoilerBridge_Server `secrets.h`:** `HOME_SSID`, `HOME_PASS`.

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
| **`GET /status`** | Bridge health: `uptime_s`, `frames_ok`, `frames_bad`, `data_age_ms`, `wifi_rssi_dbm`, `free_heap`. |
| **`GET /`** | Short text hint. |

The server announces itself over mDNS, so `http://boilerbridge.local/data` works on most networks.

**`/data`** and **`/data/raw`** return **503** when nothing arrived yet, or when the last good frame is older than 2 minutes (client offline). Successful responses carry an **`X-Data-Age-Ms`** header.

Lines that are not JSON, or have no **`stats`** object, are dropped (counted in `frames_bad`); the previous good frame stays published until it goes stale.

Fields of type `int16` with raw value `32767` are the Xtend's "not available" marker; their `scaled` is `null`.

Both boards reboot themselves when stuck: the client after 60 s without boiler Wi-Fi at boot or 10 min without a successful poll, the server after 10 min without home Wi-Fi.

## Protocol

Client **HTTP GET**s `BOILER_API_BASE + "/api/stats/values?fields=" + BOILER_STATS_FIELDS`, then forwards the response body on Serial2 as **one line** terminated with **`\n`**: carriage returns and line feeds **inside** the body are **removed** (not replaced with spaces) so pretty-printed JSON stays valid JSON. Server stores the last complete line.

**Tuning:** If **`/data`** fails to parse or serialize, increase **`JSON_IN_CAPACITY`** / **`JSON_OUT_CAPACITY`** in `BoilerBridge_Server.ino` (and ensure **`PAYLOAD_MAX`** / client **`BOILER_BODY_MAX`** are large enough for your boiler response).
