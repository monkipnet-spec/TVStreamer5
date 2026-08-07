### v44 — GStreamer deinterlace and pacing stabilization

Version v44 stabilizes the clean GStreamer transcoder introduced in v43. The
video path now always passes decoded frames through `deinterlace` and forces
progressive `video/x-raw` caps before x264 encoding, so interlaced sources are
not sent to viewers as combed/interlaced output.

The MPEG-TS mux rate is now clamped to at least video bitrate + audio bitrate +
800 kbit/s overhead. This prevents UDP-CBR output from being configured below
the actual encoded stream rate, which could cause freezes or bursts when the UI
target bitrate was lower than the selected transcoding bitrate.

The gst-launch based transcoder also uses larger queues, `audiorate` for steady
audio timestamps, `superfast` x264 preset for lower CPU pressure, and synthetic
input/output bitrate statistics so the web quality graph no longer shows zero
input while the external GStreamer transcoder is running.

# TVStreamer5

### v43 — clean GStreamer transcoder engine

Version v43 removes the FFmpeg process transcoder from the active stream path and replaces it with a clean GStreamer `gst-launch-1.0` based transcoder engine for stable UDP/UDP-CBR/UDP-VBR MPEG-TS output.

The new engine intentionally does **not** reuse the previous in-process `parsebin`/dynamic-pad transcoder path that caused caps-list, audio passthrough and program-map problems. It builds a deterministic pipeline:

```text
uridecodebin
  -> raw video -> videoscale/videorate -> x264enc -> h264parse config-interval=1 -> mpegtsmux
  -> raw audio -> audioconvert/audioresample -> AAC encoder -> aacparse ADTS -> mpegtsmux
  -> udpsink
```

Audio passthrough/copy is deliberately disabled in this stable mode. Even if the UI value is `copy`, the GStreamer engine re-encodes audio to AAC at 48 kHz stereo. This avoids the previous AAC caps-list, PMT and silent-audio problems.

UDP-CBR output uses `mpegtsmux alignment=7`, repeated PAT/PMT/PCR/SI tables, x264 Annex-B byte-stream output, AUD units, repeated SPS/PPS and optional configured video/audio PIDs through `mux.sink_<pid>` pads.



### v41 FFmpeg timing стабилизация

Версия v41 сохраняет FFmpeg-транскодер v40, но меняет параметры запуска FFmpeg для live MPEG-TS входов:

- убран низколатентный `+nobuffer`, из-за которого FFmpeg мог стартовать до получения SPS/PPS;
- добавлены `analyzeduration` и `probesize`, чтобы входной H.264/AAC успевал определиться перед стартом кодирования;
- включён `+discardcorrupt` и `ignore_err` для устойчивости к битым H.264 кадрам во входном live-потоке;
- добавлен `-re`, чтобы HTTP/file inputs не заливали muxer рывками;
- H.264 кодируется без B-frames и с AUD/repeat headers;
- MPEG-TS mux получает `muxdelay`, `muxpreload` и ограничение interleave delta для устранения предупреждений вида `dts < pcr`.

### v40 - FFmpeg transcoder engine without GStreamer transcoding

- Added `FfmpegTranscoderProcess`, a new process-based transcoder engine that runs `ffmpeg` directly instead of building the transcoding path from GStreamer `parsebin`, `decodebin`, `x264enc`, `aacparse`, and `mpegtsmux` elements.
- When `transcode_enabled` is true, TVStreamer5 now starts FFmpeg directly for the configured stream outputs. Non-transcoded streams still use the existing GStreamer passthrough/remap pipeline.
- Supported FFmpeg-transcoded outputs in this first stage: UDP/UDP-CBR, UDP-VBR, SRT listener/caller, HTTP TS listener, HLS files under `/tmp/tvstreamer5-hls/<id>`, RTMP, and YouTube Live.
- Video is encoded with FFmpeg `libx264` CBR settings, 25 fps, repeated H.264 headers, and the selected resolution/bitrate.
- Audio modes are preserved: original audio copy, AAC encoding, and MP3 encoding.
- This removes GStreamer caps/pad negotiation from the transcoder itself. GStreamer remains used for non-transcoded passthrough streams and legacy infrastructure.

### v37 - Safe transcoder rollback for UDP-CBR

- Rolled back the experimental v34 H.264/PID-request changes that could stop UDP-CBR output.
- Restored the last known packet-producing transcoder mux path from v33.
- Kept the v35 fix that allows original audio passthrough to accept non-fixed `audio/mpeg` caps from `parsebin`, including `mpegversion={2,4}` and `stream-format={raw,adts,adif,loas}`.
- Restored the previous UDP multicast sender implementation; multicast interface warnings are not fatal only in versions where the output socket is actually created by configuration.
- Added a video-link log line so audio and video pad linkage can be checked in `journalctl`.


TVStreamer5 receives RTSP camera streams and SRT/HTTP/HLS/UDP/RTP MPEG-TS
streams, optionally remaps service/PID metadata, monitors input quality,
switches to a backup source when the primary source disappears, and outputs
streams as UDP VBR, UDP CBR, SRT listener, HTTP TS, HLS, RTMP, or YouTube Live.

