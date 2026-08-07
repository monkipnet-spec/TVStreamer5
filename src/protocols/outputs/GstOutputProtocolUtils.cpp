#include "protocols/outputs/GstOutputProtocolUtils.h"

#include "protocols/GstProtocolTypes.h"

#include <algorithm>

namespace tvs::protocols::outputs {

namespace {

uint32_t effectiveVideoPid(const StreamConfig& cfg) {
    return cfg.videoPid > 0 ? cfg.videoPid : 258;
}

uint32_t effectiveAudioPid(const StreamConfig& cfg) {
    return cfg.audioPid > 0 ? cfg.audioPid : 257;
}

uint32_t effectiveServiceId(const StreamConfig& cfg) {
    return std::max<uint32_t>(cfg.serviceId, 1);
}

} // namespace

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
        "pat-interval=9000",
        "pmt-interval=9000",
        "pcr-interval=1800",
        "si-interval=9000"
    });

    // CBR null-packet padding is only useful for UDP-CBR.  Applying a fixed
    // muxrate to SRT/HTTP/HLS/RTP/UDP-VBR makes file/TCP/SRT delivery burstier
    // and can create stalls on receivers after transcoding.
    if (outputKind(cfg) == OutputKind::UdpCbr) {
        args.push_back("bitrate=" + std::to_string(muxBitrate(cfg)));
    }

    // Remap belongs to the MPEG-TS mux, not to the network sink.  Requesting
    // sink_<PID> pads fixes the elementary PIDs.  prog-map then places both
    // streams in the configured service/program so HLS gets the same remap as
    // UDP/SRT/HTTP/RTP.
    if (cfg.remapEnabled) {
        const uint32_t videoPid = effectiveVideoPid(cfg);
        const uint32_t audioPid = effectiveAudioPid(cfg);
        const uint32_t serviceId = effectiveServiceId(cfg);
        args.push_back(
            "prog-map=program_map,sink_" + std::to_string(videoPid) + "=" + std::to_string(serviceId) +
            ",sink_" + std::to_string(audioPid) + "=" + std::to_string(serviceId));
    }

    args.push_back("!");
}

void appendTsSmoother(std::vector<std::string>& args, const std::string& name, uint32_t smoothingLatencyUs) {
    args.insert(args.end(), {
        "tsparse",
        "name=" + name,
        "set-timestamps=true",
        "smoothing-latency=" + std::to_string(smoothingLatencyUs),
        "alignment=7",
        "!"
    });
}

void appendOutputQueue(std::vector<std::string>& args, const std::string& name, bool leakyDownstream) {
    addQueue(args, name, leakyDownstream ? 2000000000ULL : 5000000000ULL, leakyDownstream);
    args.push_back("!");
}

void assignTsPads(const StreamConfig& cfg, GstOutputSpec& spec) {
    if (!cfg.remapEnabled) {
        spec.videoPad = "mux.";
        spec.audioPad = "mux.";
        return;
    }

    spec.videoPad = "mux.sink_" + std::to_string(effectiveVideoPid(cfg));
    spec.audioPad = "mux.sink_" + std::to_string(effectiveAudioPid(cfg));
}

} // namespace tvs::protocols::outputs
