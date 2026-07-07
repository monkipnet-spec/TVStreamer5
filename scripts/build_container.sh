#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-tvstreamer5:local}"

docker build -t "${IMAGE_NAME}" .
echo "Built ${IMAGE_NAME}"