UDP protocol handling is isolated in `src/UdpInput.cpp` and
separate `src/UdpVbrOutput.cpp` and `src/UdpCbrOutput.cpp` modules. Shared UDP
TS datagram/socket handling lives in `src/UdpTsOutput.cpp`; MPEG-TS remap and
CBR mux processing remains in the common stream pipeline.

## Build on the Host

```bash
./install_deps.sh
cmake -S . -B build
cmake --build build --parallel
./build/TVStreamer
```

The web UI is available at:

```text
http://localhost:9000
```

The default config file is `tvstreamer5-config.json` in the current working
directory. The web UI is the recommended way to edit streams because it exposes
input/output interfaces, output format, CBR, PID remap, Telegram settings, and
backup source status in one place. It also provides stream start/stop/delete
controls, live quality and system metrics, protocol-specific player links, a
downloadable VLC playlist, subscriber management, and an English/Russian
interface switch.

The quality chart shows bitrate history together with CC-error history on a
separate right-side axis, using adaptive labels so large error spikes remain
readable. The chart dialog includes selectable auto-refresh intervals, including
a disabled mode, and clicking the chart copies the current chart image to the
clipboard for reports or support messages.

The web UI and its API use HTTP Basic Authentication with the `login` and
`password` values from `tvstreamer5-config.json`. The `/health` endpoint is
available without authentication for health checks. HTTP TS and HLS player
links are also unauthenticated; enable subscriber filtering when HTTP TS access
must be restricted by client IP.

## Screenshots

Dashboard:

![TVStreamer5 dashboard](docs/screenshots/dashboard.png)

Stream settings:

![TVStreamer5 stream settings](docs/screenshots/stream-settings.png)

Network interface load:

![TVStreamer5 network interface load](docs/screenshots/network.png)

## Install on Linux

Build the binary first:

```bash
./install_deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Install it under `/opt/tvstreamer5`:

```bash
sudo mkdir -p /opt/tvstreamer5
sudo cp build/TVStreamer /opt/tvstreamer5/
sudo cp tvstreamer5-config.json /opt/tvstreamer5/
sudo chmod +x /opt/tvstreamer5/TVStreamer
```

Run manually:

```bash
cd /opt/tvstreamer5
./TVStreamer
```

Optional systemd service:

```ini
[Unit]
Description=TVStreamer5
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/tvstreamer5
ExecStart=/opt/tvstreamer5/TVStreamer
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Save it as `/etc/systemd/system/tvstreamer5.service`, then enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now tvstreamer5
sudo systemctl status tvstreamer5
```

## Build and Run Without Installing Dependencies on the Host

Use the container build. GStreamer, Boost, libcurl, jsoncpp, and runtime
GStreamer plugins are installed inside the image. The host only needs Docker or
Podman.

```bash
chmod +x scripts/build_container.sh scripts/run_container.sh
./scripts/build_container.sh
./scripts/run_container.sh
```

The run script uses `--network host` so RTSP inputs, SRT, UDP, RTP, multicast,
HTTP TS, HLS, and the web UI can use the host network directly. The application reads
`tvstreamer5-config.json` from `/data` inside the container. If `CONFIG_FILE`
points to a file with a different name, the run script mounts it as
`/data/tvstreamer5-config.json` automatically.

Equivalent direct Docker command:

```bash
docker run --rm -it \
  --init \
  --network host \
  -v "$(pwd):/data" \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreamer5:local
```

Run as a named background service:

```bash
docker run -d \
  --name tvstreamer5 \
  --restart unless-stopped \
  --init \
  --network host \
  -v "$(pwd):/data" \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreamer5:local
```

Common Docker management commands:

```bash
# Show running containers
docker ps

# Show TVStreamer5 status
docker ps --filter name=tvstreamer5

# Follow logs
docker logs -f tvstreamer5

# Show the last 100 log lines
docker logs --tail 100 tvstreamer5

# Stop/start/restart
docker stop tvstreamer5
docker start tvstreamer5
docker restart tvstreamer5

# Remove the stopped container
docker rm tvstreamer5

# Rebuild the image after updating the source
docker build -t tvstreamer5:local .

# Recreate the container after rebuild
docker stop tvstreamer5
docker rm tvstreamer5
docker run -d --name tvstreamer5 --restart unless-stopped --init --network host \
  -v "$(pwd):/data" -w /data -e GST_DEBUG=1 tvstreamer5:local

# Open a shell inside the running container
docker exec -it tvstreamer5 bash

