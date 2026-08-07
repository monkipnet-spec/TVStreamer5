#include "protocols/outputs/GstSrtOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"

namespace tvs::protocols::outputs {

bool appendSrtSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendTsSmoother(args, "transcode_srt_ts_smoother", 300000);
    appendOutputQueue(args, "transcode_srt_output_queue", false);

    const std::string mode = srtOutputMode(cfg);
    const bool caller = mode == "caller";
    const int port = (cfg.outputPort > 0 && cfg.outputPort <= 65535) ? cfg.outputPort : 7001;
    const std::string bindHost = (!cfg.interfaceAddress.empty() && cfg.interfaceAddress != "::")
        ? cfg.interfaceAddress
        : "0.0.0.0";
    const std::string targetHost = safeHost(cfg.outputHost, "127.0.0.1");

    // Match the in-process SRT path exactly.  In listener mode the selected
    // output interface is the bind address; outputHost is only the advertised
    // address shown to clients.  Explicitly set both the URI mode and the mode
    // property because Ubuntu 22.04 commonly ships GStreamer 1.20.x and this is
    // more robust than relying only on query-string parsing.
    const std::string uri = caller
        ? "srt://" + targetHost + ":" + std::to_string(port) + "?mode=caller"
        : "srt://" + bindHost + ":" + std::to_string(port) + "?mode=listener";

    args.insert(args.end(), {
        "srtsink",
        "uri=" + uri,
        "mode=" + mode,
        "latency=250",
        "sync=false",
        "async=false",
        "qos=false",
        "max-lateness=-1",
        "blocksize=1316",
        "auto-reconnect=true",
        "wait-for-connection=false",
        "poll-timeout=1000"
    });

    if (caller) {
        if (!cfg.interfaceAddress.empty() && cfg.interfaceAddress != "0.0.0.0" && cfg.interfaceAddress != "::") {
            args.push_back("localaddress=" + cfg.interfaceAddress);
        }
        args.push_back("localport=0");
    } else {
        args.push_back("localaddress=" + bindHost);
        args.push_back("localport=" + std::to_string(port));
    }

    assignTsPads(cfg, spec);
    if (caller) {
        spec.description = "srt-caller@" + uri;
    } else {
        const std::string advertised = safeHost(cfg.outputHost, bindHost);
        spec.description = "srt-listener@srt://" + advertised + ":" + std::to_string(port);
    }
    return true;
}

} // namespace tvs::protocols::outputs
