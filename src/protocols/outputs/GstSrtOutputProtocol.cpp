#include "protocols/outputs/GstSrtOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendSrtSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendOutputQueue(args, "transcode_srt_output_queue", false);
    const std::string mode = srtOutputMode(cfg);
    const bool caller = mode == "caller";
    const std::string host = caller ? safeHost(cfg.outputHost, "127.0.0.1") : listenerBindHost(cfg);
    const std::string uri = "srt://" + host + ":" + std::to_string(cfg.outputPort) + "?mode=" + mode;
    args.insert(args.end(), {"srtsink", "uri=" + uri, "sync=false", "async=false"});
    assignTsPads(cfg, spec);
    spec.description = "srt@" + uri;
    return true;
}
}