# Check image/container disk usage
docker system df
```

For UDP, SRT listener mode, HTTP TS, HLS, and multicast, `--network host` is the
recommended mode. It gives the container access to the host network namespace,
so TVStreamer5 can see all host interfaces and bind input/output to the
interface selected in the web UI. Avoid Docker bridge port mappings for
MPEG-TS UDP/multicast; they usually add loss, jitter, or do not forward
multicast correctly.

Recommended host network tuning for high-bitrate UDP:

```bash
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=134217728
sudo sysctl -w net.core.rmem_default=8388608
sudo sysctl -w net.core.wmem_default=8388608
sudo sysctl -w net.ipv4.udp_rmem_min=131072
sudo sysctl -w net.ipv4.udp_wmem_min=131072
sudo sysctl -w net.core.netdev_max_backlog=50000
```

TVStreamer5 requests a 128 MiB UDP send socket buffer for MPEG-TS output, so
`net.core.wmem_max` must be at least `134217728` for the full outgoing buffer to
be applied.

Persist the tuning after reboot:

```bash
sudo tee /etc/sysctl.d/99-tvstreamer5-udp.conf >/dev/null <<'EOF'
net.core.rmem_max=67108864
net.core.wmem_max=134217728
net.core.rmem_default=8388608
net.core.wmem_default=8388608
net.ipv4.udp_rmem_min=131072
net.ipv4.udp_wmem_min=131072
net.core.netdev_max_backlog=50000
EOF
sudo sysctl --system
```

For multicast receive/transmit on a selected interface, replace `eth0` with the
real interface name shown by `ip -br addr`:

```bash
ip -br addr
sudo ip link set dev eth0 multicast on
sudo ip route replace 224.0.0.0/4 dev eth0
sudo sysctl -w net.ipv4.conf.eth0.rp_filter=0
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
```

If several interfaces are used for different streams, repeat the `ip link` and
`rp_filter` commands for each interface. Add only one broad multicast route if
all multicast should leave through one default interface; otherwise let
TVStreamer5 select the output interface in the stream settings.

Useful checks while testing:

```bash
ip -br addr
ip route get 239.1.1.1
ip route get 192.168.148.1
ss -u -n -a
sudo tcpdump -ni eth0 udp port 1234
sudo tcpdump -ni vlan655 host 192.168.148.1
```

Optional variables:

```bash
IMAGE_NAME=tvstreamer5:local ./scripts/build_container.sh
CONFIG_FILE=/path/to/tvstreamer5-config.json ./scripts/run_container.sh
GST_DEBUG=2 ./scripts/run_container.sh
```

## Supported Inputs

Examples:

```text
rtsp://user:password@192.168.1.10:554/stream1
rtsps://192.168.1.10/live
srt://192.168.1.10:9000
rtmp://192.168.1.10/live/camera1
http://192.168.1.10:8080/stream.ts
udp://@:1234
udp://239.1.1.1:1234
rtp://239.1.1.1:5004
test://bars
```

Enable `test_pattern` on a stream to send the built-in color bars instead of the
configured primary/backup input URLs without overwriting those URLs.

RTSP camera input is remuxed to MPEG-TS before the common output pipeline. The
current RTSP path supports common camera payloads: H.264/H.265 video and
AAC/MPA audio. Use the full camera URL, including username and password when the
camera requires authentication.

For SRT, set `input_mode` to:

```text
caller
listener
auto
```

## Output Formats

Each stream has one primary `output_type` field. To broadcast the same input
simultaneously in more than one format, keep the primary `output_type`,
`output_mode`, `output_host`, and `output_port` fields and add
`additional_outputs` entries. Existing configs without these entries keep
working as single-output streams.

```text
udp   MPEG-TS over UDP unicast or multicast
srt   MPEG-TS over SRT listener or caller
http  MPEG-TS over HTTP at /stream/<stream-id>.ts
hls   HLS playlist at /hls/<stream-id>/playlist.m3u8
rtmp  FLV over RTMP push
youtube  FLV over RTMP push to YouTube Live
```

Examples of player URLs shown by the UI:

```text
udp://@239.1.1.1:1234
srt://192.168.1.20:9000
rtmp://live.example.com:1935/live/channel-1
rtmp://a.rtmp.youtube.com/live2/<stream-key>
http://192.168.1.20:9000/stream/channel-1.ts
http://192.168.1.20:9000/hls/channel-1/playlist.m3u8
```

Output host and port meaning depends on the selected format:

```text
UDP:  output_host is the unicast/multicast destination, output_port is UDP port.
SRT:  output_mode selects listener or caller. In listener mode, output_host is
      the address advertised in the SRT player URL and TVStreamer5 binds SRT to
      interface_address or 0.0.0.0. In caller mode, output_host is the remote
      SRT listener to connect to. output_port is the SRT port in both modes.
HTTP: output_host is the address advertised in the player URL; output_port is
      the HTTP TS port. TVStreamer5 listens on this port in addition to the web
      UI port.
HLS:  output_host is the address advertised in the player URL; output_port is
      the HLS port. TVStreamer5 listens on this port in addition to the web UI
      port.
