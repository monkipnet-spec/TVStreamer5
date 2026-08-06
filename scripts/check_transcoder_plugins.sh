#!/usr/bin/env bash
set -euo pipefail

if command -v ffmpeg >/dev/null 2>&1; then
  echo "FFmpeg transcoder is available."
  echo "  ffmpeg: $(command -v ffmpeg)"
  if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' libx264'; then
    echo "  Video encoder: libx264"
  else
    echo "  WARNING: libx264 encoder was not reported by ffmpeg -encoders"
  fi
  if ffmpeg -hide_banner -encoders 2>/dev/null | grep -qE ' aac| libfdk_aac'; then
    echo "  AAC encoder: available"
  else
    echo "  WARNING: AAC encoder was not reported by ffmpeg -encoders"
  fi
  if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' libmp3lame'; then
    echo "  MP3 encoder: libmp3lame"
  else
    echo "  WARNING: libmp3lame encoder was not reported by ffmpeg -encoders"
  fi
  exit 0
fi

required=(
  parsebin tsparse decodebin
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
  for element in fdkaacenc voaacenc avenc_aac; do
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
  echo "Transcoding is unavailable. Missing ffmpeg and required GStreamer elements:"
  printf '  - %s\n' "${missing[@]}"
  echo "Install ffmpeg for the new no-GStreamer transcoder."
  exit 1
fi

echo "Legacy GStreamer transcoding dependencies are available."
echo "  Video encoder: x264enc"
echo "  AAC encoder: ${aac_encoder:-not available}"
echo "  MP3 encoder: ${mp3_encoder:-not available}"
echo "  Deinterlacing: deinterlace"
