#include "protocols/GstProtocolTypes.h"

#include "utils.h"

#include <algorithm>
#include <filesystem>
#include <functional>

namespace tvs::protocols {

std::string normalizedOutputType(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type == "udp_vbr" || type == "udpvbr") {
        type = "udp-vbr";
    } else if (type == "udp_cbr" || type == "udpcbr") {
        type = "udp-cbr";
    }
    if (type == "udp") {
        return cfg.cbr ? "udp-cbr" : "udp-vbr";
    }
    return type;
}

OutputKind outputKind(const StreamConfig& cfg) {
    const std::string type = normalizedOutputType(cfg);
    if (type == "udp-cbr") return OutputKind::UdpCbr;
    if (type == "udp-vbr") return OutputKind::UdpVbr;
    if (type == "srt") return OutputKind::Srt;
    if (type == "http") return OutputKind::Http;
    if (type == "hls") return OutputKind::Hls;
    if (type == "rtsp") return OutputKind::Rtsp;
    if (type == "rtmp") return OutputKind::Rtmp;
    if (type == "youtube") return OutputKind::Youtube;
    return OutputKind::Unknown;
}

bool isUdpOutput(OutputKind kind) {
    return kind == OutputKind::UdpCbr || kind == OutputKind::UdpVbr;
}

bool isTsOutput(OutputKind kind) {
    return kind == OutputKind::UdpCbr || kind == OutputKind::UdpVbr ||
           kind == OutputKind::Srt || kind == OutputKind::Http || kind == OutputKind::Hls;
}

bool isFlvOutput(OutputKind kind) {
    return kind == OutputKind::Rtmp || kind == OutputKind::Youtube;
}

bool isRtspOutput(OutputKind kind) {
    return kind == OutputKind::Rtsp;
}

StreamOutputConfig primaryOutputConfig(const StreamConfig& cfg) {
    StreamOutputConfig output;
    output.outputType = cfg.outputType;
    output.outputMode = cfg.outputMode;
    output.outputHost = cfg.outputHost;
    output.outputPort = cfg.outputPort;
    return output;
}

StreamConfig configForOutput(const StreamConfig& base, const StreamOutputConfig& output) {
    StreamConfig cfg = base;
    cfg.outputType = output.outputType;
    cfg.outputMode = output.outputMode;
    cfg.outputHost = output.outputHost;
    cfg.outputPort = output.outputPort;
    cfg.additionalOutputs.clear();

    const OutputKind kind = outputKind(cfg);
    if (kind == OutputKind::UdpCbr) cfg.cbr = true;
    if (kind == OutputKind::UdpVbr) cfg.cbr = false;
    return cfg;
}

std::vector<StreamConfig> outputConfigs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    outputs.push_back(configForOutput(cfg, primaryOutputConfig(cfg)));
    for (const auto& output : cfg.additionalOutputs) {
        outputs.push_back(configForOutput(cfg, output));
    }
    return outputs;
}

uint64_t safeVideoBitrate(const StreamConfig& cfg) {
    return std::max<uint64_t>(cfg.transcodeVideoBitrate, 500000);
}

uint64_t safeAudioBitrate(const StreamConfig& cfg) {
    return std::clamp<uint64_t>(cfg.transcodeAudioBitrate, 64000, 320000);
}

uint64_t muxBitrate(const StreamConfig& cfg) {
    const uint64_t video = safeVideoBitrate(cfg);
    const uint64_t audio = safeAudioBitrate(cfg);
    const uint64_t minimum = video + audio + 1200000;
    if (cfg.targetBitrate > 0) {
        return std::max<uint64_t>(cfg.targetBitrate, minimum);
    }
    return minimum;
}

std::string srtOutputMode(const StreamConfig& cfg) {
    return toLower(cfg.outputMode) == "caller" ? "caller" : "listener";
}

std::string hlsDirectory(const StreamConfig& cfg) {
    std::filesystem::create_directories("/tmp/tvstreamer5-hls/" + cfg.id);
    return "/tmp/tvstreamer5-hls/" + cfg.id;
}

std::string rtmpOutputLocation(const StreamConfig& cfg) {
    const std::string type = normalizedOutputType(cfg);
    const std::string host = cfg.outputHost;
    const std::string hostLower = toLower(host);
    if (hostLower.rfind("rtmp://", 0) == 0 || hostLower.rfind("rtmps://", 0) == 0 ||
        hostLower.rfind("rtmpt://", 0) == 0 || hostLower.rfind("rtmpe://", 0) == 0 ||
        hostLower.rfind("rtmpte://", 0) == 0 || hostLower.rfind("rtmpts://", 0) == 0) {
        return host;
    }
    if (type == "youtube") {
        return "rtmp://a.rtmp.youtube.com/live2/" + host;
    }
    const std::string targetHost = host.empty() ? "127.0.0.1" : host;
    return "rtmp://" + targetHost + ":" + std::to_string(cfg.outputPort) + "/live/" + cfg.id;
}

std::string rtspOutputLocation(const StreamConfig& cfg) {
    const std::string host = cfg.outputHost;
    const std::string hostLower = toLower(host);
    if (hostLower.rfind("rtsp://", 0) == 0 || hostLower.rfind("rtsps://", 0) == 0) {
        return host;
    }
    const std::string targetHost = host.empty() ? "127.0.0.1" : host;
    const int port = cfg.outputPort > 0 ? cfg.outputPort : 8554;
    return "rtsp://" + targetHost + ":" + std::to_string(port) + "/" + cfg.id;
}

uint16_t transcodedHttpInternalPort(const StreamConfig& cfg) {
    const std::string key = cfg.id.empty() ? cfg.name : cfg.id;
    const size_t hash = std::hash<std::string>{}(key);
    return static_cast<uint16_t>(20000 + (hash % 20000));
}

} // namespace tvs::protocols
