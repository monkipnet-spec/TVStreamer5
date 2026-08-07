#include "protocols/outputs/GstRtpOutputProtocol.h"

#include "protocols/GstProtocolTypes.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"

namespace tvs::protocols::outputs {

bool appendRtpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendOutputQueue(args, "transcode_rtp_output_queue", false);
    args.insert(args.end(), {
        "rtpmp2tpay",
        "!",
        "udpsink",
        "host=" + safeHost(cfg.outputHost, "127.0.0.1"),
        "port=" + std::to_string(cfg.outputPort),
        "sync=true",
        "async=false",
        "auto-multicast=true",
        "ttl-mc=32",
        "buffer-size=4194304"
    });
    if (!cfg.interfaceAddress.empty()) args.push_back("bind-address=" + cfg.interfaceAddress);
    assignTsPads(cfg, spec);
    spec.description = "rtp-mpegts@rtp://" + safeHost(cfg.outputHost, "127.0.0.1") + ":" + std::to_string(cfg.outputPort);
    return true;
}

} // namespace tvs::protocols::outputs
