# TVStreamer5 — Release 2

TVStreamer5 is an IPTV stream router, monitor and transcoder with a built-in web control panel. **Current program version: Release 2.**

The application receives live streams, monitors their state and bitrate, can switch to a backup input, optionally transcodes video/audio with GStreamer, remaps MPEG-TS service metadata where supported, and publishes one or more output formats.

## Main features

- Web control panel with English/Russian interface.
- Start, stop, edit and delete streams without editing JSON manually.
- Primary/backup input switching and automatic return to the primary source.
- Input/output bitrate graphs, CC-error monitoring and interface load monitoring.
- Multiple outputs per stream.
- MPEG-TS PID/service remap for compatible TS paths.
- Optional H.264/AAC or H.264/MP3 transcoding through a clean external `gst-launch-1.0` pipeline.
- YADIF deinterlacing, scaling and fixed 25 fps progressive output for the transcoder.
- HTTP TS and HLS delivery from the built-in HTTP server.
- SRT listener/caller support.
- UDP unicast/multicast VBR and CBR output.
- RTMP/YouTube push and RTSP push.
- Subscriber/IP filtering for HTTP TS, HLS and SRT listener sessions.
- Telegram notifications.
- VLC playlist generation.
- Docker build/run scripts for host-network deployments.

## Release 2 architecture

Normal streams and transcoded streams use separate protocol modules. The normal pipeline remains in-process; transcoding is performed by an external GStreamer process.

```text
Input protocol
   |
   +-- passthrough/remap path --------------------> selected output protocol
   |
   +-- optional GStreamer transcoder
          |
          +-- decode
          +-- YADIF deinterlace
          +-- scale / 25 fps progressive
          +-- x264 H.264
          +-- AAC or MP3 audio
          +-- protocol-specific output module
```

Protocol code is split under `src/protocols/`:

```text
src/protocols/inputs/          transcoder input URI modules
src/protocols/outputs/         transcoder output modules
src/protocols/stream/inputs/   normal stream input modules
src/protocols/stream/outputs/  normal stream output modules
```

The legacy in-process transcoder implementation remains in the source tree for compatibility/fallback work, but the active clean transcoder path is `GstTranscoderProcess` using `gst-launch-1.0`.

## Supported inputs

Typical input URLs:

```text
http://server/live.ts
https://server/live.ts
http://server/live/playlist.m3u8
hls://server/live/playlist.m3u8
udp://@:1234
udp://239.1.1.1:1234
rtp://239.1.1.1:5004
srt://192.168.1.10:9000
rtsp://user:password@192.168.1.10:554/stream1
rtsps://server/stream
rtmp://server/live/camera1
test://bars
```

A local backup file is also supported by the normal stream path. The input interface can be selected separately from the output interface for protocols where GStreamer/Linux provides an explicit bind/interface option.

## Output formats

The web UI currently exposes:

```text
udp-vbr   MPEG-TS over UDP
udp-cbr   paced/CBR MPEG-TS over UDP
srt       MPEG-TS over SRT listener or caller
http      MPEG-TS over HTTP
hls       HLS playlist + MPEG-TS segments
rtsp      RTSP push to an external RTSP server
rtmp      RTMP push
youtube   RTMP push to YouTube Live
```

RTP protocol modules exist in the codebase for MPEG-TS/RTP processing, but Release 2 does not expose RTP output as a selectable item in the current web UI.

HTTP and HLS player URLs use the application HTTP server:

```text
http://SERVER:9000/stream/STREAM_ID.ts
http://SERVER:9000/hls/STREAM_ID/playlist.m3u8
```

RTSP output is **push**, not an embedded RTSP server. `output_host` must point to a server that accepts RTSP publishing.

## Multiple outputs

A stream can have one primary output plus `additional_outputs`. Example:

```json
{
  "output_type": "udp-cbr",
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

## MPEG-TS remap

Enable `remap_enabled` and set the required values:

```json
{
  "remap_enabled": true,
  "video_pid": 258,
  "audio_pid": 257,
  "service_id": 2,
  "service_name": "Channel",
  "service_provider": "Provider"
}
```

For UDP/SRT/HTTP MPEG-TS paths the mux/remap code uses MPEG-TS request pads and service mapping where the selected pipeline supports it.

**Known Release 2 limitation:** exact configured elementary PIDs are not currently guaranteed after **transcoded HLS** segmentation. Verify the generated `.ts` segments with `ffprobe -show_programs -show_streams` when exact HLS PID values are required. Do not rely only on the `.m3u8` playlist to verify remap.

## HLS behavior

Transcoded HLS is generated under:

```text
/tmp/tvstreamer5-hls/<stream-id>/
```

Release 2 uses a three-segment live playlist:

```text
playlist-length = 3
target-duration = 5 seconds
max-files = 5
```

The HLS directory for a stream is cleared when a new transcoded HLS pipeline starts so stale segments from a previous run are not mixed into a new playlist.

## Transcoding

Transcoding is optional. When disabled, TVStreamer5 uses the normal passthrough/remap pipeline.

The current transcoder video path is approximately:

```text
uridecodebin
 -> raw video
 -> videoconvert
 -> deinterlace method=yadif
 -> videoscale method=lanczos
 -> videorate
 -> 25 fps progressive I420
 -> x264enc superfast / zerolatency
 -> h264parse
```

Audio is normalized to 48 kHz stereo before encoding. AAC is the stable/default transcoder path; MP3 is used when explicitly configured and an MP3 encoder is available. In the clean transcoder path, a UI/config value intended as audio `copy` is currently handled as AAC re-encode rather than bit-exact passthrough.

The external transcoder owns its own input socket. Therefore the transcoder input bitrate graph is an application-side estimate while this architecture is used; it is not a direct GStreamer pad-probe measurement of the external process input.

Check installed transcoder/protocol elements with:

```bash
./scripts/check_transcoder_plugins.sh
```

## Build on Ubuntu/Debian

Install only the current build/runtime dependencies:

```bash
./install_deps.sh
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Run from the directory that contains `tvstreamer5-config.json`:

```bash
./build/TVStreamer
```

Default web UI:

```text
http://localhost:9000
```

### Packages installed by `install_deps.sh`

Build libraries:

```text
build-essential
cmake
pkg-config
libgstreamer1.0-dev
libgstreamer-plugins-base1.0-dev
libgstreamer-plugins-bad1.0-dev
libcurl4-openssl-dev
libjsoncpp-dev
libboost-system-dev
libboost-thread-dev
```

Runtime GStreamer packages:

```text
gstreamer1.0-tools
gstreamer1.0-plugins-base
gstreamer1.0-plugins-good
gstreamer1.0-plugins-bad
gstreamer1.0-plugins-ugly
gstreamer1.0-libav
gstreamer1.0-rtsp
ca-certificates
```

`gstreamer1.0-rtsp` is required for `rtspclientsink` used by the RTSP push output module. Development packages for Boost filesystem/program-options, OpenSSL, gst-rtsp-server, Git and wget are not required by the current CMake target and are no longer installed by the dependency script.

## Install as a systemd service

```bash
sudo mkdir -p /opt/tvstreamer5
sudo install -m 755 build/TVStreamer /opt/tvstreamer5/TVStreamer
sudo cp tvstreamer5-config.json /opt/tvstreamer5/
```

Example `/etc/systemd/system/tvstreamer5.service`:

```ini
[Unit]
Description=TVStreamer5 Release 2
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

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now tvstreamer5
sudo systemctl status tvstreamer5 --no-pager --full
```

## Docker

The Docker image is based on Ubuntu 24.04. The build stage contains only the development packages used by the current CMake target. The runtime stage contains the shared libraries and GStreamer plugins used by Release 2.

Build:

```bash
chmod +x scripts/build_container.sh scripts/run_container.sh
./scripts/build_container.sh
```

Run interactively:

```bash
./scripts/run_container.sh
```

The scripts can be launched from any current working directory; they resolve the repository root from their own location.

Default image name:

```text
tvstreamer5:release2
```

Override it when needed:

```bash
IMAGE_NAME=my-tvstreamer:release2 ./scripts/build_container.sh
IMAGE_NAME=my-tvstreamer:release2 ./scripts/run_container.sh
```

Use a different config file:

```bash
CONFIG_FILE=/srv/tvstreamer/tvstreamer5-config.json ./scripts/run_container.sh
```

If the config has a different filename, the run script mounts its containing directory as `/data` and additionally maps the selected file to `/data/tvstreamer5-config.json`. This keeps `tvstreamer5-subscribers.json` and backup files in the same persistent host directory.

Host networking is intentional:

```text
--network host
```

It is strongly recommended for UDP multicast, RTP, SRT listener mode and interface-specific input/output binding. Docker bridge/NAT is not a reliable choice for multicast IPTV transport.

