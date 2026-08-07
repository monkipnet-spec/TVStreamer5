#include "protocols/outputs/GstHttpOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendHttpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    // Do not bind the public TVStreamer HTTP port here. gst-launch exposes an
    // internal localhost MPEG-TS TCP port; HttpServer proxies /stream/<id>.ts
    // to this endpoint and adds proper HTTP headers for clients.
    appendOutputQueue(args, "transcode_http_output_queue", false);
    const int internalPort = static_cast<int>(transcodedHttpInternalPort(cfg));
    args.insert(args.end(), {
        "tcpserversink",
        "host=127.0.0.1",
        "port=" + std::to_string(internalPort),
        "sync=false",
        "async=false"
    });
    assignTsPads(cfg, spec);
    spec.description = "http-ts@http-port:" + std::to_string(cfg.outputPort) + "->tcp://127.0.0.1:" + std::to_string(internalPort);
    return true;
}
}
