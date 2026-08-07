#include "protocols/stream/outputs/StreamHttpOutputProtocol.h"

namespace tvs::stream_protocols::outputs {
bool isHttpOutput(const std::string& type) {
    return type == "http";
}
}
