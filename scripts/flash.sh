#!/usr/bin/env bash
# Compile and upload BoilerBridge firmware. Run from repo root or any cwd.
#
# Server sketch needs ArduinoJson v6 once: arduino-cli lib install ArduinoJson
#
# Usage:
#   ./scripts/flash.sh server
#   ./scripts/flash.sh client
#   ./scripts/flash.sh server /dev/cu.usbserial-210
#   ./scripts/flash.sh client --monitor
#
# Env:
#   FQBN          default: esp32:esp32:nodemcu-32s
#   ARDUINO_PORT  serial device (skips auto-detect)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FQBN="${FQBN:-esp32:esp32:nodemcu-32s}"

usage() {
  echo "Usage: $0 {server|client} [PORT] [--monitor|-m]" >&2
  echo "" >&2
  echo "  server  — firmware/BoilerBridge_Server" >&2
  echo "  client  — firmware/BoilerBridge_Client" >&2
  echo "  PORT    — e.g. /dev/cu.usbserial-210 (optional if auto-detect finds one)" >&2
  echo "  --monitor — after upload, open serial at 115200 baud (Ctrl+C to exit)" >&2
  echo "" >&2
  echo "Env: FQBN, ARDUINO_PORT" >&2
  exit 1
}

pick_port() {
  if [[ -n "${ARDUINO_PORT:-}" ]]; then
    echo "$ARDUINO_PORT"
    return
  fi
  shopt -s nullglob
  local ports=(/dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem*)
  shopt -u nullglob
  if [[ ${#ports[@]} -eq 0 ]]; then
    echo "No USB serial port found. Plug in the board, or set ARDUINO_PORT or pass PORT." >&2
    exit 1
  fi
  # Both bridge boards are often plugged in at once; guessing could flash the
  # client sketch onto the server board (or vice versa).
  if [[ ${#ports[@]} -gt 1 ]]; then
    echo "Several USB serial ports found: ${ports[*]}" >&2
    echo "Pass the port explicitly, e.g. $0 {server|client} ${ports[0]}" >&2
    exit 1
  fi
  echo "${ports[0]}"
}

MONITOR=0
POSITIONAL=()
for a in "$@"; do
  case "$a" in
    --monitor | -m) MONITOR=1 ;;
    -h | --help) usage ;;
    *) POSITIONAL+=("$a") ;;
  esac
done

set -- "${POSITIONAL[@]}"

case "${1:-}" in
  server)
    SKETCH="$REPO_ROOT/firmware/BoilerBridge_Server"
    ;;
  client)
    SKETCH="$REPO_ROOT/firmware/BoilerBridge_Client"
    ;;
  *) usage ;;
esac

if [[ -n "${2:-}" ]]; then
  PORT="$2"
else
  PORT="$(pick_port)"
fi

echo "Sketch:  $SKETCH"
echo "FQBN:    $FQBN"
echo "Port:    $PORT"
echo ""

arduino-cli compile -b "$FQBN" "$SKETCH"
arduino-cli upload -p "$PORT" -b "$FQBN" "$SKETCH"

echo ""
echo "Upload OK."

if [[ "$MONITOR" -eq 1 ]]; then
  echo "Serial monitor 115200 (Ctrl+C to quit)…"
  arduino-cli monitor -p "$PORT" -c baudrate=115200
fi
