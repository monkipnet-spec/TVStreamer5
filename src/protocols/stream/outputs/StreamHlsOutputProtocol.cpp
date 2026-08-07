#include "protocols/stream/outputs/StreamHlsOutputProtocol.h"

namespace tvs::stream_protocols::outputs {
bool isHlsOutput(const std::string& type) {
    return type == "hls";
}
}
