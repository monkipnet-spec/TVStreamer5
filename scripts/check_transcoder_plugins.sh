#!/usr/bin/env bash
set -euo pipefail

# v48: the clean GStreamer transcoder always outputs to an internal
# FIFO MPEG-TS relay. TVStreamer5 then uses its existing passthrough protocol
# modules for HTTP, HLS, SRT, UDP/RTP, RTMP/YouTube and RTSP. Therefore the
# transcoder hard requirement is only the decode/encode/mux/FIFO-relay chain.
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
  mpegtsmux
  filesink
)

optional_protocols=(
  srtsrc
  srtclientsrc
  srtsink
  souphttpsrc
  hlsdemux
  hlssink
  multifdsink
  rtspsrc
  rtspclientsink
  rtmpsrc
  flvdemux
  flvmux
  rtmpsink
  rtpmp2tdepay
  rtpmp2tpay
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
  echo "GStreamer transcoding relay is unavailable. Missing required elements/tools:"
  printf '  - %s\n' "${missing[@]}"
  echo "Install GStreamer plugins base/good/bad/ugly and gstreamer1.0-libav."
  exit 1
fi

echo "GStreamer transcoding relay dependencies are available."
echo "  Launcher: $(command -v gst-launch-1.0)"
echo "  Video encoder: x264enc"
echo "  AAC encoder: ${aac_encoder}"
echo "  MP3 encoder: ${mp3_encoder:-not available}"
echo "  Relay output: mpegtsmux + filesink FIFO"
echo
echo "Optional protocol elements used by passthrough input/output modules:"
for element in "${optional_protocols[@]}"; do
  if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    printf '  [ok]      %s\n' "$element"
  else
    printf '  [missing] %s\n' "$element"
  fi
done