A production detached example:

```bash
docker run -d \
  --name tvstreamer5 \
  --restart unless-stopped \
  --init \
  --network host \
  -v /srv/tvstreamer:/data \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreamer5:release2
```

Useful commands:

```bash
docker logs -f tvstreamer5
docker restart tvstreamer5
docker exec -it tvstreamer5 bash
docker ps --filter name=tvstreamer5
```

## Network tuning for high-bitrate UDP

Example Linux tuning:

```bash
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=134217728
sudo sysctl -w net.core.rmem_default=8388608
sudo sysctl -w net.core.wmem_default=8388608
sudo sysctl -w net.ipv4.udp_rmem_min=131072
sudo sysctl -w net.ipv4.udp_wmem_min=131072
sudo sysctl -w net.core.netdev_max_backlog=50000
```

For multicast, verify routes and the selected interface instead of assuming the default route is correct:

```bash
ip -br addr
ip route get 239.1.1.1
sudo tcpdump -ni eth0 udp port 1234
```

`bad udp cksum` shown by `tcpdump` on the transmitting host can be a TX checksum-offload artifact; receiver-side capture is the better validation point.

## Backup failover

Set `backup_input_uri` to enable source failover. When the current input stops producing data, TVStreamer5 can switch to the backup source and later return to the primary source when it recovers.

Useful fields:

```text
backup_input_uri
backup_input_type
backup_file_loop
auto_start
```

The stream tile displays whether the primary or backup source is active.

## Subscriber access control

Subscriber settings are stored in:

```text
tvstreamer5-subscribers.json
```

When filtering is enabled, HTTP TS, HLS and SRT listener access can be restricted by subscriber IP and stream ID. The web UI can add/remove subscribers, assign streams and reset active sessions.

## Telegram notifications

Configure:

```text
telegram_token
telegram_chat_id
```

Notifications report stream start/stop, source failure, backup switching, recovery, pipeline errors and EOS events.

## Main configuration fields

Example:

```json
{
  "id": "channel-1",
  "name": "Channel 1",
  "input_uri": "udp://239.1.1.1:1234",
  "backup_input_uri": "",
  "input_mode": "auto",
  "input_interface_address": "",
  "output_type": "udp-cbr",
  "output_mode": "listener",
  "output_host": "239.2.2.2",
  "output_port": 1234,
  "interface_address": "",
  "target_bitrate": 7000000,
  "remap_enabled": true,
  "video_pid": 258,
  "audio_pid": 257,
  "service_id": 2,
  "service_name": "Channel 1",
  "service_provider": "TVStreamer5",
  "transcode_enabled": false,
  "transcode_resolution": "1280x720",
  "transcode_video_bitrate": 3500000,
  "transcode_audio_codec": "aac",
  "transcode_audio_bitrate": 192000,
  "additional_outputs": []
}
```

Global settings include `http_port`, `login`, `password`, Telegram settings and stream arrays. The web UI is the recommended configuration editor.

## Diagnostics

Service logs:

```bash
sudo journalctl -u tvstreamer5 -n 200 --no-pager
```

Processes:

```bash
ps aux | grep -Ei 'TVStreamer|gst-launch' | grep -v grep
```

GStreamer capability check:

```bash
./scripts/check_transcoder_plugins.sh
```

Inspect MPEG-TS:

```bash
ffprobe -hide_banner -show_programs -show_streams INPUT
```

Inspect current HLS segment:

```bash
SEG=$(ls -1t /tmp/tvstreamer5-hls/STREAM_ID/segment*.ts | head -1)
ffprobe -hide_banner -show_programs -show_streams "$SEG"
```

## Screenshots

Dashboard:

![TVStreamer5 dashboard](docs/screenshots/dashboard.png)

Stream settings:

![TVStreamer5 stream settings](docs/screenshots/stream-settings.png)

Network monitoring:

![TVStreamer5 network interface load](docs/screenshots/network.png)

## Notes

- Release 2 no longer uses FFmpeg as the active transcoder engine.
- Non-transcoded behavior should be treated as the stable baseline when diagnosing protocol-specific transcoder issues.
- Exact HLS PID preservation after transcoding remains a known limitation and must be verified on generated segments.
- For live IPTV, transport stability depends on source quality, kernel socket buffers, routing, multicast interface selection and available CPU for x264 encoding.
