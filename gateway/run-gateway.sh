#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="${0:A:h}"
cd "$SCRIPT_DIR"

if [[ ! -f "$SCRIPT_DIR/.env" ]]; then
  print -u2 "Missing gateway/.env. Run: python3 scripts/configure.py"
  exit 1
fi

set -a
source "$SCRIPT_DIR/.env"
set +a

# Keep the OpenAI credential in the macOS Keychain instead of the project,
# launchd plist, or Cardputer SD card.
if [[ -z "${OPENAI_API_KEY:-}" ]]; then
  OPENAI_API_KEY="$(/usr/bin/security find-generic-password \
    -a "$USER" -s CardputerAgentGatewayOpenAI -w 2>/dev/null || true)"
  export OPENAI_API_KEY
fi

UV_EXECUTABLE="${UV_EXECUTABLE:-$(command -v uv || true)}"
if [[ -z "$UV_EXECUTABLE" ]]; then
  print -u2 "uv is not installed. See https://docs.astral.sh/uv/"
  exit 1
fi

exec "$UV_EXECUTABLE" run uvicorn voice_gateway.app:app_factory \
  --factory \
  --host 0.0.0.0 \
  --port 8765 \
  --ssl-certfile "$SCRIPT_DIR/certs/gateway.crt" \
  --ssl-keyfile "$SCRIPT_DIR/certs/gateway.key"
