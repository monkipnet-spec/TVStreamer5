#include "protocols/inputs/GstHttpInputProtocol.h"
#include "utils.h"
namespace tvs::protocols::inputs {
bool isHttpInput(const StreamConfig& cfg) {
    const std::string lower = toLower(cfg.inputUri);
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}
std::string httpInputUri(const StreamConfig& cfg) { return cfg.inputUri; }
}
