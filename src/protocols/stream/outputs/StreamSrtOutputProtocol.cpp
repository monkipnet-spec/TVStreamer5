#include "protocols/stream/outputs/StreamSrtOutputProtocol.h"

namespace tvs::stream_protocols::outputs {
bool isSrtOutput(const std::string& type) {
    return type == "srt";
}
}
