#include "protocols/stream/outputs/StreamUdpOutputProtocol.h"

namespace tvs::stream_protocols::outputs {
bool isUdpOutput(const std::string& type) {
    return type == "udp" || type == "udp-cbr" || type == "udp_cbr" || type == "udpcbr" || type == "udp-vbr" || type == "udp_vbr" || type == "udpvbr";
}
}
