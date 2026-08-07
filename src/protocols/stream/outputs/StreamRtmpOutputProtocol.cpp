#include "protocols/stream/outputs/StreamRtmpOutputProtocol.h"

namespace tvs::stream_protocols::outputs {
bool isRtmpOutput(const std::string& type) {
    return type == "rtmp" || type == "youtube";
}
}