RTMP: output_host is a full RTMP/RTMPS URL or host; output_port is used for host mode.
YouTube: output_host is the stream key or a full RTMP/RTMPS ingest URL.
```

Example with simultaneous UDP multicast, SRT listener, and HLS output:

```json
{
  "output_type": "udp-cbr",
  "output_mode": "listener",
  "output_host": "239.1.1.1",
  "output_port": 1234,
  "additional_outputs": [
    {
      "output_type": "srt",
      "output_mode": "listener",
      "output_host": "0.0.0.0",
      "output_port": 9001
    },
    {
      "output_type": "hls",
      "output_mode": "listener",
      "output_host": "192.168.1.20",
      "output_port": 9000
    }
  ]
}
```

The web UI lets you choose the input interface separately from the output
interface. For UDP multicast, `input_interface_address` is used as the multicast
interface. For UDP/RTP unicast, the receiver binds to that local address, or to
`0.0.0.0` when `Auto / all interfaces` is selected; the URI host is never used as
a local unicast bind address. For SRT input, `input_interface_address` is used as
the SRT `localaddress` when the installed GStreamer SRT plugin supports it. If
`input_interface_address` is absent, older configs can still fall back to
`interface_address`. An explicitly empty value means all active IPv4 interfaces
that support multicast, including VLAN devices such as `enp2s0.123`. To pin UDP
or SRT reception to a VLAN, create and bring up the VLAN device in the host OS,
assign it an IPv4 address, and select that address under `Input interface`.
HTTP/HLS inputs use GStreamer's `souphttpsrc`; current GStreamer versions do not
expose a source-address bind property there, so Linux routing chooses the
interface. To force HTTP/HLS input through a VLAN, add a route to the source
address or network via the VLAN device. For SRT output, `interface_address` is
used as the local listener address when supported by the GStreamer SRT plugin.
RTSP and RTMP camera input and RTMP/YouTube output remux common
H.264/H.265/AAC streams without transcoding where supported.

Enable `auto_start` in a stream's settings to start that stream automatically
after TVStreamer5 restarts. Streams with `auto_start` disabled stay stopped.

## Backup Failover

Set `backup_input_uri` to enable source failover. If the active input produces no
data for 5 seconds, TVStreamer5 switches from the primary input to the backup
input. While running on backup, it periodically checks the primary source and
switches back automatically when data appears again.

The backup can also be a local replacement file. Set `backup_input_type` to
`file`, point `backup_input_uri` to the file path, and set `backup_file_loop` to
`true` when the file should repeat until the primary source returns. The web UI
can upload a selected file into the local `backup-files/` directory and fill that
path automatically.

The stream tile shows the currently active input:

```text
Активный вход: Основной · udp://...
Активный вход: Резерв · srt://...
```

The tile status changes to `Backup` while the backup URL is active.

## Subscriber Access Control

Subscriber settings are stored separately in
`tvstreamer5-subscribers.json`. The web UI can add, enable, disable, and remove
subscribers, assign streams to them, export the current list as
`tvstreamer5-subscribers.txt`, and show the number of active HTTP sessions.
Active sessions can be reset from the subscriber dialog; resetting disconnects
the subscriber's current HTTP TS and SRT sessions.

Set `filtering_enabled` to `true` to restrict HTTP TS, HLS, and SRT listener
playback. A request to `/stream/<stream-id>.ts`, an HLS file under
`/hls/<stream-id>/`, or an SRT caller connection is allowed only when its source
IP matches the enabled subscriber's `primary_ip` or `backup_ip`, and the stream
id is included in that subscriber's `stream_ids`. When filtering is disabled,
stream playback is not restricted by the subscriber list. SRT listener access is
checked against the configured TVStreamer5 stream id, so players do not need to
send an SRT `streamid` value. Saving subscriber changes also closes active HTTP
TS and SRT sessions that no longer match the updated access list.

Example:

```json
{
  "filtering_enabled": true,
  "subscribers": [
    {
      "name": "Subscriber 1",
      "primary_ip": "192.168.1.50",
      "backup_ip": "192.168.1.51",
      "added_at": "2026-08-01",
      "enabled": true,
      "stream_ids": ["channel-1", "channel-2"]
    }
  ]
}
```

The subscriber file is loaded from the current working directory alongside
`tvstreamer5-config.json`. Changes made in the web UI are saved automatically
when the subscriber dialog is saved.

## Telegram Notifications

Configure `telegram_token` and `telegram_chat_id` in the web UI or config file
to enable notifications. Messages use Telegram HTML formatting, colored status
indicators, and the configured UI language (`language`: `en` or `ru`):

```text
🟢 Поток запущен / Stream started
🟡 Основной поток пропал / Primary stream lost
🟠 Поток работает с резервного источника / Running from backup source
🔵 Проверка основного источника / Checking primary source
🔴 Ошибка потока или нет входного сигнала / Stream error or no input signal
⚫ GStreamer EOS
⚪ Поток остановлен вручную / Stream stopped manually
```

Notifications include the stream name, stream ID, human-readable reason, and the
active URL when applicable.

For remapping, enable `remap_enabled` and set `video_pid`, `audio_pid`,
`service_id`, `service_name`, and `service_provider` in `tvstreamer5-config.json`
or through the web UI.

## Important Config Fields

Minimal stream object:

```json
{
  "id": "channel-1",
  "name": "Channel 1",
  "input_uri": "rtsp://user:password@192.168.1.10:554/stream1",
  "backup_input_uri": "srt://192.168.1.10:9000",
  "backup_input_type": "url",
  "backup_file_loop": false,
  "input_mode": "auto",
  "input_interface_address": "",
  "test_pattern": false,
  "output_type": "udp-cbr",
  "output_mode": "listener",
  "output_host": "239.1.1.1",
  "output_port": 1234,
  "additional_outputs": [],
  "interface_address": "",
  "cbr": true,
  "target_bitrate": 7000000,
  "remap_enabled": false,
  "video_pid": 0,
  "audio_pid": 0,
  "service_id": 1,
  "service_name": "",
  "service_provider": ""
}
```

Use `"output_type": "udp-vbr"` for transparent UDP VBR output. The legacy
`"output_type": "udp"` form is still accepted and chooses CBR/VBR from the
`cbr` flag.

The HTTP interface port is configured with `http_port`; the same port serves
the web UI, HTTP TS streams, and HLS files. The `login` and `password` fields
control Basic Authentication for the web UI and API. The UI can generate a VLC
playlist containing all primary and additional output URLs; save it from the
`VLC playlist` control as `tvstreamer5-playlist.m3u`.

## Transcoding dependencies and automatic capability detection

Transcoding is optional. TVStreamer5 continues to run in passthrough mode when
software encoding support is not installed.

The preferred transcoder engine is now GStreamer. At startup/runtime the application
checks for `gst-launch-1.0` and the required GStreamer elements. The stream
editor enables transcoding when these are available and reports:

- video encoder: `x264enc`;
- AAC encoder: `fdkaacenc`, `voaacenc`, or `avenc_aac`;
- MP3 encoder: `lamemp3enc` or `avenc_mp3` when installed.

The stable v43/v44 path re-encodes audio to AAC by default, even if the UI is still
set to `copy`, because AAC passthrough was the source of the earlier no-audio
and broken-PMT problems. Existing passthrough streams remain available even when
no transcoder engine is available.

### Ubuntu/Debian installation

Install all build and runtime dependencies with:

```bash
chmod +x install_deps.sh
./install_deps.sh
```

The script installs the GStreamer base, good, bad, ugly, and libav plugin sets.
For the v43 transcoder engine install the GStreamer runtime tools and plugins.

Run the capability check manually:

```bash
./scripts/check_transcoder_plugins.sh
```

A successful result reports the selected encoders, for example:

```text
GStreamer transcoding dependencies are available.
  Launcher: /usr/bin/gst-launch-1.0
  Video encoder: x264enc
  AAC encoder: voaacenc or avenc_aac
  MP3 encoder: lamemp3enc or avenc_mp3
