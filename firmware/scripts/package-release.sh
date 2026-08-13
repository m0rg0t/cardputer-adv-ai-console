#!/usr/bin/env bash
set -euo pipefail

version="${1:?usage: package-release.sh VERSION}"
environment="cardputer-adv-recorder"
script_dir="$(cd "$(dirname "$0")" && pwd)"
firmware_dir="$(cd "${script_dir}/.." && pwd)"
repository_dir="$(cd "${firmware_dir}/.." && pwd)"
build_dir="${firmware_dir}/.pio/build/${environment}"
output_dir="${repository_dir}/release/firmware"
image_name="Cardputer-ADV-Agent-Console-${version}-M5Apps.bin"
image_path="${output_dir}/${image_name}"
max_m5apps_bytes=$((0x160000))

firmware_bytes=$(wc -c < "${build_dir}/firmware.bin")
if (( firmware_bytes > max_m5apps_bytes )); then
  echo "ERROR: firmware.bin is ${firmware_bytes} bytes; Cardputer ADV M5Apps slot allows ${max_m5apps_bytes}." >&2
  exit 1
fi

mkdir -p "${output_dir}"
cp "${build_dir}/firmware.bin" "${image_path}"

(
  cd "${output_dir}"
  shasum -a 256 "${image_name}" > SHA256SUMS.txt
)
