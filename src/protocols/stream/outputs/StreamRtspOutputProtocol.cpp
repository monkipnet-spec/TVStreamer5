#include "protocols/stream/outputs/StreamRtspOutputProtocol.h"

namespace tvs::stream_protocols::outputs {
bool isRtspOutput(const std::string& type) {
    return type == "rtsp";
}
}
