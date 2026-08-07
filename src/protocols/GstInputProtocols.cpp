#include "protocols/GstInputProtocols.h"

#include "protocols/inputs/GstHttpInputProtocol.h"
#include "protocols/inputs/GstHlsInputProtocol.h"
#include "protocols/inputs/GstRtmpInputProtocol.h"
#include "protocols/inputs/GstRtspInputProtocol.h"
#include "protocols/inputs/GstSrtInputProtocol.h"
#include "protocols/inputs/GstUdpInputProtocol.h"

namespace tvs::protocols {

std::string inputUriForGstreamer(const StreamConfig& cfg) {
    if (inputs::isHlsInput(cfg)) return inputs::hlsInputUri(cfg);
    if (inputs::isSrtInput(cfg)) return inputs::srtInputUri(cfg);
    if (inputs::isRtspInput(cfg)) return inputs::rtspInputUri(cfg);
    if (inputs::isRtmpInput(cfg)) return inputs::rtmpInputUri(cfg);
    if (inputs::isUdpInput(cfg)) return inputs::udpInputUri(cfg);
    if (inputs::isHttpInput(cfg)) return inputs::httpInputUri(cfg);
    return cfg.inputUri;
}

void appendDecodeInput(std::vector<std::string>& args, const StreamConfig& cfg) {
    args.insert(args.end(), {
        "uridecodebin",
        "name=dec",
        "uri=" + inputUriForGstreamer(cfg),
        "use-buffering=true"
    });
}

std::vector<std::string> requiredInputElements() {
    return {"uridecodebin"};
}

} // namespace tvs::protocols
