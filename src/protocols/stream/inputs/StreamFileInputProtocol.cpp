#include "protocols/stream/inputs/StreamFileInputProtocol.h"

namespace tvs::stream_protocols::inputs {
bool isFileInput(const std::string& input, const std::string& mode, bool testPattern) {
    return input.rfind("file://", 0) == 0 || input.find("://") == std::string::npos;
}
}
