#include "protocols/stream/inputs/StreamRtspInputProtocol.h"

namespace tvs::stream_protocols::inputs {
bool isRtspInput(const std::string& input, const std::string& mode, bool testPattern) {
    return input.rfind("rtsp://", 0) == 0 || input.rfind("rtsps://", 0) == 0;
}
}
