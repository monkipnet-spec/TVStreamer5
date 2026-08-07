#include "protocols/stream/outputs/StreamRtpOutputProtocol.h"

namespace tvs::stream_protocols::outputs {
bool isRtpOutput(const std::string& type) {
    return type == "rtp";
}
}
