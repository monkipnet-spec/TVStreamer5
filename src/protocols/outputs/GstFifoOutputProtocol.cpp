#include "protocols/outputs/GstFifoOutputProtocol.h"

#include "protocols/GstProtocolTypes.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"

namespace tvs::protocols::outputs {

bool appendFifoSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendTsSmoother(args, "transcode_fifo_ts_smoother", 500000);
    appendCbrPacer(args, cfg, "transcode_fifo_cbr_pacer");
    appendOutputQueue(args, "transcode_fifo_output_queue", false);
    const std::string location = cfg.outputHost.empty() ? transcodedFifoRelayPath(cfg) : cfg.outputHost;
    args.insert(args.end(), {
        "filesink",
        "location=" + location,
        "sync=false",
        "async=false"
    });
    assignTsPads(cfg, spec);
    spec.description = "fifo-relay@file://" + location;
    return true;
}

} // namespace tvs::protocols::outputs
