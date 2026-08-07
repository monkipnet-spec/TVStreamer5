#include "protocols/inputs/GstUdpInputProtocol.h"
#include "utils.h"
namespace tvs::protocols::inputs {
bool isUdpInput(const StreamConfig& cfg) {
    return toLower(cfg.inputUri).rfind("udp://", 0) == 0;
}
std::string udpInputUri(const StreamConfig& cfg) { return cfg.inputUri; }
}
