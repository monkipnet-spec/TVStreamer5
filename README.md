# TVStreamer5

TVStreamer5 receives RTSP camera streams and SRT/HTTP/HLS/UDP/RTP MPEG-TS
streams, optionally remaps service/PID metadata, monitors input quality,
switches to a backup source when the primary source disappears, and outputs
streams as UDP, SRT listener, HTTP TS, HLS, RTMP, or YouTube Live.

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
backup source status in one place.

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
sudo sysctl -w net.core.wmem_max=67108864
sudo sysctl -w net.core.rmem_default=8388608
sudo sysctl -w net.core.wmem_default=8388608
sudo sysctl -w net.ipv4.udp_rmem_min=131072
sudo sysctl -w net.ipv4.udp_wmem_min=131072
sudo sysctl -w net.core.netdev_max_backlog=50000
```

TVStreamer5 requests a 64 MiB UDP send socket buffer for MPEG-TS output, so
`net.core.wmem_max` must be at least `67108864` for the full outgoing buffer to
be applied.

If VLC reports skipped frames or the picture breaks up, enable CBR and keep
`target_bitrate` above the real input bitrate. A good starting point is
20-30% above the measured input bitrate; disable CBR for pure passthrough.

UDP output is a transparent pass-through for a clean input stream. It does not
parse, pace, demux, remux, rewrite PID or service information, or add CBR null
packets. The CBR and remap settings are therefore ignored for UDP output. With a
UDP input, each datagram is forwarded directly from `udpsrc` to `udpsink` without
an intermediate queue or forced packet size. SRT input/output latency is 2000 ms,
and input failover waits 15 seconds before declaring the source lost. If
GStreamer reports EOS or a stream error, TVStreamer5 attempts to rebuild the
current pipeline before marking the stream failed or switching to the configured
backup source.

Persist the tuning after reboot:

```bash
sudo tee /etc/sysctl.d/99-tvstreamer5-udp.conf >/dev/null <<'EOF'
net.core.rmem_max=67108864
net.core.wmem_max=67108864
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
ss -u -n -a
sudo tcpdump -ni eth0 udp port 1234
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
udp://@239.1.1.1:1234
udp://239.1.1.1:1234
rtp://239.1.1.1:5004
test://bars
```

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

Each stream has an `output_type` field. Existing configs without this field are
treated as `udp`.

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
UDP:  output_host is the unicast/multicast destination and output_port is UDP
      port. Flussonic-style full URLs like udp://@239.1.1.1:1234 are also
      accepted in output_host; in that case the URL port overrides output_port.
SRT:  output_mode selects listener or caller. In listener mode, output_host is
      the address advertised in the SRT player URL and TVStreamer5 binds SRT to
      interface_address or 0.0.0.0. In caller mode, output_host is the remote
      SRT listener to connect to. output_port is the SRT port in both modes.
HTTP: output_host is the address advertised in the player URL; port is web UI port.
HLS:  output_host is the address advertised in the player URL; port is web UI port.
RTMP: output_host is a full RTMP/RTMPS URL or host; output_port is used for host mode.
YouTube: output_host is the stream key or a full RTMP/RTMPS ingest URL.
```

The web UI lets you choose the output interface. For UDP multicast it is used as
the multicast interface. For UDP unicast it is used as the bind address. For SRT
it is used as the local listener address when supported by the GStreamer SRT
plugin. RTSP and RTMP camera input and RTMP/YouTube output remux common
H.264/H.265/AAC streams without transcoding where supported.

Enable `auto_start` in a stream's settings to start that stream automatically
after TVStreamer5 restarts. Streams with `auto_start` disabled stay stopped.

## Backup Failover

Set `backup_input_uri` to enable source failover. If the active input produces no
data for 2 seconds, TVStreamer5 switches from the primary input to the backup
input. While running on backup, it periodically checks the primary source and
switches back automatically when data appears again.

The stream tile shows the currently active input:

```text
Активный вход: Основной · udp://...
Активный вход: Резерв · srt://...
```

The tile status changes to `Backup` while the backup URL is active.

## Telegram Notifications

Configure `telegram_token` and `telegram_chat_id` in the web UI or config file
to enable notifications. Messages use Telegram HTML formatting and colored
status indicators:

```text
🟢 Поток запущен / основной восстановлен
🟡 Основной поток пропал / идет переключение
🟠 Поток работает с резервного источника
🔵 Проверка основного источника
🔴 Ошибка потока или нет входного сигнала
⚫ GStreamer EOS
⚪ Поток остановлен вручную
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
  "input_mode": "auto",
  "output_type": "udp",
  "output_mode": "listener",
  "output_host": "239.1.1.1",
  "output_port": 1234,
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
