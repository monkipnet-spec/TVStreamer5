#!/usr/bin/env bash
set -euo pipefail

required=(
  tsparse tsdemux decodebin
  videoconvert deinterlace videoscale videorate
  x264enc h264parse
  audioconvert audioresample
  mpegtsmux
)

missing=()
for element in "${required[@]}"; do
  if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    missing+=("$element")
  fi
done

aac_encoder=""
if gst-inspect-1.0 aacparse >/dev/null 2>&1; then
  for element in avenc_aac fdkaacenc voaacenc; do
    if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
      aac_encoder="$element"
      break
    fi
  done
fi

mp3_encoder=""
if gst-inspect-1.0 mpegaudioparse >/dev/null 2>&1; then
  for element in lamemp3enc avenc_mp3; do
    if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
      mp3_encoder="$element"
      break
    fi
  done
fi

if [[ -z "$aac_encoder" && -z "$mp3_encoder" ]]; then
  missing+=("audio encoder/parser (AAC or MP3)")
fi

if ((${#missing[@]} > 0)); then
  echo "Transcoding is unavailable. Missing GStreamer elements:"
  printf '  - %s\n' "${missing[@]}"
  echo "Passthrough streaming can still be used."
  exit 1
fi

echo "Transcoding dependencies are available."
echo "  Video encoder: x264enc"
echo "  AAC encoder: ${aac_encoder:-not available}"
echo "  MP3 encoder: ${mp3_encoder:-not available}"
echo "  Deinterlacing: deinterlace"
