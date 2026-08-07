#!/usr/bin/env bash
set -euo pipefail

required=(
  gst-launch-1.0
  uridecodebin
  queue
  videoconvert
  videoscale
  videorate
  x264enc
  h264parse
  audioconvert
  audioresample
  aacparse
  mpegtsmux
  udpsink
)

missing=()
if ! command -v gst-launch-1.0 >/dev/null 2>&1; then
  missing+=("gst-launch-1.0")
fi

for element in "${required[@]:1}"; do
  if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    missing+=("$element")
  fi
done

aac_encoder=""
for element in voaacenc fdkaacenc avenc_aac; do
  if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    aac_encoder="$element"
    break
  fi
done

mp3_encoder=""
for element in lamemp3enc avenc_mp3; do
  if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    mp3_encoder="$element"
    break
  fi
done

if [[ -z "$aac_encoder" ]]; then
  missing+=("AAC encoder: fdkaacenc, voaacenc or avenc_aac")
fi

if ((${#missing[@]} > 0)); then
  echo "GStreamer transcoding is unavailable. Missing elements/tools:"
  printf '  - %s\n' "${missing[@]}"
  echo "Install GStreamer plugins base/good/bad/ugly and gstreamer1.0-libav."
  exit 1
fi

echo "GStreamer transcoding dependencies are available."
echo "  Launcher: $(command -v gst-launch-1.0)"
echo "  Video encoder: x264enc"
echo "  AAC encoder: ${aac_encoder}"
echo "  MP3 encoder: ${mp3_encoder:-not available}"
echo "  UDP MPEG-TS mux/output: mpegtsmux + udpsink"
