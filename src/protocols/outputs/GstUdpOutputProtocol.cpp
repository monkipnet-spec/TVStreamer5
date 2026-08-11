#include "protocols/outputs/GstUdpOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendUdpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    const bool internalRelay = cfg.outputMode == "internal-relay";
    const std::size_t muxStart = args.size();
    appendMpegTsMux(args, cfg);

    if (internalRelay) {
        for (std::size_t i = muxStart; i < args.size(); ++i) {
            if (args[i].rfind("bitrate=", 0) == 0) {
                args[i] = "bitrate=0";
                break;
            }
        }
        appendOutputQueue(args, "transcode_internal_udp_queue", false);
    } else {
        appendTsSmoother(args, "transcode_udp_ts_smoother", 300000);
        appendCbrPacer(args, cfg, "transcode_udp_cbr_pacer");
        appendOutputQueue(args, "transcode_udp_output_queue", false);
    }
    args.insert(args.end(), {
        "udpsink",
        "host=" + safeHost(cfg.outputHost, "127.0.0.1"),
        "port=" + std::to_string(cfg.outputPort),
        "sync=false",
        "async=false",
        "qos=false",
        "auto-multicast=true",
        "ttl-mc=32",
        "buffer-size=8388608"
    });
    if (!cfg.interfaceAddress.empty()) args.push_back("bind-address=" + cfg.interfaceAddress);
    assignTsPads(cfg, spec);
    spec.description = (internalRelay ? "internal-raw-ts" : normalizedOutputType(cfg)) + std::string("@udp://") + safeHost(cfg.outputHost, "127.0.0.1") + ":" + std::to_string(cfg.outputPort);
    return true;
}
}