```

For a manual package installation on Ubuntu:

```bash
sudo apt update
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-good1.0-dev libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav
```

### Docker

The runtime image includes the same GStreamer plugin groups and
`gst-inspect-1.0`. Capability detection therefore works in the container in the
same way as on a native installation.

After changing installed plugins, restart TVStreamer5 so the capability state
shown in the web interface is refreshed.

### GStreamer transcoder process architecture

When `transcode_enabled` is true, the transcoding path no longer uses a
the older in-process GStreamer `GstBin`. TVStreamer5 starts `gst-launch-1.0` directly and lets GStreamer handle
input demuxing, decoding, scaling, encoding, audio copy/encoding, and output
muxing:

```text
input URL / file / network source
        |
        v
+-------------------------------+
| gst-launch-1.0 process        |
|  demux/decode                 |
|  scale/fps/yuv420p            |
|  libx264 CBR                  |
|  audio copy / AAC / MP3       |
|  MPEG-TS / HLS / FLV mux      |
+-------------------------------+
        |
        v
UDP, UDP-CBR, SRT, HTTP TS, HLS, RTMP, YouTube
```

This avoids GStreamer `pad-added`, `capsfilter`, parser, and `mpegtsmux`
negotiation issues in the transcoder. Non-transcoded streams continue to use
the existing GStreamer passthrough and remap pipeline.

The passthrough pipeline and the transcoder pipeline no longer share internal
demuxers, decoders, parsers, or muxers. Unsupported, subtitle, data, and duplicate
program pads are connected to a `fakesink`, preventing `GST_FLOW_NOT_LINKED` from
stopping the complete stream. This is particularly important for multi-program
MPEG-TS inputs and streams with multiple audio tracks.

### AAC encoder format negotiation

The transcoder does not force a fixed raw PCM sample format before the AAC encoder. `audioconvert` negotiates the format supported by the selected encoder (`avenc_aac`, `fdkaacenc`, or `voaacenc`). The encoded branch is normalized to raw AAC before `mpegtsmux`, ensuring that the output MPEG-TS contains a playable AAC audio track.


### Audio codec selection and deinterlacing

The optional transcoder supports selectable output audio encoding:

- AAC-LC: 96, 128, 160, 192, 256, or 320 kbit/s
- MP3: 96, 128, 160, 192, 256, or 320 kbit/s

The selected settings are stored per stream as:

```json
{
  "transcode_audio_codec": "aac",
  "transcode_audio_bitrate": 192000
}
```

Interlaced input video is passed through the GStreamer `deinterlace` element and
encoded as progressive 25 fps video. This prevents combing artifacts when 1080i
or 576i sources are transcoded for HTTP, HLS, SRT, or UDP delivery.

Verify the additional elements:

```bash
gst-inspect-1.0 deinterlace
gst-inspect-1.0 lamemp3enc || gst-inspect-1.0 avenc_mp3
gst-inspect-1.0 mpegaudioparse
```


### Transcoder timeline and output remuxing

The transcoder produces the final MPEG-TS program itself. When transcoding is enabled,
TVStreamer5 does not pass that transport stream through the legacy PID-remap demux/mux
branch a second time. The transcoder mux requests the configured video and audio PIDs and
assigns both elementary streams to the configured service ID. This avoids duplicate H.264
parsing, backward DTS warnings, dropped NAL units, and audio tracks disappearing during a
second remux.

The encoded H.264 stream is normalized to Annex-B byte-stream access units and SPS/PPS are
repeated every second. Video uses `videorate` and audio uses `audiorate`; both branches use
a single running-time segment before entering `mpegtsmux`.


### Direct transcoder MPEG-TS output

The transcoder output is delivered directly from `mpegtsmux` through a bounded `queue`.
No internal output `tsparse` or timestamp rewriting is used after multiplexing. This preserves
the PCR, PTS and DTS timeline generated by `mpegtsmux` and prevents an internal parser with
an uninitialised PCR from blocking or delaying delivery to UDP, UDP-CBR, SRT, HTTP and HLS outputs.

### Transcoder output compatibility fix (v21)

The transcoder output path uses the proven MPEG-TS parser/remux integration from v18.
This restores UDP/UDP-CBR packet delivery while retaining AAC/MP3 selection,
configurable audio bitrate, and progressive deinterlaced video output.


### Audio timestamp and remux compatibility

The transcoder audio branch uses `audiorate` before AAC or MP3 encoding to create a continuous, monotonic 48 kHz timeline. The output remuxer reads the `audio/mpeg` caps fields directly instead of relying on one textual caps representation, so both AAC (`mpegversion=4`) and MP3 (`mpegversion=1`, layer 3) are preserved in the final MPEG-TS program. H.264 SPS/PPS are repeated every second for reliable mid-stream client joins.


### Encoded audio timestamp normalization

The transcoder normalizes AAC and MP3 PTS/DTS after the audio parser and before MPEG-TS muxing. This prevents encoder delay or discontinuous source timestamps from making DTS move backwards, which could otherwise cause audio to be heard briefly after restart and then disappear.


### Transcoded audio stability

The final MPEG-TS remux keeps one stable program map containing both video and audio. AAC remains in the native raw format produced by `aacparse`, preserving the negotiated `codec_data`/AudioSpecificConfig required by MPEG-TS receivers.

### AAC decoder configuration fix

The AAC branch uses the PCM format required by the selected encoder. `avenc_aac` receives interleaved `F32LE`, 48 kHz stereo audio, while compatible native encoders receive `S16LE`. After encoding, `aacparse` outputs framed raw AAC and preserves `codec_data` (AudioSpecificConfig). The code no longer declares raw AAC buffers as ADTS without actually adding ADTS headers, which previously produced tracks detected as AAC with `0 channels` and no sound.


### MP3 encoder configuration fix

The MP3 transcoding path is independent from AAC. `lamemp3enc` is configured in bitrate-target mode with CBR enabled and receives its bitrate in kbit/s. The libav `avenc_mp3` fallback receives non-interleaved planar `S16P` PCM, while `lamemp3enc` receives interleaved `S16LE`; both use 48 kHz stereo input. After `mpegaudioparse`, the mux receives parsed MPEG-1 Layer III caps with the sample rate and channel count preserved.

### AAC encoder negotiation fix (v30)

AAC transcoding no longer forces encoded caps after `aacparse`. The parser now
negotiates directly with `mpegtsmux`, preserving `codec_data` / AudioSpecificConfig
instead of exposing an AAC PID with unknown sample rate or zero channels.

AAC encoder preference is now:

1. `fdkaacenc`
2. `voaacenc`
3. `avenc_aac` (fallback)

The MP3 path remains separate and keeps its parsed MPEG-1 Layer III caps.


### Original audio passthrough

The transcoder audio codec selector includes **Original audio passthrough** (`transcode_audio_codec: "copy"`). In this mode only the video is decoded, deinterlaced, scaled, and encoded. The first supported original audio track is remuxed without decoding or re-encoding. AAC, MPEG Layer I/II/III, AC-3, and E-AC-3 inputs are parsed and passed to the transcoder MPEG-TS mux. The audio bitrate selector is disabled because the source audio bitrate is preserved. Unsupported audio formats are logged and safely ignored rather than stopping the video pipeline.


### v32: final remux audio/program fix

The final MPEG-TS remux no longer forces AAC to ADTS by caps declaration. `aacparse` now negotiates its actual output directly with `mpegtsmux`, so encoded or copied AAC frames are not mislabeled. The remux also uses the default single-program mapping from `mpegtsmux` instead of applying a live `prog-map` after pads are active. This keeps video and audio in the same MPEG-TS program and fixes players seeing a video program and a separate silent audio program.


### v33: universal transcoder input/output routing

The transcoder now uses `parsebin` at its input instead of a fixed `tsparse -> tsdemux`
front end. This lets the same transcoder bin accept MPEG-TS produced from UDP, SRT,
HTTP TS, HLS, RTSP, RTMP, and file inputs after the normal source chain. Encoded video
pads are decoded for scaling and H.264 re-encoding, while the **Original audio passthrough**
mode keeps the first supported encoded audio pad and only parses/remuxes it.

When transcoding is enabled, UDP, UDP-CBR, SRT, HTTP TS, and HLS outputs all receive the
same transcoded MPEG-TS program through the normal passthrough output branch. The legacy
PID-remap demux/remux branch is skipped for transcoded TS outputs because it can remove the
copied audio PID or split audio and video into separate programs. RTMP/YouTube outputs still
use their required TS-to-FLV remux branch.

HLS input is also normalized through `mpegtsmux` before it reaches the transcoder, so HLS
video and audio pads are kept together instead of linking only the first demuxed pad.

### v34: H.264/TS stabilisation for transcoder output

The transcoder now forces the encoded H.264 output to `stream-format=byte-stream` and
`alignment=au` before it reaches `mpegtsmux`. `x264enc` is configured for live output with
B-frames disabled and repeated headers enabled, while `h264parse` inserts SPS/PPS on every
IDR frame. This fixes multicast receivers joining an already running transcoded stream and
seeing H.264 slices before SPS/PPS.

The transcoder mux also publishes PAT, PMT, SDT/SI and PCR at short intervals and requests
the configured video/audio PIDs on its own mux sink pads. This keeps the generated MPEG-TS
self-describing for UDP-CBR, UDP, SRT, HTTP TS, HLS and RTMP/YouTube remux outputs.

### v35: audio-copy caps and multicast-interface resilience

The original-audio passthrough path now accepts non-fixed `audio/mpeg` caps from
`parsebin`, including lists and ranges such as `mpegversion={2,4}` and
`stream-format={raw,adts,adif,loas}`. The previous strict integer-only check could
reject a valid AAC input pad before it reached `aacparse`, which removed the audio
branch from the transcoder output. v35 inspects GStreamer `GValue` lists/ranges and
selects `aacparse` or `mpegaudioparse` accordingly.

UDP/UDP-CBR multicast output is also more tolerant when selecting the outgoing
interface. TVStreamer5 now binds the multicast sender socket to the configured source
address, retries `IP_MULTICAST_IF` using the interface index on Linux, and falls back
to the kernel multicast route with a warning instead of stopping the output if the
interface-specific socket option is rejected.


### v42 — FFmpeg H.264 Annex-B output hardening

The FFmpeg transcoder now prints the complete generated ffmpeg command to the service log before starting each child process. MPEG-TS outputs force H.264 codec extradata to be dumped at every key frame with `dump_extra=freq=keyframe`, keep x264 headers repeated, disable global-header-only output, and use zero mux delay/preload with immediate packet flushing. This is intended to make multicast joins self-describing when `ffprobe` or VLC join an already running UDP-CBR stream.

### v45 protocol module split

The clean GStreamer transcoder is now split into protocol modules:

- `src/protocols/GstInputProtocols.*` builds input protocol handling for `uridecodebin` based inputs, including HLS URL normalization.
- `src/protocols/GstOutputProtocols.*` builds output mux/sink chains for UDP/UDP-CBR/UDP-VBR, SRT, HLS, RTMP/YouTube and raw MPEG-TS TCP output for HTTP-mode streams.
- `src/protocols/GstProtocolTypes.*` owns protocol normalization, per-output config expansion, bitrate safety limits and shared URL/path helpers.

The transcoder core now only builds the common decode -> deinterlace -> scale -> encode video/audio chain. Protocol-specific sink details are isolated in output modules, so adding or repairing a protocol no longer changes the encoder path.

For stable mode, audio is still re-encoded to AAC by default even when the UI says copy. This avoids the previous AAC passthrough caps/PMT/silent-audio failure mode.

HTTP note: the external `gst-launch-1.0` transcoder cannot attach directly to TVStreamer5's in-process `multifdsink` HTTP session manager. For HTTP-mode transcoded output v45 starts a raw MPEG-TS TCP listener using `tcpserversink`. HLS remains available through TVStreamer5's `/hls/<id>/playlist.m3u8` file server.

## v46 - modular protocol engine and HTTP relay for transcoded streams

v46 splits the clean GStreamer transcoder protocol layer into explicit input and output modules:

- input modules: HTTP, HLS, UDP, SRT, RTSP and RTMP URI handling;
- output modules: UDP/UDP-CBR/UDP-VBR, HTTP TS, HLS, SRT, RTSP push, RTMP push and YouTube push;
- common output helpers: MPEG-TS muxing, PID pad assignment, queue/sink buffering and deterministic mux bitrate.

The transcoded HTTP output no longer tries to bind the public TVStreamer5 web port from `gst-launch-1.0`. Instead, the GStreamer process writes MPEG-TS to an internal localhost TCP relay port. The built-in HTTP server keeps the normal public URL:

```text
http://<server>:<http_port>/stream/<stream_id>.ts
```

and proxies that request to the internal transcoder relay. This fixes the previous case where a transcoded HTTP stream either did not start or had to be opened as raw `tcp://` instead of real HTTP.

