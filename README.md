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

TVStreamer5 can be built and run entirely in Docker. This keeps compiler and
GStreamer development packages out of the host operating system. The runtime
container still uses the host network because IPTV multicast, RTP, SRT listener
mode and interface-specific bindings work most reliably with `--network host`.

### Install Docker Engine on Ubuntu

The recommended production installation uses Docker's official `apt`
repository. The commands below are suitable for supported Ubuntu releases such
as 22.04 LTS and 24.04 LTS.

Remove packages that can conflict with the official Docker Engine packages:

```bash
sudo apt remove -y docker.io docker-compose docker-compose-v2 docker-doc \
  docker-buildx podman-docker containerd runc || true
```

Add Docker's signing key and repository:

```bash
sudo apt update
sudo apt install -y ca-certificates curl

sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
  -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

sudo tee /etc/apt/sources.list.d/docker.sources >/dev/null <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}")
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF

sudo apt update
```

Install Docker Engine, Buildx and the Compose plugin:

```bash
sudo apt install -y \
  docker-ce \
  docker-ce-cli \
  containerd.io \
  docker-buildx-plugin \
  docker-compose-plugin
```

Enable Docker at boot and verify the daemon:

```bash
sudo systemctl enable --now docker
sudo systemctl status docker --no-pager --full
sudo docker run --rm hello-world
```

Docker commands require `sudo` by default. To allow the current user to run
Docker commands without `sudo`:

```bash
sudo usermod -aG docker "$USER"
newgrp docker
docker version
```

Membership in the `docker` group effectively grants root-level access to the
host. Keep using `sudo docker ...` instead if that is preferable for the server.

### Build the TVStreamer5 Docker image

From the repository directory:

```bash
cd /home/monk/TVStreamer5
chmod +x scripts/build_container.sh scripts/run_container.sh
./scripts/build_container.sh
```

The default image name is:

```text
tvstreamer5:release2
```

Verify that the image exists:

```bash
docker image ls tvstreamer5:release2
docker image inspect tvstreamer5:release2 >/dev/null && echo "TVStreamer5 image OK"
```

The build script can be launched from any working directory because it resolves
the repository path from the script location itself. To use another image name:

```bash
IMAGE_NAME=my-tvstreamer:release2 ./scripts/build_container.sh
```

A direct equivalent build command is:

```bash
docker build --pull -t tvstreamer5:release2 .
```

### Prepare persistent TVStreamer5 data

For a production container, keep configuration and application data outside the
container. Example:

```bash
sudo mkdir -p /srv/tvstreamer5
sudo cp tvstreamer5-config.json /srv/tvstreamer5/tvstreamer5-config.json
sudo chown -R "$USER":"$USER" /srv/tvstreamer5
```

The `/srv/tvstreamer5` directory can then persist:

```text
tvstreamer5-config.json
tvstreamer5-subscribers.json
backup-files/
```

### Test the container interactively

The supplied run script starts an interactive temporary container:

```bash
CONFIG_FILE=/srv/tvstreamer5/tvstreamer5-config.json \
  ./scripts/run_container.sh
```

Press `Ctrl+C` to stop it. Because this helper uses `--rm`, the temporary
container is automatically removed after exit; configuration and other data
remain on the host.

To increase GStreamer logging temporarily:

```bash
GST_DEBUG=2 \
CONFIG_FILE=/srv/tvstreamer5/tvstreamer5-config.json \
  ./scripts/run_container.sh
```

### Run TVStreamer5 as a background Docker service

Create the persistent production container:

```bash
docker run -d \
  --name tvstreamer5 \
  --restart unless-stopped \
  --init \
  --network host \
  -v /srv/tvstreamer5:/data \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreamer5:release2
```

The web interface then uses the HTTP port configured by TVStreamer5, normally:

```text
http://SERVER_IP:9000
```

`--network host` is intentional. With host networking, Docker does not need
`-p 9000:9000`, `-p` mappings for UDP multicast, or separate SRT port
forwarding. TVStreamer5 binds directly to the host's interfaces and ports.

### Docker management from the console

The following commands manage the `tvstreamer5` container directly from a Linux
console. If the current user is not in the `docker` group, prefix each
`docker ...` command with `sudo`. Commands that stop or restart the Docker daemon
affect **all** containers on the host, not only TVStreamer5.

