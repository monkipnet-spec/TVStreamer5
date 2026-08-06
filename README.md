# TVStreamer5

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
software encoding plugins are not installed.

At startup/runtime the application checks the GStreamer element registry for:

- `tsparse`, `tsdemux`, and `decodebin`
- `videoconvert`, `videoscale`, and `videorate`
- `x264enc` and `h264parse`
- `audioconvert`, `audioresample`, and `aacparse`
- `mpegtsmux`
- at least one AAC encoder: `avenc_aac`, `fdkaacenc`, or `voaacenc`

The stream editor shows the detected H.264 and AAC encoders. When any required
element is missing, the **Transcode** checkbox is disabled and the missing
components are listed in the interface. Existing passthrough streams remain
available.

### Ubuntu/Debian installation

Install all build and runtime dependencies with:

```bash
chmod +x install_deps.sh
./install_deps.sh
```

The script installs the GStreamer base, good, bad, ugly, and libav plugin sets.
It then runs the transcoder capability check without preventing installation of
the passthrough server when optional encoding support is unavailable.

Run the capability check manually:

```bash
./scripts/check_transcoder_plugins.sh
```

A successful result reports the selected encoders, for example:

```text
Transcoding dependencies are available.
  Video encoder: x264enc
  Audio encoder: avenc_aac
```

For a manual package installation on Ubuntu:

```bash
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

### Isolated transcoder pipeline architecture

The transcoder is implemented as an independent GStreamer `GstBin` with a single
MPEG-TS sink pad and a single MPEG-TS source pad:

```text
existing input chain
        |
        v
+---------------------------+
| TranscoderPipeline GstBin |
| tsparse -> tsdemux        |
|   video -> decode ->      |
|     scale -> x264 ->      |
|     h264parse             |
|   audio -> decode ->      |
|     AAC encode -> aacparse|
|          \       /        |
|           mpegtsmux       |
|              -> tsparse   |
+---------------------------+
        |
        v
existing output branches
```

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
