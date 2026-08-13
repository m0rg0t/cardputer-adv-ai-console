#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
gateway_dir="$(cd "${script_dir}/.." && pwd)"
output_dir="${1:-${gateway_dir}/certs}"
shift $(( $# > 0 ? 1 : 0 ))

if (( $# == 0 )); then
  echo "usage: generate-certificates.sh [OUTPUT_DIR] HOST [HOST ...]" >&2
  echo "example: gateway/scripts/generate-certificates.sh gateway/certs mac-mini.local 192.168.1.20" >&2
  exit 2
fi

mkdir -p "${output_dir}"
extensions="${output_dir}/gateway-openssl.cnf"
{
  printf '%s\n' \
    '[server_cert]' \
    'basicConstraints = critical,CA:FALSE' \
    'keyUsage = critical,digitalSignature,keyEncipherment' \
    'extendedKeyUsage = serverAuth' \
    'subjectAltName = @gateway_names' \
    'subjectKeyIdentifier = hash' \
    'authorityKeyIdentifier = keyid,issuer' \
    '' \
    '[gateway_names]'
  ip_index=1
  dns_index=1
  for host in "$@"; do
    if [[ ! "$host" =~ ^[A-Za-z0-9._:-]+$ ]]; then
      echo "invalid host name: $host" >&2
      exit 2
    fi
    if [[ "$host" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
      printf 'IP.%d = %s\n' "$ip_index" "$host"
      ip_index=$((ip_index + 1))
    else
      printf 'DNS.%d = %s\n' "$dns_index" "$host"
      dns_index=$((dns_index + 1))
    fi
  done
} > "${extensions}"

openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
  -subj "/CN=Cardputer Agent Local CA" \
  -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -addext "subjectKeyIdentifier=hash" \
  -keyout "${output_dir}/cardputer-agent-ca.key" \
  -out "${output_dir}/cardputer-agent-ca.pem"

openssl req -new -newkey rsa:2048 -nodes -sha256 \
  -subj "/CN=Cardputer Agent Gateway" \
  -keyout "${output_dir}/gateway.key" \
  -out "${output_dir}/gateway.csr"

openssl x509 -req -sha256 -days 825 \
  -in "${output_dir}/gateway.csr" \
  -CA "${output_dir}/cardputer-agent-ca.pem" \
  -CAkey "${output_dir}/cardputer-agent-ca.key" \
  -CAcreateserial \
  -extfile "${extensions}" \
  -extensions server_cert \
  -out "${output_dir}/gateway.crt"

openssl verify -x509_strict \
  -CAfile "${output_dir}/cardputer-agent-ca.pem" \
  "${output_dir}/gateway.crt"

echo "Copy ${output_dir}/cardputer-agent-ca.pem to the SD card as AGENT_CA.PEM"