```bash
# Show Docker Engine status
systemctl status docker --no-pager --full

# Start / stop / restart the Docker daemon
sudo systemctl start docker
sudo systemctl stop docker
sudo systemctl restart docker

# Enable / disable Docker daemon autostart
sudo systemctl enable docker
sudo systemctl disable docker

# Show all running containers
docker ps

# Show running and stopped containers
docker ps -a

# Show only TVStreamer5
docker ps -a --filter name=tvstreamer5

# Start TVStreamer5
docker start tvstreamer5

# Stop TVStreamer5 gracefully
docker stop -t 15 tvstreamer5

# Restart TVStreamer5
docker restart -t 15 tvstreamer5

# Show container state, exit code and restart count
docker inspect tvstreamer5 --format \
  'status={{.State.Status}} running={{.State.Running}} exit={{.State.ExitCode}} restart={{.RestartCount}}'

# Follow logs in real time
docker logs -f tvstreamer5

# Show the last 200 log lines
docker logs --tail 200 tvstreamer5

# Show logs from the last 10 minutes
docker logs --since 10m tvstreamer5

# Show CPU, RAM and network usage
docker stats tvstreamer5

# Open a shell inside the running container
docker exec -it tvstreamer5 bash

# Show processes running in the TVStreamer5 container
docker top tvstreamer5

# Check GStreamer plugins inside the container
docker exec tvstreamer5 gst-inspect-1.0 x264enc
docker exec tvstreamer5 gst-inspect-1.0 srtsink
docker exec tvstreamer5 gst-inspect-1.0 rtspclientsink

# Check the config visible inside the container
docker exec tvstreamer5 ls -lah /data
docker exec tvstreamer5 test -f /data/tvstreamer5-config.json && echo "config OK"

# Inspect mounted host directories
docker inspect tvstreamer5 --format '{{json .Mounts}}'

# Inspect container network mode
docker inspect tvstreamer5 --format '{{.HostConfig.NetworkMode}}'

# Show the image used by the container
docker inspect tvstreamer5 --format '{{.Config.Image}}'

# Show installed TVStreamer5 images
docker image ls 'tvstreamer5*'

# Show Docker disk usage
docker system df
```

### Update TVStreamer5 in Docker

After pulling new source code, rebuild the image and recreate the container. A
running container does not automatically switch to a newly built image.

```bash
cd /home/monk/TVStreamer5
git pull origin main

./scripts/build_container.sh

docker stop -t 15 tvstreamer5
docker rm tvstreamer5

docker run -d \
  --name tvstreamer5 \
  --restart unless-stopped \
  --init \
  --network host \
  -v /srv/tvstreamer5:/data \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreamer5:release2

# Verify the new container
docker ps --filter name=tvstreamer5
docker logs --tail 100 tvstreamer5
```

The bind-mounted `/srv/tvstreamer5` directory is not removed when the container
is recreated, so the configuration, subscriber database and backup files remain
persistent.

### Change the configuration and restart

Edit the host copy of the configuration:

```bash
sudoedit /srv/tvstreamer5/tvstreamer5-config.json
```

Then restart the container:

```bash
docker restart -t 15 tvstreamer5
docker logs --tail 100 tvstreamer5
```

### Remove or recreate the container

Removing the container does not remove `/srv/tvstreamer5` because that directory
is a host bind mount.

```bash
docker stop -t 15 tvstreamer5
docker rm tvstreamer5
```

Force-remove a stuck container only when a normal stop does not work:

```bash
docker rm -f tvstreamer5
```

Remove an old TVStreamer5 image after the container using it has been removed:

```bash
docker image rm tvstreamer5:release2
```

Remove only unused Docker objects:

```bash
docker container prune
docker image prune
docker builder prune
```

Do not use `docker system prune -a --volumes` on a production server unless you
explicitly intend to delete all unused images, networks, build cache and unused
Docker volumes.

### Docker troubleshooting

If the container immediately exits:

```bash
docker ps -a --filter name=tvstreamer5
docker inspect tvstreamer5 --format 'exit={{.State.ExitCode}} error={{.State.Error}}'
docker logs --tail 300 tvstreamer5
```

If port 9000 is already occupied:

```bash
sudo ss -lntp | grep ':9000'
```

If UDP multicast or SRT traffic is missing, first verify that host networking is
actually enabled and inspect the host interface directly:

```bash
docker inspect tvstreamer5 --format '{{.HostConfig.NetworkMode}}'
ip -br addr
ip route
sudo tcpdump -ni eth0 udp
```

Replace `eth0` with the real IPTV interface. Since TVStreamer5 uses host
networking, protocol diagnostics are performed on the host network namespace.

If the image needs to be rebuilt without Docker layer cache:

```bash
cd /home/monk/TVStreamer5
docker build --pull --no-cache -t tvstreamer5:release2 .
```

If Docker itself is unhealthy:

```bash
sudo systemctl status docker --no-pager --full
sudo journalctl -u docker -n 200 --no-pager
docker info
```

Docker's official Ubuntu installation documentation should be checked when
upgrading the host to a new Ubuntu release because supported distributions and
package names can change.

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
