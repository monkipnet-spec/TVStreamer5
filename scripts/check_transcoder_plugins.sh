#!/usr/bin/env bash
set -euo pipefail

required=(
  tsparse tsdemux decodebin
  videoconvert videoscale videorate
  x264enc h264parse
  audioconvert audioresample aacparse
  mpegtsmux
)

missing=()
for element in "${required[@]}"; do
  if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    missing+=("$element")
  fi
done

audio_encoder=""
for element in avenc_aac fdkaacenc voaacenc; do
  if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    audio_encoder="$element"
    break
  fi
done
if [[ -z "$audio_encoder" ]]; then
  missing+=("AAC encoder (avenc_aac/fdkaacenc/voaacenc)")
fi

if ((${#missing[@]} > 0)); then
  echo "Transcoding is unavailable. Missing GStreamer elements:"
  printf '  - %s\n' "${missing[@]}"
  echo "Passthrough streaming can still be used."
  exit 1
fi

echo "Transcoding dependencies are available."
echo "  Video encoder: x264enc"
echo "  Audio encoder: $audio_encoder"
