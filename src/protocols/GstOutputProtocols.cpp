#include "protocols/GstOutputProtocols.h"

#include "utils.h"

#include <filesystem>
#include <sstream>

namespace tvs::protocols {
namespace {

void addQueue(std::vector<std::string>& args, const std::string& name, uint64_t maxTimeNs = 3000000000ULL) {
    args.insert(args.end(), {
        "queue",
        "name=" + name,
        "max-size-buffers=0",
        "max-size-bytes=0",
        "max-size-time=" + std::to_string(maxTimeNs)
    });
}

std::string safeHost(const std::string& host, const std::string& fallback) {
    return host.empty() ? fallback : host;
}

std::string listenerBindHost(const StreamConfig& cfg) {
    if (!cfg.interfaceAddress.empty()) return cfg.interfaceAddress;
    if (!cfg.outputHost.empty() && cfg.outputHost != "0.0.0.0" && cfg.outputHost != "::") return cfg.outputHost;
    return "0.0.0.0";
}

void appendMpegTsMux(std::vector<std::string>& args, const StreamConfig& cfg) {
    args.insert(args.end(), {
        "mpegtsmux",
        "name=mux",
        "alignment=7",
        "bitrate=" + std::to_string(muxBitrate(cfg)),
        "pat-interval=9000",
        "pmt-interval=9000",
        "pcr-interval=3600",
        "si-interval=9000",
        "!"
    });
    addQueue(args, "transcode_output_queue", 3000000000ULL);
    args.push_back("!");
}

void assignTsPads(const StreamConfig& cfg, GstOutputSpec& spec) {
    spec.videoPad = cfg.videoPid > 0 ? "mux.sink_" + std::to_string(cfg.videoPid) : "mux.";
    spec.audioPad = cfg.audioPid > 0 ? "mux.sink_" + std::to_string(cfg.audioPid) : "mux.";
}

bool appendUdpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    args.insert(args.end(), {
        "udpsink",
        "host=" + safeHost(cfg.outputHost, "127.0.0.1"),
        "port=" + std::to_string(cfg.outputPort),
        "sync=true",
        "async=false",
        "auto-multicast=true",
        "ttl-mc=32",
        "buffer-size=4194304"
    });
    if (!cfg.interfaceAddress.empty()) {
        args.push_back("bind-address=" + cfg.interfaceAddress);
    }
    assignTsPads(cfg, spec);
    spec.description = normalizedOutputType(cfg) + "@udp://" + safeHost(cfg.outputHost, "127.0.0.1") + ":" + std::to_string(cfg.outputPort);
    return true;
}

bool appendSrtSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    const std::string mode = srtOutputMode(cfg);
    const bool caller = mode == "caller";
    const std::string host = caller ? safeHost(cfg.outputHost, "127.0.0.1") : listenerBindHost(cfg);
    const std::string uri = "srt://" + host + ":" + std::to_string(cfg.outputPort) + "?mode=" + mode;
    args.insert(args.end(), {
        "srtsink",
        "uri=" + uri,
        "sync=false",
        "async=false"
    });
    assignTsPads(cfg, spec);
    spec.description = "srt@" + uri;
    return true;
}

bool appendHttpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    // gst-launch cannot attach to TVStreamer5's multifdsink-based HTTP server.
    // This module provides a raw MPEG-TS TCP listener so transcoded HTTP-mode
    // streams start reliably instead of blocking the whole stream. The in-app
    // HTTP server path can be wired to this module later without changing the
    // transcoder core.
    appendMpegTsMux(args, cfg);
    args.insert(args.end(), {
        "tcpserversink",
        "host=" + listenerBindHost(cfg),
        "port=" + std::to_string(cfg.outputPort),
        "sync=true",
        "async=false"
    });
    assignTsPads(cfg, spec);
    spec.description = "http-ts@tcp://" + listenerBindHost(cfg) + ":" + std::to_string(cfg.outputPort);
    return true;
}

bool appendHlsSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    const std::string dir = hlsDirectory(cfg);
    args.insert(args.end(), {
        "hlssink",
        "playlist-location=" + dir + "/playlist.m3u8",
        "location=" + dir + "/segment%05d.ts",
        "target-duration=4",
        "max-files=8"
    });
    assignTsPads(cfg, spec);
    spec.description = "hls@" + dir + "/playlist.m3u8";
    return true;
}

bool appendFlvSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    args.insert(args.end(), {
        "flvmux",
        "name=mux",
        "streamable=true",
        "latency=0",
        "!"
    });
    addQueue(args, "transcode_output_queue", 3000000000ULL);
    args.insert(args.end(), {
        "!", "rtmpsink",
        "location=" + rtmpOutputLocation(cfg),
        "sync=false",
        "async=false"
    });
    spec.videoPad = "mux.";
    spec.audioPad = "mux.";
    spec.container = ContainerKind::Flv;
    spec.description = normalizedOutputType(cfg) + "@" + rtmpOutputLocation(cfg);
    return true;
}

} // namespace

std::vector<std::string> requiredOutputElements() {
    return {"mpegtsmux", "udpsink", "srtsink", "tcpserversink", "hlssink", "flvmux", "rtmpsink"};
}

std::vector<std::string> requiredElementsForOutput(OutputKind kind) {
    switch (kind) {
        case OutputKind::UdpCbr:
        case OutputKind::UdpVbr:
            return {"mpegtsmux", "udpsink"};
        case OutputKind::Srt:
            return {"mpegtsmux", "srtsink"};
        case OutputKind::Http:
            return {"mpegtsmux", "tcpserversink"};
        case OutputKind::Hls:
            return {"mpegtsmux", "hlssink"};
        case OutputKind::Rtmp:
        case OutputKind::Youtube:
            return {"flvmux", "rtmpsink"};
        default:
            return {};
    }
}

bool appendOutputMuxAndSink(
    std::vector<std::string>& args,
    const StreamConfig& cfg,
    GstOutputSpec& spec,
    std::string& error) {
    spec.kind = outputKind(cfg);
    spec.container = isFlvOutput(spec.kind) ? ContainerKind::Flv : ContainerKind::MpegTs;

    switch (spec.kind) {
        case OutputKind::UdpCbr:
        case OutputKind::UdpVbr:
            return appendUdpSink(args, cfg, spec);
        case OutputKind::Srt:
            return appendSrtSink(args, cfg, spec);
        case OutputKind::Http:
            return appendHttpSink(args, cfg, spec);
        case OutputKind::Hls:
            return appendHlsSink(args, cfg, spec);
        case OutputKind::Rtmp:
        case OutputKind::Youtube:
            return appendFlvSink(args, cfg, spec);
        default:
            error = "unsupported output protocol: " + cfg.outputType;
            return false;
    }
}

} // namespace tvs::protocols
