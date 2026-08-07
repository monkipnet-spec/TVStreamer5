#include "protocols/outputs/GstHttpOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendHttpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendTsSmoother(args, "transcode_http_ts_smoother", 250000);
    appendOutputQueue(args, "transcode_http_output_queue", false);
    const int internalPort = static_cast<int>(transcodedHttpInternalPort(cfg));
    args.insert(args.end(), {
        "tcpserversink",
        "host=127.0.0.1",
        "port=" + std::to_string(internalPort),
        "sync=true",
        "async=false",
        "qos=false"
    });
    assignTsPads(cfg, spec);
    spec.description = "http-ts@http-port:" + std::to_string(cfg.outputPort) + "->tcp://127.0.0.1:" + std::to_string(internalPort);
    return true;
}
}
