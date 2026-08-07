#include "protocols/inputs/GstRtpInputProtocol.h"

#include "utils.h"

namespace tvs::protocols::inputs {

bool isRtpInput(const StreamConfig& cfg) {
    const std::string lower = toLower(cfg.inputUri);
    return lower.rfind("rtp://", 0) == 0 || toLower(cfg.inputMode) == "rtp";
}

std::string rtpInputUri(const StreamConfig& cfg) {
    return cfg.inputUri;
}

} // namespace tvs::protocols::inputs
