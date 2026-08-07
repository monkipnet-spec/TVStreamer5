#include "protocols/inputs/GstHlsInputProtocol.h"
#include "utils.h"
namespace tvs::protocols::inputs {
bool isHlsInput(const StreamConfig& cfg) {
    const std::string lower = toLower(cfg.inputUri);
    return lower.rfind("hls://", 0) == 0 || lower.find(".m3u8") != std::string::npos || toLower(cfg.inputMode) == "hls";
}
std::string hlsInputUri(const StreamConfig& cfg) {
    if (toLower(cfg.inputUri).rfind("hls://", 0) == 0) {
        return "http://" + cfg.inputUri.substr(6);
    }
    return cfg.inputUri;
}
}
