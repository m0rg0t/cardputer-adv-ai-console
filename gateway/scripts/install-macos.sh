#!/bin/zsh
set -euo pipefail

gateway_dir="${0:A:h:h}"
label="io.github.cardputer-adv-agent.gateway"
template="$gateway_dir/${label}.plist.template"
destination="$HOME/Library/LaunchAgents/${label}.plist"

if [[ ! -f "$gateway_dir/.env" ]]; then
  print -u2 "Configure the gateway first: python3 gateway/scripts/configure.py"
  exit 1
fi
if [[ ! -f "$gateway_dir/certs/gateway.crt" || ! -f "$gateway_dir/certs/gateway.key" ]]; then
  print -u2 "Generate TLS certificates first: gateway/scripts/generate-certificates.sh"
  exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
escaped_gateway=${gateway_dir//&/\\&}
escaped_home=${HOME//&/\\&}
sed -e "s|__GATEWAY_DIR__|$escaped_gateway|g" \
    -e "s|__HOME__|$escaped_home|g" \
    "$template" > "$destination"

launchctl bootout "gui/$UID/$label" 2>/dev/null || true
launchctl bootstrap "gui/$UID" "$destination"
launchctl enable "gui/$UID/$label"
print "Installed and started $label"