The output queues for HTTP, HLS, SRT and RTMP/YouTube are leaky downstream queues, so a slow or missing client cannot freeze the video encoder. UDP keeps the normal paced MPEG-TS path.

RTSP output is implemented as RTSP push through `rtspclientsink`. It requires an external RTSP server target. RTSP server/listener mode is intentionally not exposed as a fake TCP stream.

### v47 — stable transcoded protocol relay

Transcoded streams now use the clean GStreamer transcoder only to produce a local
MPEG-TS relay on `udp://127.0.0.1:<internal-port>`. TVStreamer5 then reads that
relay with the same passthrough pipeline used by non-transcoded streams and sends
it through the selected output protocol modules.

This keeps the stable non-transcoded HTTP, HLS, SRT, UDP, RTP, RTMP/YouTube and
RTSP protocol behavior unchanged, while avoiding the previous problems caused by
making the external `gst-launch-1.0` process talk directly to each client-facing
protocol.

The practical flow is:

```text
input protocol -> clean GStreamer transcode -> local UDP TS relay -> TVStreamer5 passthrough output modules -> selected protocol
```

Non-transcoded streams are not transcoded or rerouted; their existing protocol
behavior is preserved.

### v48 - FIFO transcode relay and stream protocol modules
- Replaced the internal UDP transcode relay from v47 with a local FIFO MPEG-TS relay under `/tmp/tvstreamer5-relay/`.
- The GStreamer transcoder now writes the completed MPEG-TS stream to a FIFO through `filesink`, and the normal TVStreamer5 pipeline reads the same FIFO through the existing file/TS input path.
- This avoids the localhost UDP relay startup race that could mark transcoded streams as `no input signal`.
- Non-transcoded streams are unchanged.
- Transcoded streams still reuse the stable normal output path for UDP, RTP, HTTP, HLS, SRT, RTMP/YouTube and RTSP after the FIFO relay.
- Added stream input/output protocol classification modules for the remaining normal pipeline protocols so protocol routing is no longer hidden only inside `StreamManager` conditionals.
- Added a dedicated FIFO relay output protocol module for the transcoder process.

