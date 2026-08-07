#include "protocols/outputs/GstRtspOutputProtocol.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendRtspSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    args.insert(args.end(), {
        "rtspclientsink",
        "name=mux",
        "location=" + rtspOutputLocation(cfg),
        "protocols=tcp",
        "latency=200"
    });
    spec.videoPad = "mux.";
    spec.audioPad = "mux.";
    spec.container = ContainerKind::Rtsp;
    spec.description = "rtsp@" + rtspOutputLocation(cfg);
    return true;
}
}
