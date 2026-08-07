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
    const int port = cfg.outputPort > 0 ? cfg.outputPort : 7001;

    // Keep the external transcoder SRT semantics identical to the proven
    // in-process StreamManager path.  outputHost is the remote/advertised
    // address; it must never become the bind address for listener mode.
    // GStreamer documents srt://:PORT as the listener form.  Bind a selected
    // local interface explicitly through localaddress instead.
    const std::string uri = caller
        ? "srt://" + safeHost(cfg.outputHost, "127.0.0.1") + ":" + std::to_string(port) + "?mode=caller"
        : "srt://:" + std::to_string(port) + "?mode=listener";

    args.insert(args.end(), {
        "srtsink",
        "uri=" + uri,
        "latency=250",
        "sync=false",
        "async=false",
        "qos=false",
        "max-lateness=-1",
        "blocksize=1316",
        "wait-for-connection=false",
        "poll-timeout=1000"
    });

    if (!cfg.interfaceAddress.empty() && cfg.interfaceAddress != "0.0.0.0" && cfg.interfaceAddress != "::") {
        args.push_back("localaddress=" + cfg.interfaceAddress);
    }
    if (caller) {
        args.push_back("localport=0");
    } else {
        args.push_back("localport=" + std::to_string(port));
    }

    assignTsPads(cfg, spec);
    if (caller) {
        spec.description = "srt-caller@" + uri;
    } else {
        const std::string advertised = safeHost(cfg.outputHost, safeHost(cfg.interfaceAddress, "0.0.0.0"));
        spec.description = "srt-listener@srt://" + advertised + ":" + std::to_string(port);
    }
    return true;
}

} // namespace tvs::protocols::outputs
