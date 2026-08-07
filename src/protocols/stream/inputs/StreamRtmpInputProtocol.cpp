#include "protocols/stream/inputs/StreamRtmpInputProtocol.h"

namespace tvs::stream_protocols::inputs {
bool isRtmpInput(const std::string& input, const std::string& mode, bool testPattern) {
    return input.rfind("rtmp://", 0) == 0 || input.rfind("rtmps://", 0) == 0;
}
}
