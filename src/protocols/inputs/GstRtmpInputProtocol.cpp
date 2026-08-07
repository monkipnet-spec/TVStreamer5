#include "protocols/inputs/GstRtmpInputProtocol.h"
#include "utils.h"
namespace tvs::protocols::inputs {
bool isRtmpInput(const StreamConfig& cfg) {
    const std::string lower = toLower(cfg.inputUri);
    return lower.rfind("rtmp://", 0) == 0 || lower.rfind("rtmps://", 0) == 0;
}
std::string rtmpInputUri(const StreamConfig& cfg) { return cfg.inputUri; }
}
