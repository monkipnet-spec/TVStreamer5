#include "protocols/stream/inputs/StreamRtpInputProtocol.h"

namespace tvs::stream_protocols::inputs {
bool isRtpInput(const std::string& input, const std::string& mode, bool testPattern) {
    return input.rfind("rtp://", 0) == 0;
}
}
