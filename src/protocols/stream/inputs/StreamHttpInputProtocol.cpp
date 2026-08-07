#include "protocols/stream/inputs/StreamHttpInputProtocol.h"

namespace tvs::stream_protocols::inputs {
bool isHttpInput(const std::string& input, const std::string& mode, bool testPattern) {
    return input.rfind("http://", 0) == 0 || input.rfind("https://", 0) == 0;
}
}