### v49 - Direct modular transcoder protocol outputs

- Reverted the broken FIFO transcode relay path that could make every transcoded stream report `no input signal`.
- The clean GStreamer transcoder now outputs directly through protocol-specific modules again.
- Kept the normal non-transcoded input/output protocol modules unchanged.
- Added a dedicated transcoded RTP MPEG-TS output module through `rtpmp2tpay`.
- Added a dedicated transcoded RTP input classifier module.
- HTTP transcoded TS output uses an internal localhost `tcpserversink`; TVStreamer5 proxies `/stream/<id>.ts` to that sink so clients still use the normal HTTP URL.
- HLS transcoded output writes segments under `/tmp/tvstreamer5-hls/<id>` and the existing HTTP server serves `/hls/<id>/playlist.m3u8`.
- Removed downstream-leaky queues from direct protocol outputs to avoid corrupt TS bursts, frozen pictures and audio crackling.
- Switched the deinterlacer method to `yadif` for the clean GStreamer transcoder video branch.


## v50 transcoder transport stabilization

The GStreamer transcoder keeps protocol outputs modular, but the MPEG-TS based outputs now share the same transport rules:

- UDP/RTP/SRT/HTTP/HLS run the encoded TS through `tsparse set-timestamps=true` with live timestamp smoothing before the network/file sink.
- SRT is clock-synchronised and uses a 400 ms SRT latency so buffers are not dumped as fast as possible.
- HLS remap is applied in `mpegtsmux`: requested video/audio PIDs are mapped to the configured service/program with `prog-map`.
- Deinterlace uses YADIF with one output field per source frame (`fields=top`) before the fixed 25 fps stage, avoiding the previous 50->25 field-rate churn.
- The transcoder monitor thread now runs for external GStreamer pipelines, so the input bitrate graph remains populated while transcoding.

For a remapped HLS stream, `ffprobe -show_programs -show_streams` should show the configured service ID and V-PID/A-PID in every segment.
