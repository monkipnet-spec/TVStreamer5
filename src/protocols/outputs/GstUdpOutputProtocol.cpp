#include "protocols/outputs/GstUdpOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendUdpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendTsSmoother(args, "transcode_udp_ts_smoother", 250000);
    appendOutputQueue(args, "transcode_udp_output_queue", false);
    args.insert(args.end(), {
        "udpsink",
        "host=" + safeHost(cfg.outputHost, "127.0.0.1"),
        "port=" + std::to_string(cfg.outputPort),
        "sync=true",
        "async=false",
        "qos=false",
        "auto-multicast=true",
        "ttl-mc=32",
        "buffer-size=8388608"
    });
    if (!cfg.interfaceAddress.empty()) args.push_back("bind-address=" + cfg.interfaceAddress);
    assignTsPads(cfg, spec);
    spec.description = normalizedOutputType(cfg) + "@udp://" + safeHost(cfg.outputHost, "127.0.0.1") + ":" + std::to_string(cfg.outputPort);
    return true;
}
}
