#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-tvstreamer5:local}"
CONFIG_FILE="${CONFIG_FILE:-$(pwd)/tvstreamer5-config.json}"
DATA_DIR="$(dirname "${CONFIG_FILE}")"

if [[ ! -f "${CONFIG_FILE}" ]]; then
    echo "Config file not found: ${CONFIG_FILE}" >&2
    echo "Create it first or set CONFIG_FILE=/path/to/tvstreamer5-config.json" >&2
    exit 1
fi

docker run --rm -it \
    --init \
    --network host \
    -v "${DATA_DIR}:/data" \
    -w /data \
    -e GST_DEBUG="${GST_DEBUG:-1}" \
    "${IMAGE_NAME}"
