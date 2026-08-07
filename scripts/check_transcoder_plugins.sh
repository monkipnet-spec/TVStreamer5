#!/usr/bin/env bash
set -euo pipefail

# v50: transcoded streams use direct protocol output modules again. The base
# transcoder chain is required; protocol-specific elements are checked and
# reported separately because they are only required when that output is used.
required=(
  gst-launch-1.0
  uridecodebin
  queue
  videoconvert
  deinterlace
  videoscale
  videorate
  x264enc
  h264parse
  audioconvert
  audioresample
  audiorate
  aacparse
)

protocol_elements=(
  "udp/udp-cbr/udp-vbr:mpegtsmux tsparse udpsink"
  "rtp:mpegtsmux tsparse rtpmp2tpay udpsink"
  "http:mpegtsmux tsparse tcpserversink"
  "hls:mpegtsmux tsparse hlssink"
  "srt:mpegtsmux tsparse srtsink"
  "rtmp/youtube:flvmux rtmpsink"
  "rtsp-push:rtspclientsink"
  "fifo-debug:mpegtsmux filesink"
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
  echo "GStreamer transcoding is unavailable. Missing required elements/tools:"
  printf '  - %s\n' "${missing[@]}"
  echo "Install GStreamer plugins base/good/bad/ugly and gstreamer1.0-libav."
  exit 1
fi

echo "GStreamer transcoder core dependencies are available."
echo "  Launcher: $(command -v gst-launch-1.0)"
echo "  Video encoder: x264enc"
echo "  AAC encoder: ${aac_encoder}"
echo "  MP3 encoder: ${mp3_encoder:-not available}"
echo
echo "Protocol output elements:"
for entry in "${protocol_elements[@]}"; do
  protocol="${entry%%:*}"
  elements="${entry#*:}"
  ok=true
  missing_list=()
  for element in $elements; do
    if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
      ok=false
      missing_list+=("$element")
    fi
  done
  if $ok; then
    printf '  [ok]      %s\n' "$protocol"
  else
    printf '  [missing] %s -> %s\n' "$protocol" "${missing_list[*]}"
  fi
done
