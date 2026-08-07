#include "protocols/outputs/GstRtmpOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendRtmpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    args.insert(args.end(), {"flvmux", "name=mux", "streamable=true", "latency=0", "!"});
    appendOutputQueue(args, "transcode_rtmp_output_queue", true);
    args.insert(args.end(), {"rtmpsink", "location=" + rtmpOutputLocation(cfg), "sync=false", "async=false"});
    spec.videoPad = "mux.";
    spec.audioPad = "mux.";
    spec.container = ContainerKind::Flv;
    spec.description = normalizedOutputType(cfg) + "@" + rtmpOutputLocation(cfg);
    return true;
}
}
