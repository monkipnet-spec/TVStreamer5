#include "protocols/outputs/GstOutputProtocolUtils.h"

#include "protocols/GstProtocolTypes.h"

namespace tvs::protocols::outputs {

void addQueue(std::vector<std::string>& args, const std::string& name, uint64_t maxTimeNs, bool leakyDownstream) {
    args.insert(args.end(), {
        "queue",
        "name=" + name,
        "max-size-buffers=0",
        "max-size-bytes=0",
        "max-size-time=" + std::to_string(maxTimeNs)
    });
    if (leakyDownstream) {
        args.push_back("leaky=downstream");
    }
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
        "pcr-interval=1800",
        "si-interval=9000",
        "!"
   });
}

void appendOutputQueue(std::vector<std::string>& args, const std::string& name, bool leakyDownstream) {
    addQueue(args, name, leakyDownstream ? 2000000000ULL : 3000000000ULL, leakyDownstream);
    args.push_back("!");
}

void assignTsPads(const StreamConfig& cfg, GstOutputSpec& spec) {
    spec.videoPad = cfg.videoPid > 0 ? "mux.sink_" + std::to_string(cfg.videoPid) : "mux.";
    spec.audioPad = cfg.audioPid > 0 ? "mux.sink_" + std::to_string(cfg.audioPid) : "mux.";
}

} // namespace tvs::protocols::outputs
