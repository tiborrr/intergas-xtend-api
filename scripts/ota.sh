#!/usr/bin/env bash
# Build and install firmware over the network, via BoilerBridge_Server.
# The client board can only be reached through the server.
#
# Usage:
#   ./scripts/ota.sh server
#   ./scripts/ota.sh client
#
# Env:
#   BRIDGE  default: http://boilerbridge.local
#   FQBN    default: esp32:esp32:nodemcu-32s

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE="${BRIDGE:-http://boilerbridge.local}"
FQBN="${FQBN:-esp32:esp32:nodemcu-32s}"

case "${1:-}" in
  server) SKETCH="BoilerBridge_Server" ;;
  client) SKETCH="BoilerBridge_Client" ;;
  *) echo "Usage: $0 {server|client}" >&2; exit 1 ;;
esac

PASSWORD="$(sed -nE 's/^#define OTA_PASSWORD "(.*)"/\1/p' "$REPO_ROOT/firmware/BoilerBridge_Server/secrets.h")"
if [[ -z "$PASSWORD" ]]; then
  echo "No OTA_PASSWORD in firmware/BoilerBridge_Server/secrets.h" >&2
  exit 1
fi

# Firmware images hold the Wi-Fi passwords in plain text: don't keep them around.
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "Building $SKETCH..."
arduino-cli compile -b "$FQBN" --output-dir "$OUT" "$REPO_ROOT/firmware/$SKETCH" | tail -2

echo "Uploading, the client takes a few minutes over its UART..."
# The password goes through stdin, so it never shows up in the process list.
printf 'user = "ota:%s"\n' "$PASSWORD" | curl --fail-with-body --silent --show-error --config - \
  --max-time 600 -F "firmware=@$OUT/$SKETCH.ino.bin" "$BRIDGE/ota/$1"
