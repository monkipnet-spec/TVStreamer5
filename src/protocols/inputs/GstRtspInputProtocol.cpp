#include "protocols/inputs/GstRtspInputProtocol.h"
#include "utils.h"
namespace tvs::protocols::inputs {
bool isRtspInput(const StreamConfig& cfg) {
    const std::string lower = toLower(cfg.inputUri);
    return lower.rfind("rtsp://", 0) == 0 || lower.rfind("rtsps://", 0) == 0;
}
std::string rtspInputUri(const StreamConfig& cfg) { return cfg.inputUri; }
}
