# TVStreamer5

TVStreamer5 receives SRT/HTTP/UDP/RTP MPEG-TS streams, optionally remaps
service/PID metadata, and outputs MPEG-TS over UDP.

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
directory.

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

The run script uses `--network host` so SRT, UDP, RTP, multicast, and the web UI
can use the host network directly. The application reads
`tvstreamer5-config.json` from the mounted project directory.

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

For UDP and multicast, `--network host` is the recommended mode. It gives the
container access to the host network namespace, so TVStreamer5 can see all host
interfaces and bind UDP input/output to the interface selected in the web UI.
Avoid Docker bridge port mappings for MPEG-TS UDP/multicast; they usually add
loss, jitter, or do not forward multicast correctly.

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
srt://192.168.1.10:9000
http://192.168.1.10:8080/stream.ts
udp://@:1234
udp://239.1.1.1:1234
rtp://239.1.1.1:5004
test://bars
```

For SRT, set `input_mode` to:

```text
caller
listener
auto
```

For remapping, enable `remap_enabled` and set `video_pid`, `audio_pid`,
`service_id`, `service_name`, and `service_provider` in `tvstreamer5-config.json`
or through the web UI.
